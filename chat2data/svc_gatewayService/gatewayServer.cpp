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

extern char **environ; // 传递给 posix_spawn 的环境变量

namespace GatewayService
{
    namespace
    {
        constexpr int kEventFd = 3;                                 // 服务发现事件通道的固定文件描述符，避免与 socketpair 返回的描述符冲突。
        constexpr std::size_t kMaxEventSize = 4096;                 // 服务发现事件的最大长度，避免缓冲区溢出。
        using Nodes = std::map<std::string, std::set<std::string>>; // 服务名称到地址集合的映射，用于记录当前在线的服务节点。

        // 文件描述符随对象销毁关闭，使用 RAII 管理资源，避免泄漏。
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
            FileDescriptor(FileDescriptor &&other) noexcept : _fd(std::exchange(other._fd, -1)) {} // 移动构造函数，转移所有权
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
            // 向指定的kEventFd发送服务发现事件，格式为：type + name + '\0' + address并且不能超过 kMaxEventSize，避免缓冲区溢出。
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
        std::mutex nodes_mutex; // 保护 nodes 的互斥锁，避免多线程访问冲突
        auto update = [&](char type, const std::string &name, const std::string &address)
        {
            if (names.count(name) == 0 || address.empty())
                return;
            std::lock_guard<std::mutex> lock(nodes_mutex);
            auto &addresses = nodes[name];
            // type 为 '+' 表示新增节点，为 '-' 表示删除节点，更新 nodes 并发送事件。
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
        client.set_grpc_timeout(std::chrono::seconds(1)); // 设置 bRPC 超时时间为 1 秒，避免阻塞过久
        for (;;)
        {
            const auto response = client.ls("/").get(); // 获取 etcd 根目录下的所有键值对
            if (response.is_ok())
            {
                Nodes snapshot; // 此次轮询的快照，用于补齐原版监听器读取快照与开始监听之间遗漏的节点变化。
                for (const auto &value : response.values())
                {
                    const std::string key = value.key();
                    const auto slash = key.find('/', 1); // 查找第二个斜杠，确保键格式为 /service_name/address
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
                        if (snapshot[service.first].count(address) == 0) // 如果快照中没有该节点，说明它已经下线，发送 '-' 事件
                            SendEvent('-', service.first, address);
                for (const auto &service : snapshot)
                    for (const auto &address : service.second)
                        if (nodes[service.first].count(address) == 0) // 如果当前在线的服务节点快照中没有该节点，说明它已经上线，发送 '+' 事件
                            SendEvent('+', service.first, address);
                nodes = std::move(snapshot); // 更新当前在线的服务节点快照
            }
            std::this_thread::sleep_for(std::chrono::seconds(2)); // 每隔 2 秒轮询一次 etcd
        }
    }

    class DiscoveryProcess // 在网关主进程中运行服务发现的子进程，负责监听 etcd 服务节点变化并通知网关更新 RPC 信道。
    {
    public:
        DiscoveryProcess(std::string etcd_address, std::vector<std::string> service_names,
                         std::shared_ptr<SvcChannels> channels)
            : _etcd_address(std::move(etcd_address)), _service_names(std::move(service_names)),
              _channels(std::move(channels)), _stop_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) // 创建通知退出的事件 fd，非阻塞
        {
            if (_stop_fd.get() < 0)
                throw std::runtime_error("创建服务发现退出通道失败");
            _thread = std::thread(&DiscoveryProcess::Run, this); // 启动监听线程，负责启动子进程并处理服务发现事件
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
        // 在子进程中启动服务发现监听器，使用 socketpair 创建事件通道，父进程通过该通道接收服务节点变化事件。
        pid_t Spawn(FileDescriptor &receiver)
        {
            // sockets[0]：父进程侧端点；sockets[1]：子进程侧端点，二者互联，组成一条双向通信通道
            int sockets[2];
            // 创建一对 UNIX 域套接字 AF_UNIX：unix 本地域；SOCK_SEQPACKET：有序报文，保留消息边界；SOCK_CLOEXEC：exec 时自动关闭，防止 fd 泄漏到新程序
            if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0)
            {
                ERR("创建服务发现事件通道失败: {}", std::strerror(errno));
                return -1;
            }
            // RAII 对象接管管道两端的裸 fd
            FileDescriptor parent(sockets[0]); // 父进程持有这一端，后续交给调用方 receiver
            FileDescriptor child(sockets[1]);  // 原始子进程端 socket

            // F_DUPFD_CLOEXEC：复制 child 的 fd，要求新分配 fd >= kEventFd + 1，同时带上 O_CLOEXEC
            // 避免源 fd 恰好等于 kEventFd，最后close将通信用的 fd 关闭
            FileDescriptor child_fd(fcntl(child.get(), F_DUPFD_CLOEXEC, kEventFd + 1));
            if (child_fd.get() < 0)
            {
                ERR("复制服务发现事件通道失败: {}", std::strerror(errno));
                return -1;
            }

            // 构造子进程启动参数：复用当前可执行文件，通过标记 --gateway-discovery-worker 进入 worker 分支
            // 参数依次：程序路径、worker 标记、etcd 地址、父进程 PID、待监听的所有服务名
            std::vector<std::string> args{"/proc/self/exe", "--gateway-discovery-worker", _etcd_address, std::to_string(getpid())};
            args.insert(args.end(), _service_names.begin(), _service_names.end());
            // posix_spawn 需要 char* 数组，把 string 的底层指针转存，末尾必须 nullptr
            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            // posix_spawn_file_actions_t：spawn 专用动作列表，在 exec 执行前批量执行 fd 操作
            posix_spawn_file_actions_t actions;
            int error = posix_spawn_file_actions_init(&actions);
            if (error != 0)
            {
                ERR("初始化服务发现进程失败: {}", std::strerror(error));
                return -1;
            }
            // 动作 1：子进程启动前关闭父进程那一端 socket，子进程不需要该端点，防止 fd 泄漏、管道无法正常 EOF
            error = posix_spawn_file_actions_addclose(&actions, parent.get());
            // 动作 2：dup2，把复制出来的 child_fd 安装到固定编号 kEventFd，worker 直接读这个固定 fd 和父通信
            if (error == 0)
                error = posix_spawn_file_actions_adddup2(&actions, child_fd.get(), kEventFd);
            // 动作 3：dup2 完成后，关闭临时源 fd child_fd，只保留目标 fd kEventFd
            if (error == 0)
                error = posix_spawn_file_actions_addclose(&actions, child_fd.get());

            pid_t pid = -1;
            // posix_spawn：fork + exec，使用预先编排好的 fd 动作，继承当前环境变量 environ
            if (error == 0)
                error = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            // 销毁动作结构体，释放内存，必须执行，否则内存泄漏
            posix_spawn_file_actions_destroy(&actions);

            if (error != 0)
            {
                ERR("启动服务发现进程失败: {}", std::strerror(error));
                return -1;
            }
            // 移动赋值：把 parent（父侧通信 socket）所有权转移给出参 receiver
            // receiver 如果之前持有 fd，会先 close
            receiver = std::move(parent);
            // 返回 worker 子进程 pid，调用方可用于 waitpid / 监控子进程存活
            return pid;
        }

        // 处理服务发现事件，更新服务信道管理对象
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
            // _stop_fd：用于通知停止的事件 fd / pipe 读端；只关心是否有可读事件 POLLIN
            pollfd fd{_stop_fd.get(), POLLIN, 0};
            int result;
            do
            {
                // poll：同步等待事件就绪；1 表示只监听这一个 fd，返回值：>0 有就绪事件；0 超时；<0 出错
                result = poll(&fd, 1, timeout);
                // EINTR：系统调用被信号打断（比如收到 SIGCHLD），这种情况不算失败，重试 poll
            } while (result < 0 && errno == EINTR);

            // result != 0：说明不是超时，有事件就绪 → 收到停止信号
            return result != 0;
        }

