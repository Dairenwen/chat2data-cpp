#include <etcd/Client.hpp>
#include <gtest/gtest.h>
#include <httplib.h>
#include <json/json.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char **environ;

namespace
{
    using namespace std::chrono_literals;
    constexpr char kEtcdAddress[] = "http://dev-etcd:2379";

    class Socket
    {
    public:
        Socket() : _fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {}
        ~Socket() { Close(); }
        Socket(const Socket &) = delete;
        Socket &operator=(const Socket &) = delete;

        bool Bind()
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return _fd >= 0 && bind(_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
        }

        bool Listen() const { return listen(_fd, 1) == 0; }

        int Port() const
        {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            return getsockname(_fd, reinterpret_cast<sockaddr *>(&address), &length) == 0 ? ntohs(address.sin_port) : -1;
        }

        void Close()
        {
            if (_fd >= 0)
            {
                close(_fd);
                _fd = -1;
            }
        }

    private:
        int _fd;
    };

    class GatewayProcess
    {
    public:
        GatewayProcess()
        {
            char path[] = "/tmp/chat2data-gateway-test-XXXXXX";
            _log_fd = mkstemp(path);
            if (_log_fd >= 0)
            {
                unlink(path);
                fcntl(_log_fd, F_SETFD, FD_CLOEXEC);
            }
        }

        ~GatewayProcess()
        {
            // 超时或断言失败时回收测试启动的进程。
            if (_pid > 0 && !WaitForExit(0ms))
            {
                kill(_pid, SIGKILL);
                while (waitpid(_pid, &_status, 0) < 0 && errno == EINTR)
                {
                }
            }
            if (_log_fd >= 0)
                close(_log_fd);
        }

        bool Start(int port, const std::string &user_service = "userService")
        {
            if (_log_fd < 0)
                return false;
            std::vector<std::string> args{
                GATEWAY_TEST_EXECUTABLE,
                "--gateway_address=127.0.0.1",
                "--gateway_port=" + std::to_string(port),
                std::string("--etcd_address=") + kEtcdAddress,
                "--user_service=" + user_service,
                "--log_level=info"};
            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            posix_spawn_file_actions_t actions;
            if (posix_spawn_file_actions_init(&actions) != 0)
                return false;
            int error = posix_spawn_file_actions_adddup2(&actions, _log_fd, STDOUT_FILENO);
            if (error == 0)
                error = posix_spawn_file_actions_adddup2(&actions, _log_fd, STDERR_FILENO);
            pid_t pid = -1;
            if (error == 0)
                error = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            if (error == 0)
                _pid = pid;
            return error == 0;
        }

        bool WaitForExit(std::chrono::milliseconds timeout)
        {
            if (_pid <= 0)
                return true;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            do
            {
                const pid_t result = waitpid(_pid, &_status, WNOHANG);
                if (result == _pid)
                {
                    _pid = -1;
                    return true;
                }
                if (result < 0 && errno != EINTR)
                    return false;
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(20ms);
            } while (true);
        }

        bool Stop(int signal)
        {
            return _pid > 0 && kill(_pid, signal) == 0 && WaitForExit(8s);
        }

        int Status() const { return _status; }

        std::string Logs() const
        {
            std::string logs;
            char buffer[4096];
            off_t offset = 0;
            ssize_t length;
            while ((length = pread(_log_fd, buffer, sizeof(buffer), offset)) > 0)
            {
                logs.append(buffer, static_cast<std::size_t>(length));
                offset += length;
            }
            return logs;
        }

        bool WaitForLog(const std::string &message, std::chrono::seconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (Logs().find(message) != std::string::npos)
                    return true;
                if (WaitForExit(0ms))
                    return false;
                std::this_thread::sleep_for(50ms);
            }
            return false;
        }

