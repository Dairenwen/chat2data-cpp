#include "../svc_userService/testSupport.hpp"
#include "sessionData.hpp"
#include "userService.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/endpoint.h>
#include <butil/fd_guard.h>
#include <etcd/Client.hpp>
#include <httplib.h>
#include <json/json.h>

#include <arpa/inet.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace
{
    using namespace std::chrono_literals;

    constexpr char kLoopback[] = "127.0.0.1";
    constexpr char kEtcdAddress[] = "http://dev-etcd:2379";

    // 保留系统分配的端口，直到对应的服务即将启动。
    int ReservePort(int fd)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        butil::EndPoint endpoint;
        if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            butil::get_local_side(fd, &endpoint) != 0)
            return -1;
        return endpoint.port;
    }

    template <typename Predicate>
    bool WaitUntil(Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        do
        {
            if (ready())
                return true;
            std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    // 保留服务日志，便于确认网关收到了实际的 etcd 节点事件。
    class ServiceProcess
    {
    public:
        ~ServiceProcess()
        {
            // 断言失败时也会回收测试启动的进程。
            if (!Stop() && _pid > 0)
            {
                kill(_pid, SIGKILL);
                while (waitpid(_pid, nullptr, 0) < 0 && errno == EINTR)
                {
                }
            }
        }

        bool Start(std::vector<std::string> args)
        {
            _log.reset(std::tmpfile());
            if (!_log)
                return false;

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

        bool Stop()
        {
            if (_pid <= 0)
                return true;
            kill(_pid, SIGTERM);
            int status = 0;
            if (!WaitUntil([&] { return waitpid(_pid, &status, WNOHANG) == _pid; }))
                return false;
            _pid = -1;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

        std::string Logs() const
        {
            std::string logs;
            char buffer[4096];
            off_t offset = 0;
            // pread 使用独立偏移，服务进程可继续写日志。
            ssize_t count;
            while ((count = pread(fileno(_log.get()), buffer, sizeof(buffer), offset)) > 0)
            {
                logs.append(buffer, count);
                offset += count;
            }
            return logs;
        }

        bool WaitForLog(const std::string &message) const
        {
            return WaitUntil([&] { return Logs().find(message) != std::string::npos; });
        }

    private:
        std::unique_ptr<std::FILE, decltype(&std::fclose)> _log{nullptr, &std::fclose};
        pid_t _pid = -1;
    };

    using GatewayUserTest = user_data_test::DataTestFixture;

    // 通过真实服务注册和发现联调，检查网关健康接口及用户资料查询。
    TEST_F(GatewayUserTest, DiscoversUserServiceAndServesRequests)
    {
        // 两个服务共用独立注册名，避免发现开发环境中的其他用户服务。
        const auto service_name = "gateway-user-" + prefix;
        const auto service_flag = "--user_service=" + service_name;
        const auto etcd_flag = std::string("--etcd_address=") + kEtcdAddress;
        butil::fd_guard gateway_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        butil::fd_guard user_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int gateway_port = ReservePort(gateway_socket);
        const int user_port = ReservePort(user_socket);
        ASSERT_GT(gateway_port, 0);
        ASSERT_GT(user_port, 0);
        const auto user_address = std::string(kLoopback) + ":" + std::to_string(user_port);

        // 使用独立 Redis 键准备会话和用户资料，服务进程可以跨连接读取。
        const auto session_id = Track("session", prefix);
        const auto user_id = Track("user", prefix + "-user");
        const auto email = prefix + "@example.com";
        chat2Data::UserData(database, redis).SaveCache({user_id, prefix, "test-cache-value", email, 1});
        ASSERT_TRUE(chat2Data::SessionData(database, redis).SaveCache(
            {session_id, user_id, user_data_test::NowMs() + 30000}));

        ServiceProcess gateway;
        gateway_socket.reset(-1);
        ASSERT_TRUE(gateway.Start({GATEWAY_TEST_EXECUTABLE, std::string("--gateway_address=") + kLoopback,
                                   "--gateway_port=" + std::to_string(gateway_port),
                                   service_flag, etcd_flag, "--log_level=info"}));
        ASSERT_TRUE(gateway.WaitForLog("网关服务器已启动")) << gateway.Logs();

        ServiceProcess user;
        user_socket.reset(-1);
        ASSERT_TRUE(user.Start({USER_SERVICE_EXECUTABLE, "--user_port=" + std::to_string(user_port),
                                "--user_address=" + user_address, service_flag, etcd_flag}));
        ASSERT_TRUE(gateway.WaitForLog("服务上线 " + service_name + " " + user_address))
            << gateway.Logs() << user.Logs();

        httplib::Client http(kLoopback, gateway_port);
        http.set_connection_timeout(1, 0);
        http.set_read_timeout(2, 0);
        const auto health = http.Get("/health");
        ASSERT_TRUE(health);
        ASSERT_EQ(health->status, 200);
        Json::Value body;
        Json::CharReaderBuilder reader;
        std::istringstream json(health->body);
        std::string errors;
        ASSERT_TRUE(Json::parseFromStream(reader, json, &body, &errors)) << errors;
        EXPECT_EQ(body["status"].asString(), "healthy");
        EXPECT_EQ(body["service"].asString(), "GatewayService");

        // 用户 HTTP 路由当前为占位，直接使用 etcd 公布的地址验证用户 RPC。
        etcd::Client etcd(kEtcdAddress);
        etcd.set_grpc_timeout(1s);
        const auto registration = etcd.ls("/" + service_name + "/").get();
        ASSERT_TRUE(registration.is_ok()) << registration.error_message();
        ASSERT_EQ(registration.values().size(), 1u);
        const auto endpoint = registration.value(0).as_string();
        ASSERT_EQ(endpoint, user_address);
        brpc::Channel channel;
        brpc::ChannelOptions options;
        options.timeout_ms = 2000;
        options.max_retry = 0;
        ASSERT_EQ(channel.Init(endpoint.c_str(), &options), 0);
        brpc::Controller controller;
        chat2Data::GetUserInfoRequest request;
        chat2Data::GetUserInfoResponse response;
        request.set_session_id(session_id);
        request.set_request_id(prefix);
        chat2Data::UserService_Stub(&channel).GetUserInfo(&controller, &request, &response, nullptr);
        ASSERT_FALSE(controller.Failed()) << controller.ErrorText() << user.Logs();
        EXPECT_EQ(response.request_id(), prefix);
        ASSERT_EQ(response.error_code(), static_cast<int>(chat2Data::ErrorCode::SUCCESS)) << response.error_msg();
        ASSERT_TRUE(response.has_user_info());
        EXPECT_EQ(response.user_info().user_id(), user_id);
        EXPECT_EQ(response.user_info().nickname(), prefix);
        EXPECT_EQ(response.user_info().email(), email);

        // 用户服务正常退出后，网关应收到该实例的下线事件。
        ASSERT_TRUE(user.Stop()) << user.Logs();
        EXPECT_TRUE(gateway.WaitForLog("服务下线 " + service_name + " " + user_address)) << gateway.Logs();
        EXPECT_TRUE(gateway.Stop()) << gateway.Logs();
    }
}