        // 运行在独立线程中，启动子进程并处理服务发现事件
        void Run()
        {
            INF("启动服务发现");
            for (;;)
            {
                FileDescriptor receiver;
                const pid_t pid = Spawn(receiver); // 返回子进程 pid，父进程持有 receiver 端 socket

                // 创建子进程失败，可能是系统资源不足，尝试等待一段时间后重试
                if (pid < 0)
                {
                    if (WaitForStop(1000))
                        return;
                    continue;
                }

                // _stop_fd（退出信号）、receiver（服务发现事件）
                pollfd fds[] = {{_stop_fd.get(), POLLIN, 0}, {receiver.get(), POLLIN, 0}};
                bool stopping = false;
                for (;;)
                {
                    const int ready = poll(fds, 2, -1); // 阻塞等待事件，直到有事件就绪或被信号打断
                    if (ready < 0 && errno == EINTR)    // 系统调用被信号打断，继续等待
                        continue;
                    if (ready < 0 || fds[0].revents != 0) // _stop_fd 可读，说明收到退出信号，停止服务发现
                    {
                        stopping = true;
                        break;
                    }
                    if (fds[1].revents != 0) // receiver 可读，说明有服务发现事件，读取并处理
                    {
                        char message[kMaxEventSize];                                              // 读取服务发现事件，最大长度 kMaxEventSize，避免缓冲区溢出
                        const ssize_t length = recv(receiver.get(), message, sizeof(message), 0); // 阻塞读取
                        if (length < 0 && errno == EINTR)                                         // 系统调用被信号打断，继续读取
                            continue;
                        if (length <= 0)
                            break;
                        ApplyEvent(std::string(message, static_cast<std::size_t>(length))); // 处理服务发现事件，更新服务信道管理对象
                    }
                }
                if (stopping) // 收到退出信号，停止服务发现
                    kill(pid, SIGTERM);

                // 等待子进程退出，避免僵尸进程，如果被信号打断，继续等待
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
                {
                }
                if (stopping)
                    return;

                // 重启时清除旧通道，由新监听器重新填充。
                for (const auto &service : _nodes)
                    for (const auto &address : service.second)
                        _channels->delNode(service.first, address);
                _nodes.clear();
                WRN("服务发现进程已退出，正在重新启动");
                if (WaitForStop(1000)) // 等待一段时间后重试，避免频繁重启
                    return;
            }
        }

