#include "gatewayServer.hpp"
#include "gatewayServiceImpl.hpp"

#include <BreezeEtcd.hpp>
#include <BreezeLog.hpp>
#include <BreezeRPC.hpp>
#include <httplib.h>
#include <sys/socket.h>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace GatewayService
{
    namespace
    {
        constexpr int kEventFd = 3;
        constexpr std::size_t kMaxEventSize = 4096;
        using Nodes = std::map<std::string, std::set<std::string>>;

        // 文件描述符随对象销毁关闭，线程创建失败时也能释放资源。
        class FileDescriptor
        {
        public:
            explicit FileDescriptor(int fd = -1) : _fd(fd) {}
            ~FileDescriptor()
            {
                if (_fd >= 0)
                    close(_fd);
            }
            FileDescriptor(const FileDescriptor &) = delete;
            FileDescriptor &operator=(const FileDescriptor &) = delete;
            FileDescriptor(FileDescriptor &&other) noexcept : _fd(std::exchange(other._fd, -1)) {}
            FileDescriptor &operator=(FileDescriptor &&other) noexcept
            {
                if (_fd >= 0)
                    close(_fd);
                _fd = std::exchange(other._fd, -1);
                return *this;
            }
            int get() const { return _fd; }

        private:
            int _fd;
        };

        // SOCK_SEQPACKET 保留消息边界，一次发送对应一次接收。
        void SendEvent(char type, const std::string &name, const std::string &address)
        {
            std::string message(1, type);
            message += name;
            message += '\0';
            message += address;
            if (message.size() > kMaxEventSize)
            {
                WRN("服务发现事件过长: {}", name);
                return;
            }
            if (send(kEventFd, message.data(), message.size(), MSG_NOSIGNAL) < 0)
                ERR("发送服务发现事件失败: {}", std::strerror(errno));
        }
    }

    int RunDiscoveryWorker(const std::string &etcd_address, const std::vector<std::string> &service_names, int parent_pid)
    {
        // 原版 SvcWatcher 无法取消；父进程退出时内核负责终止子进程。
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_pid)
            return 1;
        const std::set<std::string> names(service_names.begin(), service_names.end());
        Nodes nodes;
        std::mutex nodes_mutex;
        auto update = [&](char type, const std::string &name, const std::string &address)
        {
            if (names.count(name) == 0 || address.empty())
                return;
            std::lock_guard<std::mutex> lock(nodes_mutex);
            auto &addresses = nodes[name];
            const bool changed = type == '+' ? addresses.insert(address).second : addresses.erase(address) != 0;
            if (changed)
                SendEvent(type, name, address);
        };
        SvcWatcher watcher(
            etcd_address,
            [&](std::string name, std::string address)
            { update('+', name, address); },
            [&](std::string name, std::string address)
            { update('-', name, address); });

        // watch() 在连接失败时会阻塞，因此在线程中执行并保留 watcher 的生命周期。
        bool started = false;
        std::thread watch_thread([&]
                                 { started = watcher.watch(); });
        watch_thread.join();
        if (!started)
            return 1;

        etcd::Client client(etcd_address);
        client.set_grpc_timeout(std::chrono::seconds(1));
        for (;;)
        {
            const auto response = client.ls("/").get();
            if (response.is_ok())
            {
                Nodes snapshot;
                for (const auto &value : response.values())
                {
                    const std::string key = value.key();
                    const auto slash = key.find('/', 1);
                    if (key.empty() || key[0] != '/' || slash == std::string::npos ||
                        slash + 1 == key.size() || key.find('/', slash + 1) != std::string::npos)
                        continue;
                    const auto name = key.substr(1, slash - 1);
                    if (names.count(name) && !value.as_string().empty())
                        snapshot[name].insert(value.as_string());
                }

                // 补齐原版监听器读取快照与开始监听之间遗漏的节点变化。
                std::lock_guard<std::mutex> lock(nodes_mutex);
                for (const auto &service : nodes)
                    for (const auto &address : service.second)
                        if (snapshot[service.first].count(address) == 0)
                            SendEvent('-', service.first, address);
                for (const auto &service : snapshot)
                    for (const auto &address : service.second)
                        if (nodes[service.first].count(address) == 0)
                            SendEvent('+', service.first, address);
                nodes = std::move(snapshot);
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    class DiscoveryProcess
    {
    public:
        DiscoveryProcess(std::string etcd_address, std::vector<std::string> service_names,
                         std::shared_ptr<SvcChannels> channels)
            : _etcd_address(std::move(etcd_address)), _service_names(std::move(service_names)),
              _channels(std::move(channels)), _stop_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
        {
            if (_stop_fd.get() < 0)
                throw std::runtime_error("创建服务发现退出通道失败");
            _thread = std::thread(&DiscoveryProcess::Run, this);
        }

        ~DiscoveryProcess()
        {
            const std::uint64_t stop = 1;
            while (write(_stop_fd.get(), &stop, sizeof(stop)) < 0 && errno == EINTR)
            {
            }
            _thread.join();
            INF("网关服务发现已停止");
        }

    private:
        pid_t Spawn(FileDescriptor &receiver)
        {
            int sockets[2];
            if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
            {
                ERR("创建服务发现事件通道失败: {}", std::strerror(errno));
                return -1;
            }
            FileDescriptor parent(sockets[0]);
            FileDescriptor child(sockets[1]);
            // 固定子进程通道为 3，并避免与 socketpair 返回的描述符冲突。
            FileDescriptor child_fd(fcntl(child.get(), F_DUPFD_CLOEXEC, kEventFd + 1));
            if (child_fd.get() < 0)
            {
                ERR("复制服务发现事件通道失败: {}", std::strerror(errno));
                return -1;
            }

            std::vector<std::string> args{"/proc/self/exe", "--gateway-discovery-worker", _etcd_address,
                                          std::to_string(getpid())};
            args.insert(args.end(), _service_names.begin(), _service_names.end());
            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            posix_spawn_file_actions_t actions;
            int error = posix_spawn_file_actions_init(&actions);
            if (error != 0)
            {
                ERR("初始化服务发现进程失败: {}", std::strerror(error));
                return -1;
            }
            error = posix_spawn_file_actions_addclose(&actions, parent.get());
            if (error == 0)
                error = posix_spawn_file_actions_adddup2(&actions, child_fd.get(), kEventFd);
            if (error == 0)
                error = posix_spawn_file_actions_addclose(&actions, child_fd.get());
            pid_t pid = -1;
            if (error == 0)
                error = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (error != 0)
            {
                ERR("启动服务发现进程失败: {}", std::strerror(error));
                return -1;
            }
            receiver = std::move(parent);
            return pid;
        }

        void ApplyEvent(const std::string &message)
        {
            // 消息由本进程启动的子进程生成：类型、服务名、零字节、地址。
            const auto separator = message.find('\0', 1);
            const auto name = message.substr(1, separator - 1);
            const auto address = message.substr(separator + 1);
            if (message[0] == '+')
            {
                INF("服务上线 {} {}", name, address);
                _channels->addNode(name, address);
                _nodes[name].insert(address);
            }
            else
            {
                INF("服务下线 {} {}", name, address);
                _channels->delNode(name, address);
                _nodes[name].erase(address);
            }
        }

        bool WaitForStop(int timeout)
        {
            pollfd fd{_stop_fd.get(), POLLIN, 0};
            int result;
            do
            {
                result = poll(&fd, 1, timeout);
            } while (result < 0 && errno == EINTR);
            return result != 0;
        }

        void Run()
        {
            INF("启动服务发现");
            for (;;)
            {
                FileDescriptor receiver;
                const pid_t pid = Spawn(receiver);
                if (pid < 0)
                {
                    if (WaitForStop(1000))
                        return;
                    continue;
                }

                pollfd fds[] = {{_stop_fd.get(), POLLIN, 0}, {receiver.get(), POLLIN, 0}};
                bool stopping = false;
                for (;;)
                {
                    const int ready = poll(fds, 2, -1);
                    if (ready < 0 && errno == EINTR)
                        continue;
                    if (ready < 0 || fds[0].revents != 0)
                    {
                        stopping = true;
                        break;
                    }
                    if (fds[1].revents != 0)
                    {
                        char message[kMaxEventSize];
                        const ssize_t length = recv(receiver.get(), message, sizeof(message), 0);
                        if (length < 0 && errno == EINTR)
                            continue;
                        if (length <= 0)
                            break;
                        ApplyEvent(std::string(message, static_cast<std::size_t>(length)));
                    }
                }
                if (stopping)
                    kill(pid, SIGTERM);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
                {
                }
                if (stopping)
                    return;

                // 重启时清除旧通道，由新监听器的快照重新填充。
                for (const auto &service : _nodes)
                    for (const auto &address : service.second)
                        _channels->delNode(service.first, address);
                _nodes.clear();
                WRN("服务发现进程已退出，正在重新启动");
                if (WaitForStop(1000))
                    return;
            }
        }

        std::string _etcd_address;
        std::vector<std::string> _service_names;
        std::shared_ptr<SvcChannels> _channels;
        Nodes _nodes;
        FileDescriptor _stop_fd;
        std::thread _thread;
    };

    GatewayServer::GatewayServer(std::string address, int port, std::unique_ptr<httplib::Server> server,
                                 std::shared_ptr<GatewayServiceImpl> service,
                                 std::unique_ptr<DiscoveryProcess> discovery)
        : _address(std::move(address)), _port(port), _server(std::move(server)),
          _service(std::move(service)), _discovery(std::move(discovery))
    {
        INF("创建网关服务器 {}:{}", _address, _port);
    }

    GatewayServer::~GatewayServer()
    {
        stop();
    }

    bool GatewayServer::start()
    {
        if (_server->is_running())
        {
            INF("网关服务器已经运行");
            return true;
        }
        if (_server_thread.joinable())
            _server_thread.join();
        if (!_server->bind_to_port(_address, _port))
        {
            ERR("网关监听地址绑定失败 {}:{}", _address, _port);
            return false;
        }
        _listener_finished.store(false);
        _server_thread = std::thread([this]
                                     {
            const bool success = _server->listen_after_bind();
            _listener_finished.store(true);
            if (!success)
                ERR("网关监听失败"); });
        while (!_server->is_running() && !_listener_finished.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!_server->is_running())
        {
            _server_thread.join();
            return false;
        }
        INF("网关服务器已启动 {}:{}", _address, _port);
        return true;
    }

    void GatewayServer::stop()
    {
        DBG("检查网关服务器停止状态");
        if (_server->is_running())
        {
            INF("停止网关服务器");
            _server->stop();
        }
        if (_server_thread.joinable())
            _server_thread.join();
    }

    bool GatewayServer::isRunning() const
    {
        DBG("检查网关服务器运行状态");
        return _server->is_running();
    }

    GatewayServerBuilder::GatewayServerBuilder(std::string address, int port, std::string etcd_address,
                                               std::vector<std::string> service_names)
        : _address(std::move(address)), _port(port), _etcd_address(std::move(etcd_address)),
          _service_names(std::move(service_names))
    {
        INF("配置网关服务器构建器");
    }

    std::unique_ptr<GatewayServer> GatewayServerBuilder::Build() const
    {
        INF("构建网关服务器");
        auto channels = std::make_shared<SvcChannels>();
        const std::set<std::string> names(_service_names.begin(), _service_names.end());
        for (const auto &name : names)
            channels->setWatch(name);

        auto discovery = std::make_unique<DiscoveryProcess>(_etcd_address, _service_names, channels);
        auto service = std::make_shared<GatewayServiceImpl>(channels);
        auto http = std::make_unique<httplib::Server>();
        // 允许重启后复用地址，同时保证端口被占用时报告监听失败。
        http->set_socket_options([](socket_t socket)
                                 {
            const int enabled = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)); });
        http->set_read_timeout(300, 0);
        http->set_write_timeout(300, 0);
        service->BindRoutes(*http);
        return std::unique_ptr<GatewayServer>(
            new GatewayServer(_address, _port, std::move(http), std::move(service), std::move(discovery)));
    }
}