    private:
        pid_t _pid{-1};
        int _status{0};
        int _log_fd{-1};
    };

    bool WaitForHealth(GatewayProcess &process, int port)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 100000);
        client.set_read_timeout(1, 0);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto response = client.Get("/health"); response && response->status == 200)
                return true;
            if (process.WaitForExit(0ms))
                return false;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    class GatewayHttpTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            Socket socket;
            ASSERT_TRUE(socket.Bind());
            _port = socket.Port();
            ASSERT_GT(_port, 0);
            // 真实网关进程使用本地临时端口，避免占用固定端口。
            socket.Close();
            ASSERT_TRUE(_gateway.Start(_port));
            ASSERT_TRUE(WaitForHealth(_gateway, _port)) << _gateway.Logs();
        }

        int _port{};
        GatewayProcess _gateway;
    };

    TEST_F(GatewayHttpTest, HealthReportsServiceAndCurrentTime)
    {
        httplib::Client client("127.0.0.1", _port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(2, 0);
        const auto before = std::time(nullptr);
        const auto response = client.Get("/health");
        const auto after = std::time(nullptr);
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200);
        EXPECT_EQ(response->get_header_value("Content-Type").find("application/json"), 0u);

        Json::Value body;
        Json::CharReaderBuilder builder;
        std::istringstream stream(response->body);
        std::string errors;
        ASSERT_TRUE(Json::parseFromStream(builder, stream, &body, &errors)) << errors;
        EXPECT_EQ(body["status"].asString(), "healthy");
        EXPECT_EQ(body["service"].asString(), "GatewayService");
        ASSERT_TRUE(body["timestamp"].isInt64());
        EXPECT_GE(body["timestamp"].asInt64(), before - 1);
        EXPECT_LE(body["timestamp"].asInt64(), after + 1);
    }

    TEST_F(GatewayHttpTest, DocumentedBusinessRoutesHavePlaceholders)
    {
        // 当前业务接口都是空占位函数；实现业务后应相应更新其响应断言。
        struct Route
        {
            const char *method;
            const char *path;
        };
        const Route routes[] = {
            {"POST", "/api/user/valid/nickname"}, {"POST", "/api/user/valid/email"}, {"POST", "/api/user/register"}, {"POST", "/api/user/session/login"}, {"POST", "/api/user/passwd/login"}, {"POST", "/api/user/code"}, {"POST", "/api/user/vcode/login"}, {"POST", "/api/user/logout"}, {"GET", "/api/user/info"}, {"POST", "/api/file/upload/info"}, {"GET", "/api/file/info"}, {"POST", "/api/file/upload"}, {"GET", "/api/file/download"}, {"DELETE", "/api/file/test-file"}, {"POST", "/api/file/preview"}, {"POST", "/api/file/list"}, {"POST", "/api/file/chat/map"}, {"POST", "/api/file/sqlite/upload"}, {"POST", "/api/db/connect"}, {"GET", "/api/db/tables"}, {"POST", "/api/db/disconnect"}, {"POST", "/api/db/table/data"}, {"POST", "/api/db/connection/status"}, {"POST", "/api/ai/models"}, {"POST", "/api/ai/session/create"}, {"POST", "/api/ai/chatSessionLists"}, {"POST", "/api/ai/history"}, {"POST", "/api/ai/delete"}, {"POST", "/api/ai/sendStreamMessage"}};
        httplib::Client client("127.0.0.1", _port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(2, 0);
        for (const auto &route : routes)
        {
            SCOPED_TRACE(std::string(route.method) + " " + route.path);
            httplib::Result response = std::string(route.method) == "POST"
                                           ? client.Post(route.path, "{}", "application/json")
                                       : std::string(route.method) == "DELETE"
                                           ? client.Delete(route.path)
                                           : client.Get(route.path);
            ASSERT_TRUE(response);
            EXPECT_EQ(response->status, 200);
            EXPECT_TRUE(response->body.empty());
        }
    }

    TEST_F(GatewayHttpTest, UnknownRouteAndWrongMethodReturnNotFound)
    {
        httplib::Client client("127.0.0.1", _port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(2, 0);
        const auto missing = client.Get("/api/unknown");
        const auto wrong_method = client.Get("/api/user/register");
        ASSERT_TRUE(missing);
        ASSERT_TRUE(wrong_method);
        EXPECT_EQ(missing->status, 404);
        EXPECT_EQ(wrong_method->status, 404);
    }

    TEST_F(GatewayHttpTest, HandlesConcurrentHealthRequests)
    {
        // 每个线程使用独立客户端，模拟多个用户同时请求。
        std::mutex failures_mutex;
        std::vector<std::string> failures;
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
        {
            threads.emplace_back([this, &failures, &failures_mutex]
                                 {
            httplib::Client client("127.0.0.1", _port);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(5, 0);
            for (int j = 0; j < 10; ++j)
            {
                const auto response = client.Get("/health");
                if (!response || response->status != 200 || response->body.find("healthy") == std::string::npos)
                {
                    std::lock_guard<std::mutex> lock(failures_mutex);
                    failures.push_back(response ? "健康响应内容或状态错误" : httplib::to_string(response.error()));
                }
            } });
        }
        for (auto &thread : threads)
            thread.join();
        for (const auto &failure : failures)
            ADD_FAILURE() << failure;
    }

    class GatewaySignalTest : public GatewayHttpTest, public ::testing::WithParamInterface<int>
    {
    };

    TEST_P(GatewaySignalTest, StopsCleanlyOnSignal)
    {
        // 等到服务发现管理线程启动，再验证信号退出。
        ASSERT_TRUE(_gateway.WaitForLog("启动服务发现", 10s)) << _gateway.Logs();
        ASSERT_TRUE(_gateway.Stop(GetParam())) << _gateway.Logs();
        ASSERT_TRUE(WIFEXITED(_gateway.Status())) << _gateway.Logs();
        EXPECT_EQ(WEXITSTATUS(_gateway.Status()), 0);
        EXPECT_NE(_gateway.Logs().find("网关服务发现已停止"), std::string::npos);
    }

    INSTANTIATE_TEST_SUITE_P(ExitSignals, GatewaySignalTest, ::testing::Values(SIGINT, SIGTERM));

    TEST(GatewayStartupTest, OccupiedPortIsReportedAsFailure)
    {
        // 保持本地监听套接字打开，模拟端口被其他服务占用。
        Socket occupied;
        ASSERT_TRUE(occupied.Bind());
        ASSERT_TRUE(occupied.Listen());
        const int port = occupied.Port();
        ASSERT_GT(port, 0);

        GatewayProcess gateway;
        ASSERT_TRUE(gateway.Start(port));
        ASSERT_TRUE(gateway.WaitForExit(10s)) << gateway.Logs();
        ASSERT_TRUE(WIFEXITED(gateway.Status())) << gateway.Logs();
        EXPECT_NE(WEXITSTATUS(gateway.Status()), 0);
        EXPECT_NE(gateway.Logs().find("网关监听地址绑定失败"), std::string::npos);
    }

    TEST(GatewayStartupTest, InvalidPortIsRejected)
    {
        GatewayProcess gateway;
        ASSERT_TRUE(gateway.Start(0));
        ASSERT_TRUE(gateway.WaitForExit(5s)) << gateway.Logs();
        ASSERT_TRUE(WIFEXITED(gateway.Status())) << gateway.Logs();
        EXPECT_NE(WEXITSTATUS(gateway.Status()), 0);
        EXPECT_NE(gateway.Logs().find("无效的网关端口"), std::string::npos);
    }

    class EtcdTestKey
    {
    public:
        EtcdTestKey(etcd::Client &client, std::string key, std::string value)
            : _client(client), _key(std::move(key)), _value(std::move(value)) {}
        ~EtcdTestKey()
        {
            if (_created)
            {
                try
                {
                    if (!_client.rm_if(_key, _value).get().is_ok())
                        ADD_FAILURE() << "清理测试键失败: " << _key;
                }
                catch (const std::exception &error)
                {
                    ADD_FAILURE() << "清理测试键失败: " << error.what();
                }
            }
        }

        bool Create(std::int64_t lease)
        {
            _created = _client.add(_key, _value, lease).get().is_ok();
            return _created;
        }

        bool Remove()
        {
            const bool removed = _client.rm_if(_key, _value).get().is_ok();
            if (removed)
                _created = false;
            return removed;
        }

    private:
        etcd::Client &_client;
        std::string _key;
        std::string _value;
        bool _created{false};
    };

    TEST(GatewayDiscoveryTest, DiscoversExistingAndNewNodesAndTheirRemoval)
    {
        etcd::Client etcd(kEtcdAddress);
        etcd.set_grpc_timeout(2s);
        ASSERT_TRUE(etcd.head().get().is_ok()) << "无法连接测试用 etcd";
        const auto id = std::to_string(getpid()) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string service = "gatewayTest-" + id;
        const std::string endpoint = "127.0.0.1:19001";
        EtcdTestKey key(etcd, "/" + service + "/node", endpoint);
        // 使用独立租约，测试进程异常退出时测试键也会自动过期。
        const auto lease = etcd.leasegrant(120).get();
        ASSERT_TRUE(lease.is_ok());
        ASSERT_TRUE(key.Create(lease.value().lease()));

        Socket socket;
        ASSERT_TRUE(socket.Bind());
        const int port = socket.Port();
        ASSERT_GT(port, 0);
        socket.Close();
        GatewayProcess gateway;
        ASSERT_TRUE(gateway.Start(port, service));
        ASSERT_TRUE(WaitForHealth(gateway, port)) << gateway.Logs();
        ASSERT_TRUE(gateway.WaitForLog("服务上线 " + service + " " + endpoint, 12s)) << gateway.Logs();
        ASSERT_TRUE(key.Remove());
        ASSERT_TRUE(gateway.WaitForLog("服务下线 " + service + " " + endpoint, 12s)) << gateway.Logs();

        // 服务发现运行后再注册节点，覆盖运行期间的上线和下线。
        const std::string new_endpoint = "127.0.0.1:19002";
        EtcdTestKey new_key(etcd, "/" + service + "/new-node", new_endpoint);
        ASSERT_TRUE(new_key.Create(lease.value().lease()));
        ASSERT_TRUE(gateway.WaitForLog("服务上线 " + service + " " + new_endpoint, 12s)) << gateway.Logs();
        ASSERT_TRUE(new_key.Remove());
        ASSERT_TRUE(gateway.WaitForLog("服务下线 " + service + " " + new_endpoint, 12s)) << gateway.Logs();
        ASSERT_TRUE(gateway.Stop(SIGTERM)) << gateway.Logs();
        ASSERT_TRUE(WIFEXITED(gateway.Status()));
        EXPECT_EQ(WEXITSTATUS(gateway.Status()), 0);
    }
}