        std::string _etcd_address;               // etcd 服务地址
        std::vector<std::string> _service_names; // 需要监听的服务名称列表
        std::shared_ptr<SvcChannels> _channels;  // 服务信道管理对象，负责维护各服务的 RPC 信道
        Nodes _nodes;                            // 当前在线的服务节点，用于在子进程退出时清除旧通道
        FileDescriptor _stop_fd;                 // 退出信号通道，避免阻塞主线程
        std::thread _thread;                     // 监听线程，负责启动子进程并处理服务发现事件
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
        if (_server_thread.joinable()) // 等待上次监听线程结束，避免资源泄漏
            _server_thread.join();
        if (!_server->bind_to_port(_address, _port))
        {
            ERR("网关监听地址绑定失败 {}:{}", _address, _port);
            return false;
        }
        _listener_finished.store(false);
        // 在独立线程中启动 HTTP 监听，避免阻塞主线程，确保退出时能及时响应
        _server_thread = std::thread([this]
                                     {
                                        const bool success = _server->listen_after_bind();
                                        _listener_finished.store(true);
                                        if (!success)
                                            ERR("网关监听失败"); });
        while (!_server->is_running() && !_listener_finished.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 等待监听线程启动完成
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
        if (_server_thread.joinable()) // 等待监听线程结束，避免资源泄漏
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
        auto channels = std::make_shared<SvcChannels>();                                 // 服务信道管理对象，负责维护各服务的 RPC 信道
        const std::set<std::string> names(_service_names.begin(), _service_names.end()); // 去重服务名称，避免重复监听
        for (const auto &name : names)
            channels->setWatch(name); // 设置服务名称的监听，确保服务发现事件能正确更新信道

        auto discovery = std::make_unique<DiscoveryProcess>(_etcd_address, _service_names, channels); // 服务发现对象，负责监听 etcd 服务节点变化并更新信道管理对象
        auto service = std::make_shared<GatewayServiceImpl>(channels);
        auto http = std::make_unique<httplib::Server>();
        // 允许重启后复用地址，同时保证端口被占用时报告监听失败。
        http->set_socket_options([](socket_t socket)
                                 {
                                    const int enabled = 1;
                                    // 设置 SO_REUSEADDR 允许重启后复用地址
                                    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)); });
        http->set_read_timeout(300, 0);  // 设置读超时
        http->set_write_timeout(300, 0); // 设置写超时
        service->BindRoutes(*http);
        return std::unique_ptr<GatewayServer>(new GatewayServer(_address, _port, std::move(http), std::move(service), std::move(discovery)));
    }
}
