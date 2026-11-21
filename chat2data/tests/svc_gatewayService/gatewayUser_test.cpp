#include <brpc/server.h> // brpc 的 Redis 协议头需要先于 hiredis。

#include "../svc_userService/testSupport.hpp"
#include "notifyService.pb.h"
#include "sessionData.hpp"
#include "utils.hpp"

#include <brpc/closure_guard.h>
#include <butil/endpoint.h>
#include <butil/fd_guard.h>
#include <etcd/Client.hpp>
#include <etcd/v3/action_constants.hpp>
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
#include <cstdint>
#include <initializer_list>
#include <memory>
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
    using chat2Data::ErrorCode;

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

    // 测试接收端记录用户服务发来的通知 RPC，并按协议确认请求。
    class NotificationReceiver final : public chat2Data::NotifyService
    {
    public:
        void SendVerifyCode(google::protobuf::RpcController *, const chat2Data::SendVerifyCodeRequest *request,
                            chat2Data::SendVerifyCodeResponse *response, google::protobuf::Closure *done) override
        {
            brpc::ClosureGuard guard(done);
            const std::lock_guard<std::mutex> lock(_mutex);
            _request = *request;
            response->set_request_id(request->request_id());
        }

        chat2Data::SendVerifyCodeRequest Received()
        {
            const std::lock_guard<std::mutex> lock(_mutex);
            return _request;
        }

    private:
        std::mutex _mutex;
        chat2Data::SendVerifyCodeRequest _request;
    };

    class GatewayUserTest : public user_data_test::DataTestFixture
    {
    protected:
        void SetUp() override
        {
            prefix = "gw" + chat2Data::Utils::GenerateUUID();
            _schema = "gateway_test_" + prefix;
            _admin = OpenMysql(FLAGS_mysql_database);
            // 网关和用户服务分别运行，使用专用数据库共享测试表。
            _admin->connection()->execute("CREATE DATABASE `" + _schema + "` CHARACTER SET utf8mb4");
            _owns_schema = true;
            database = OpenMysql(_schema);
            OpenRedis();
            ASSERT_NO_FATAL_FAILURE(CreateUserTables(false));
            _etcd.set_grpc_timeout(1s);
        }

        void TearDown() override
        {
            if (_notification_lease)
                EXPECT_TRUE(_etcd.leaserevoke(_notification_lease).get().is_ok());
            EXPECT_NO_THROW(DataTestFixture::TearDown());
            database.reset();
            if (_owns_schema)
                _admin->connection()->execute("DROP DATABASE `" + _schema + "`");
        }

        void StartGateway(ServiceProcess &gateway)
        {
            butil::fd_guard reservation(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
            const int port = ReservePort(reservation);
            ASSERT_GT(port, 0);
            reservation.reset(-1);
            ASSERT_TRUE(gateway.Start({GATEWAY_TEST_EXECUTABLE, "--gateway_address=127.0.0.1",
                                       "--gateway_port=" + std::to_string(port), "--user_service=" + ServiceName(),
                                       std::string("--etcd_address=") + kEtcdAddress, "--log_level=info"}));
            ASSERT_TRUE(gateway.WaitForLog("网关服务器已启动")) << gateway.Logs();
            _http = std::make_unique<httplib::Client>(kLoopback, port);
            _http->set_connection_timeout(1, 0);
            _http->set_read_timeout(3, 0);
            const auto health = _http->Get("/health");
            ASSERT_TRUE(health);
            EXPECT_EQ(health->status, 200);
        }

        void PublishNotification(int port)
        {
            const auto lease = _etcd.leasegrant(120).get();
            ASSERT_TRUE(lease.is_ok()) << lease.error_message();
            _notification_lease = lease.value().lease();
            ASSERT_NE(_notification_lease, 0);
            const auto node = _etcd.add("/" + NotificationName() + "/receiver",
                                        "127.0.0.1:" + std::to_string(port), _notification_lease).get();
            ASSERT_TRUE(node.is_ok()) << node.error_message();
        }

        void StartUser(ServiceProcess &user, int port)
        {
            ASSERT_TRUE(user.Start({USER_SERVICE_EXECUTABLE, "--user_port=" + std::to_string(port),
                                    "--user_address=127.0.0.1:" + std::to_string(port),
                                    "--user_service=" + ServiceName(), "--watch_service=" + NotificationName(),
                                    std::string("--etcd_address=") + kEtcdAddress, "--mysql_database=" + _schema}));
        }

        void ExpectDiscovered(ServiceProcess &gateway, ServiceProcess &user, int port)
        {
            const auto address = "127.0.0.1:" + std::to_string(port);
            ASSERT_TRUE(gateway.WaitForLog("服务上线 " + ServiceName() + " " + address))
                << gateway.Logs() << user.Logs();
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get();
            ASSERT_TRUE(registration.is_ok()) << registration.error_message();
            ASSERT_EQ(registration.values().size(), 1u);
            const auto &node = registration.value(0);
            EXPECT_EQ(node.as_string(), address);
            EXPECT_NE(node.lease(), 0);
            // 节点事件日志和信道更新异步发生，用只读 HTTP 请求确认网关实际就绪。
            ASSERT_TRUE(WaitForHttpStatus(200)) << gateway.Logs() << user.Logs();
        }

        void ExpectOffline(ServiceProcess &gateway, ServiceProcess &user, int port)
        {
            ASSERT_TRUE(user.Stop()) << user.Logs();
            ASSERT_TRUE(gateway.WaitForLog("服务下线 " + ServiceName() + " 127.0.0.1:" + std::to_string(port)))
                << gateway.Logs();
            ASSERT_TRUE(WaitForHttpStatus(503)) << gateway.Logs();
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get();
            EXPECT_TRUE(registration.is_ok() || registration.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
                << registration.error_message();
            EXPECT_TRUE(registration.values().empty());
        }

        Json::Value Post(const std::string &route,
                         std::initializer_list<std::pair<const char *, std::string>> fields,
                         ErrorCode code = ErrorCode::SUCCESS, int status = 200)
        {
            SCOPED_TRACE(route);
            Json::Value body(Json::objectValue);
            const auto request_id = NextRequestId();
            body["requestId"] = request_id;
            for (const auto &[name, value] : fields)
                body[name] = value;
            const auto reply = _http->Post("/api/user/" + route, Json::writeString(Json::StreamWriterBuilder{}, body),
                                           "application/json");
            return ExpectReply(reply, request_id, code, status);
        }

        Json::Value GetInfo(const std::string &session_id, ErrorCode code = ErrorCode::SUCCESS, int status = 200)
        {
            SCOPED_TRACE("GET /api/user/info");
            const auto request_id = NextRequestId();
            const httplib::Params query{{"requestId", request_id}, {"sessionId", session_id}};
            const auto reply = _http->Get("/api/user/info", query, httplib::Headers{});
            return ExpectReply(reply, request_id, code, status)["result"]["userInfo"];
        }

        void ExpectInvalidRequests()
        {
            ExpectReply(_http->Post("/api/user/valid/email", "{", "application/json"), "",
                        ErrorCode::USER_INVALID_INPUT, 400);
            ExpectReply(_http->Get("/api/user/info?requestId=missing-session"), "missing-session",
                        ErrorCode::USER_INVALID_INPUT, 400);
        }

    private:
        std::string ServiceName() const { return "gateway-user-" + prefix; }
        std::string NotificationName() const { return "gateway-notify-" + prefix; }
        std::string NextRequestId() { return prefix + "-" + std::to_string(++_request_sequence); }

        bool WaitForHttpStatus(int status)
        {
            return WaitUntil([&]
            {
                const auto reply = _http->Post("/api/user/valid/nickname",
                    R"({"requestId":"gateway-ready","nickname":"gateway-readiness"})", "application/json");
                return reply && reply->status == status;
            });
        }

        // 九个接口都应关联请求 ID，并用统一错误字段报告结果。
        static Json::Value ExpectReply(const httplib::Result &reply, const std::string &request_id,
                                       ErrorCode code, int status)
        {
            if (!reply)
            {
                ADD_FAILURE() << "网关没有返回 HTTP 响应";
                return {};
            }
            EXPECT_EQ(reply->status, status);
            EXPECT_EQ(reply->get_header_value("Content-Type").find("application/json"), 0u);
            Json::Value body;
            Json::CharReaderBuilder reader;
            std::istringstream input(reply->body);
            if (!Json::parseFromStream(reader, input, &body, nullptr) || !body.isObject())
            {
                ADD_FAILURE() << "网关响应不是 JSON 对象";
                return {};
            }
            EXPECT_TRUE(body["requestId"].isString());
            EXPECT_TRUE(body["errorCode"].isInt());
            EXPECT_TRUE(body["errorMsg"].isString());
            EXPECT_EQ(body["requestId"].asString(), request_id);
            EXPECT_EQ(body["errorCode"].asInt(), static_cast<int>(code));
            EXPECT_EQ(body["errorMsg"].asString(), code == ErrorCode::SUCCESS ? "" : chat2Data::error2String(code));
            if (code != ErrorCode::SUCCESS)
                EXPECT_FALSE(body.isMember("result"));
            return body;
        }

        etcd::Client _etcd{kEtcdAddress};
        std::int64_t _notification_lease = 0;
        std::shared_ptr<odb::database> _admin;
        std::string _schema;
        bool _owns_schema = false;
        unsigned _request_sequence = 0;
        std::unique_ptr<httplib::Client> _http;
    };

    // 所有用户操作从网关 HTTP 入口发起，账号和会话由真实用户服务创建。
    TEST_F(GatewayUserTest, CompletesAccountAndMultiDeviceSessionFlowOverHttp)
    {
        NotificationReceiver notification;
        brpc::Server notification_server;
        ASSERT_EQ(notification_server.AddService(&notification, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(notification_server.Start("127.0.0.1:0", nullptr), 0);
        ASSERT_NO_FATAL_FAILURE(PublishNotification(notification_server.listen_address().port));

        ServiceProcess gateway;
        ASSERT_NO_FATAL_FAILURE(StartGateway(gateway));
        butil::fd_guard reservation(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = ReservePort(reservation);
        ASSERT_GT(port, 0);
        reservation.reset(-1);
        ServiceProcess user;
        ASSERT_NO_FATAL_FAILURE(StartUser(user, port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(gateway, user, port));

        const auto nickname = prefix;
        const auto email = prefix + "@example.com";
        const std::string password = "GatewayTest!2026";
        Post("valid/nickname", {{"nickname", nickname}});
        Post("valid/email", {{"email", email}});
        Post("register", {{"nickname", nickname}, {"email", email}, {"password", password}});
        Post("valid/nickname", {{"nickname", nickname}}, ErrorCode::USER_NICKNAME_TAKEN);
        Post("valid/email", {{"email", email}}, ErrorCode::USER_EMAIL_TAKEN);
        Post("register", {{"nickname", nickname}, {"email", "another-" + email}, {"password", password}},
             ErrorCode::USER_NICKNAME_TAKEN);

        chat2Data::UserData users(database, redis);
        const auto registered = users.FindByEmail(email);
        ASSERT_TRUE(registered);
        ASSERT_FALSE(registered->userId.empty());
        const auto user_id = Track("user", registered->userId);
        EXPECT_EQ(registered->status, 0);

        Post("passwd/login", {{"nickname", nickname}, {"password", "incorrect"}}, ErrorCode::USER_INVALID_LOGIN);
        const auto first = Post("passwd/login", {{"nickname", nickname}, {"password", password}});
        const auto first_id = first["result"]["sessionId"].asString();
        ASSERT_FALSE(first_id.empty());
        Track("session", first_id);
        const auto second = Post("passwd/login", {{"nickname", email}, {"password", password}});
        const auto second_id = second["result"]["sessionId"].asString();
        ASSERT_FALSE(second_id.empty());
        Track("session", second_id);
        EXPECT_TRUE(first_id != second_id);

        chat2Data::SessionData sessions(database, redis);
        const auto before_login = sessions.Find(first_id);
        ASSERT_TRUE(before_login.session);
        EXPECT_FALSE(Post("session/login", {{"sessionId", first_id}}).isMember("result"));
        const auto after_login = sessions.Find(first_id);
        ASSERT_TRUE(after_login.session);
        EXPECT_EQ(after_login.session->expiresAt, before_login.session->expiresAt);

        const auto profile = GetInfo(first_id);
        EXPECT_EQ(profile.size(), 3u);
        EXPECT_EQ(profile["userId"].asString(), user_id);
        EXPECT_EQ(profile["nickname"].asString(), nickname);
        EXPECT_EQ(profile["email"].asString(), email);

        // 验证码通过通知 RPC 交付，HTTP 仅返回验证码 ID。
        Post("code", {{"email", "unknown-" + email}}, ErrorCode::USER_NOT_FOUND);
        const auto code_reply = Post("code", {{"email", email}});
        EXPECT_EQ(code_reply["result"].size(), 1u);
        const auto code_id = code_reply["result"]["codeId"].asString();
        ASSERT_FALSE(code_id.empty());
        Track("verifyCode", code_id);
        const auto request = notification.Received();
        ASSERT_EQ(request.request_id(), code_reply["requestId"].asString());
        EXPECT_EQ(request.email(), email);
        const auto &code = request.verify_code();
        ASSERT_EQ(code.size(), 6u);
        ASSERT_EQ(code.find_first_not_of("0123456789"), std::string::npos);
        const auto wrong_code = code == "000000" ? "000001" : "000000";
        Post("vcode/login", {{"email", email}, {"codeId", code_id}, {"verifyCode", wrong_code}},
             ErrorCode::USER_CODE_MISMATCH);
        const auto third = Post("vcode/login", {{"email", email}, {"codeId", code_id}, {"verifyCode", code}});
        const auto third_id = third["result"]["sessionId"].asString();
        ASSERT_FALSE(third_id.empty());
        Track("session", third_id);
        EXPECT_TRUE(third_id != first_id && third_id != second_id);
        Post("vcode/login", {{"email", email}, {"codeId", code_id}, {"verifyCode", code}},
             ErrorCode::USER_CODE_MISSING);

        Post("logout", {{"sessionId", first_id}});
        GetInfo(first_id, ErrorCode::USER_SESSION_NOT_FOUND, 401);
        Post("logout", {{"sessionId", first_id}}, ErrorCode::USER_SESSION_NOT_FOUND, 401);
        EXPECT_EQ(GetInfo(second_id)["userId"].asString(), user_id);
        Post("logout", {{"sessionId", second_id}});
        EXPECT_EQ(GetInfo(third_id)["userId"].asString(), user_id);
        const auto online = users.FindByEmail(email);
        ASSERT_TRUE(online);
        EXPECT_EQ(online->status, 1);
        Post("logout", {{"sessionId", third_id}});
        GetInfo(third_id, ErrorCode::USER_SESSION_NOT_FOUND, 401);
        const auto offline = users.FindByEmail(email);
        ASSERT_TRUE(offline);
        EXPECT_EQ(offline->status, 0);
        EXPECT_TRUE(sessions.FindValidByUserId(user_id).empty());
    }

    // 同一注册名换端口重新上线后，网关应自动恢复 HTTP 转发。
    TEST_F(GatewayUserTest, DiscoversUserServiceAndReconnectsAfterRestart)
    {
        ServiceProcess gateway;
        ASSERT_NO_FATAL_FAILURE(StartGateway(gateway));
        ExpectInvalidRequests();
        Post("valid/nickname", {{"nickname", prefix}}, ErrorCode::GATEWAY_USER_RPC_FAILED, 503);

        butil::fd_guard first_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        butil::fd_guard second_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int first_port = ReservePort(first_socket);
        const int second_port = ReservePort(second_socket);
        ASSERT_GT(first_port, 0);
        ASSERT_GT(second_port, 0);

        ServiceProcess first;
        first_socket.reset(-1);
        ASSERT_NO_FATAL_FAILURE(StartUser(first, first_port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(gateway, first, first_port));
        Post("valid/nickname", {{"nickname", prefix}});
        ASSERT_NO_FATAL_FAILURE(ExpectOffline(gateway, first, first_port));
        Post("valid/nickname", {{"nickname", prefix}}, ErrorCode::GATEWAY_USER_RPC_FAILED, 503);

        ServiceProcess second;
        second_socket.reset(-1);
        ASSERT_NO_FATAL_FAILURE(StartUser(second, second_port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(gateway, second, second_port));
        Post("valid/nickname", {{"nickname", prefix}});
        ASSERT_NO_FATAL_FAILURE(ExpectOffline(gateway, second, second_port));
        EXPECT_TRUE(gateway.Stop()) << gateway.Logs();
    }
}
