#include "sessionData.hpp"
#include "testSupport.hpp"
#include "userService.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/fd_guard.h>
#include <etcd/Client.hpp>
#include <etcd/v3/action_constants.hpp>

#include <arpa/inet.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace
{
    using namespace std::chrono_literals;
    using chat2Data::ErrorCode;

    constexpr char kEtcdAddress[] = "http://dev-etcd:2379";

    // 由系统分配回环端口。调用方可以释放端口供服务监听，也可以保留端口测试冲突。
    int BindTemporaryPort(int fd)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        if (bind(fd, reinterpret_cast<sockaddr *>(&address), length) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return -1;
        return ntohs(address.sin_port);
    }

    // 等待注册、监听事件或进程退出，给异步操作设置明确的截止时间。
    template <typename Predicate>
    bool WaitUntil(Predicate ready, std::chrono::milliseconds timeout = 8s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do
        {
            if (ready())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(50ms);
        } while (true);
    }

    // 管理实际服务进程，保留标准输出和错误日志供断言失败时查看。
    class UserServiceProcess
    {
    public:
        ~UserServiceProcess()
        {
            // 断言中途失败时同样回收子进程，注册键会随租约到期清理。
            if (_pid > 0 && !WaitForExit(0ms))
            {
                kill(_pid, SIGKILL);
                while (waitpid(_pid, &_status, 0) < 0 && errno == EINTR)
                {
                }
            }
        }

        bool Start(std::vector<std::string> args)
        {
            _log.reset(std::tmpfile());
            if (!_log)
                return false;
            args.insert(args.begin(), USER_SERVICE_EXECUTABLE);
            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            const int fd = fileno(_log.get());
            posix_spawn_file_actions_t actions;
            if (posix_spawn_file_actions_init(&actions) != 0)
                return false;
            int error = posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
            if (error == 0)
                error = posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
            if (error == 0)
                error = posix_spawn_file_actions_addclose(&actions, fd);
            if (error == 0)
                error = posix_spawn(&_pid, argv.front(), &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            return error == 0;
        }

        bool WaitForExit(std::chrono::milliseconds timeout = 8s)
        {
            if (_pid <= 0)
                return true;
            return WaitUntil([this]
            {
                if (waitpid(_pid, &_status, WNOHANG) != _pid)
                    return false;
                _pid = -1;
                return true;
            }, timeout);
        }

        bool Stop(int signal)
        {
            return _pid > 0 && kill(_pid, signal) == 0 && WaitForExit();
        }

        int ExitCode() const
        {
            return WIFEXITED(_status) ? WEXITSTATUS(_status) : -1;
        }

        std::string Logs() const
        {
            std::string logs;
            char buffer[4096];
            // pread 单独指定读取偏移，避免改变子进程追加日志的位置。
            for (off_t offset = 0; ; )
            {
                const auto count = pread(fileno(_log.get()), buffer, sizeof(buffer), offset);
                if (count <= 0)
                    return logs;
                logs.append(buffer, count);
                offset += count;
            }
        }

    private:
        std::unique_ptr<std::FILE, decltype(&std::fclose)> _log{nullptr, &std::fclose};
        pid_t _pid = -1;
        int _status = 0;
    };

    class UserServerTest : public user_data_test::DataTestFixture
    {
    protected:
        void SetUp() override
        {
            DataTestFixture::SetUp();
            etcd.set_grpc_timeout(1s);
        }

        void TearDown() override
        {
            if (notification_lease != 0)
                EXPECT_TRUE(etcd.leaserevoke(notification_lease).get().is_ok());
            DataTestFixture::TearDown();
        }

        std::string ServicePrefix() const
        {
            return "/user-server-" + prefix + "/";
        }

        bool Start(int port, const std::vector<std::string> &flags = {})
        {
            std::vector<std::string> args{
                "--user_port=" + std::to_string(port),
                "--user_service=user-server-" + prefix};
            args.insert(args.end(), flags.begin(), flags.end());
            return server.Start(std::move(args));
        }

        std::optional<etcd::Value> WaitForRegistration()
        {
            std::optional<etcd::Value> value;
            WaitUntil([&]
            {
                const auto response = etcd.ls(ServicePrefix()).get();
                if (!response.is_ok() || response.values().size() != 1)
                    return false;
                value = response.value(0);
                return true;
            });
            return value;
        }

        void ExpectGracefulExit(int signal, const std::string &key)
        {
            ASSERT_TRUE(server.Stop(signal)) << server.Logs();
            EXPECT_EQ(server.ExitCode(), 0) << server.Logs();
            EXPECT_TRUE(WaitUntil([&]
            {
                return etcd.get(key).get().error_code() == etcdv3::ERROR_KEY_NOT_FOUND;
            })) << key;
        }

        void ExpectStartupFailure(int port, const std::vector<std::string> &flags, ErrorCode error)
        {
            ASSERT_TRUE(Start(port, flags));
            ASSERT_TRUE(server.WaitForExit()) << server.Logs();
            EXPECT_EQ(server.ExitCode(), 1) << server.Logs();
            const auto expected = "用户服务错误码 " + std::to_string(static_cast<int>(error)) +
                                  "，" + chat2Data::error2String(error);
            const auto logs = server.Logs();
            EXPECT_NE(logs.find(expected), std::string::npos) << logs;
            const auto nodes = etcd.ls(ServicePrefix()).get();
            EXPECT_TRUE(nodes.is_ok() || nodes.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
                << nodes.error_message();
            EXPECT_TRUE(nodes.values().empty());
        }

        etcd::Client etcd{kEtcdAddress};
        std::int64_t notification_lease = 0;
        UserServiceProcess server;
    };

    // 通过 etcd 发布的地址调用真实 RPC，并检查租约续期和 SIGTERM 退出。
    TEST_F(UserServerTest, ServesRpcAndMaintainsRegistrationUntilShutdown)
    {
        butil::fd_guard socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = BindTemporaryPort(socket_fd);
        ASSERT_GT(port, 0);
        const auto address = "127.0.0.1:" + std::to_string(port);

        // 使用独立 Redis 键准备有效会话，供服务进程跨连接读取。
        const auto session_id = Track("session", prefix);
        const auto user_id = prefix + "-user";
        chat2Data::SessionData sessions(database, redis);
        ASSERT_TRUE(sessions.SaveCache({session_id, user_id, user_data_test::NowMs() + 60000}));

        socket_fd.reset(-1);
        ASSERT_TRUE(Start(port, {"--user_address=" + address}));
        const auto registered = WaitForRegistration();
        ASSERT_TRUE(registered) << server.Logs();
        EXPECT_EQ(registered->as_string(), address);
        ASSERT_NE(registered->lease(), 0);

        brpc::Channel channel;
        brpc::ChannelOptions options;
        options.timeout_ms = 1500;
        options.max_retry = 0;
        ASSERT_EQ(channel.Init(registered->as_string().c_str(), &options), 0);
        chat2Data::UserService_Stub stub(&channel);
        brpc::Controller controller;
        chat2Data::IsSessionValidRequest request;
        chat2Data::IsSessionValidResponse response;
        request.set_request_id(prefix);
        request.set_session_id(session_id);
        stub.IsSessionValid(&controller, &request, &response, nullptr);
        ASSERT_FALSE(controller.Failed()) << controller.ErrorText() << server.Logs();
        EXPECT_EQ(response.request_id(), prefix);
        EXPECT_EQ(response.error_code(), static_cast<int>(ErrorCode::SUCCESS));
        EXPECT_TRUE(response.valid());
        EXPECT_EQ(response.user_id(), user_id);

        controller.Reset();
        request.clear_session_id();
        stub.IsSessionValid(&controller, &request, &response, nullptr);
        ASSERT_FALSE(controller.Failed()) << controller.ErrorText();
        EXPECT_EQ(response.request_id(), prefix);
        EXPECT_EQ(response.error_code(), static_cast<int>(ErrorCode::USER_INVALID_INPUT));
        EXPECT_EQ(response.error_msg(), chat2Data::error2String(ErrorCode::USER_INVALID_INPUT));
        EXPECT_FALSE(response.valid());
        EXPECT_TRUE(response.user_id().empty());

        // 越过 SDK 的初始三秒租期，确认同一注册键仍绑定原租约。
        std::this_thread::sleep_for(4s);
        const auto renewed = etcd.get(registered->key()).get();
        ASSERT_TRUE(renewed.is_ok()) << renewed.error_message();
        EXPECT_EQ(renewed.value().lease(), registered->lease());
        EXPECT_EQ(renewed.value().as_string(), address);
        ExpectGracefulExit(SIGTERM, registered->key());
    }

    // 预先存在和运行时变更的通知节点都应被实际服务进程中的 etcd 监听器接收。
    TEST_F(UserServerTest, DiscoversExistingAndChangedNotificationNodes)
    {
        const auto watch_name = "notify-server-" + prefix;
        const auto existing_key = "/" + watch_name + "/existing";
        const auto new_key = "/" + watch_name + "/new";
        const auto lease = etcd.leasegrant(60).get();
        ASSERT_TRUE(lease.is_ok()) << lease.error_message();
        notification_lease = lease.value().lease();
        ASSERT_NE(notification_lease, 0);
        ASSERT_TRUE(etcd.add(existing_key, "127.0.0.1:1", notification_lease).get().is_ok());

        butil::fd_guard socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = BindTemporaryPort(socket_fd);
        ASSERT_GT(port, 0);
        socket_fd.reset(-1);
        ASSERT_TRUE(Start(port, {"--watch_service=" + watch_name, "--log_level=debug"}));
        const auto registered = WaitForRegistration();
        ASSERT_TRUE(registered) << server.Logs();
        EXPECT_EQ(registered->as_string(), "dev-env-service:" + std::to_string(port));
        EXPECT_NE(server.Logs().find("已有 " + watch_name + " 节点: 127.0.0.1:1"), std::string::npos);

        ASSERT_TRUE(etcd.add(new_key, "127.0.0.1:2", notification_lease).get().is_ok());
        ASSERT_TRUE(WaitUntil([&]
        {
            return server.Logs().find(watch_name + " 服务上线节点: 127.0.0.1:2") != std::string::npos;
        })) << server.Logs();
        ASSERT_TRUE(etcd.rm_if(new_key, "127.0.0.1:2").get().is_ok());
        EXPECT_TRUE(WaitUntil([&]
        {
            return server.Logs().find(watch_name + " 服务下线节点: 127.0.0.1:2") != std::string::npos;
        })) << server.Logs();
        ExpectGracefulExit(SIGINT, registered->key());
    }

    // 检查启动入口和构建器拒绝无效配置，并返回项目统一错误码。
    TEST_F(UserServerTest, RejectsInvalidStartupConfiguration)
    {
        ASSERT_NO_FATAL_FAILURE(ExpectStartupFailure(0, {}, ErrorCode::USER_SERVER_CONFIG_INVALID));
        ASSERT_NO_FATAL_FAILURE(ExpectStartupFailure(9001, {"--watch_service="},
                                                      ErrorCode::USER_SERVER_CONFIG_INVALID));
        ExpectStartupFailure(9001, {"--redis_pool_size=0"}, ErrorCode::USER_SERVER_CONFIG_INVALID);
    }

    // 使用未监听的回环端口分别模拟注册中心和发现端点连接失败。
    TEST_F(UserServerTest, RejectsUnavailableRegistryAndDiscoveryEndpoints)
    {
        butil::fd_guard socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = BindTemporaryPort(socket_fd);
        ASSERT_GT(port, 0);
        const auto unavailable = "http://127.0.0.1:" + std::to_string(port);
        ASSERT_NO_FATAL_FAILURE(ExpectStartupFailure(9001, {"--registry_address=" + unavailable},
                                                      ErrorCode::USER_ETCD_CONNECTION_FAILED));
        ExpectStartupFailure(9001, {"--etcd_address=" + unavailable, std::string("--registry_address=") + kEtcdAddress},
                             ErrorCode::USER_ETCD_CONNECTION_FAILED);
    }

    // 端口已被占用时启动应失败，且 etcd 中没有本次启动的注册节点。
    TEST_F(UserServerTest, DoesNotRegisterWhenPortIsOccupied)
    {
        butil::fd_guard socket_fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = BindTemporaryPort(socket_fd);
        ASSERT_GT(port, 0);
        ASSERT_EQ(listen(socket_fd, 1), 0);
        ExpectStartupFailure(port, {}, ErrorCode::USER_SERVER_START_FAILED);
    }
}
