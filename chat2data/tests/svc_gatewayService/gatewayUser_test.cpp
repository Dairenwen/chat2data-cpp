#include <brpc/server.h> // brpc 的 Redis 协议头需要先于 hiredis

#include "../svc_userService/testSupport.hpp"
#include "BreezeLog.hpp"
#include "notifyService.pb.h"
#include "sessionData.hpp"
#include "utils.hpp"

#include <brpc/closure_guard.h>
#include <butil/endpoint.h>
#include <butil/fd_guard.h>
#include <butil/logging.h>
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
#include <cstdint>
#include <cstring>
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

    constexpr char kLoopback[] = "127.0.0.1";               // 网关和用户服务在同一容器内运行，使用环回地址互相访问
    constexpr char kEtcdAddress[] = "http://dev-etcd:2379"; // etcd的容器地址

    // 保留系统分配的端口，直到对应的服务即将启动
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

    // 等待条件成立或超时，返回是否成功
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

    // 负责启动和关闭真正的网关、用户服务可执行程序，并确保测试结束时清理进程。
    class ServiceProcess
    {
    public:
        ServiceProcess() = default;
        ServiceProcess(const ServiceProcess &) = delete;
        ServiceProcess &operator=(const ServiceProcess &) = delete;

        ~ServiceProcess()
        {
            // 正常退出超时时强制回收，避免测试实例继续占用端口。
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
            // 准备创建一个新进程，传递参数和环境变量，关闭日志输出
            args.emplace_back("--log_level=off"); // 关闭 BreezeLog 日志
            args.emplace_back("--minloglevel=2"); // 关闭 glog 日志

            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            pid_t pid = -1;
            // 执行另一个可执行程序，传递参数和环境变量，返回新进程的 PID
            const int error = posix_spawn(&pid, argv.front(), nullptr, nullptr, argv.data(), environ);
            if (error != 0)
            {
                ERR("启动测试进程 {} 失败，{}", args.front(), std::strerror(error));
                return false;
            }
            _pid = pid;
            return true;
        }

        bool Stop()
        {
            if (_pid <= 0)
                return true;

            // 模拟正常停服，等待服务完成清理并退出，发送信号让进程终止
            kill(_pid, SIGTERM);
            int status = 0;
            // 使用 waitpid() 等待子进程退出，WNOHANG 表示非阻塞模式，如果子进程还没有退出，则立即返回
            if (!WaitUntil([&]
                           { return waitpid(_pid, &status, WNOHANG) == _pid; }))
            {
                ERR("等待测试进程 {} 退出超时", _pid);
                return false;
            }

            _pid = -1;
            // 返回子进程是否正常退出：
            // WIFEXITED(status) 检查子进程是否正常退出，WEXITSTATUS(status) 获取子进程的退出状态码
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

    private:
        pid_t _pid = -1;
    };

    // 模拟通知子服务的 RPC 接口，接收验证码请求并保存到本地，供测试线程读取，这里还没有真实实现
    class NotificationReceiver final : public chat2Data::NotifyService
    {
    public:
        void SendVerifyCode(google::protobuf::RpcController *, const chat2Data::SendVerifyCodeRequest *request,
                            chat2Data::SendVerifyCodeResponse *response, google::protobuf::Closure *done) override
        {
            brpc::ClosureGuard guard(done);
            // RPC 工作线程写入，测试线程通过 Received 读取。
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

    // 负责准备测试环境、启动服务、发送 HTTP 请求，以及检查响应和服务发现结果
    // 这里继承了已经实现的 DataTestFixture，提供了数据库和 Redis 的初始化和清理功能
    class GatewayUserTest : public user_data_test::DataTestFixture
    {
    protected:
        // 在每一个test_f开始前执行
        void SetUp() override
        {
            // 1. 设置日志等级。
            BreezeLog::Logger::setLevel(spdlog::level::info);
            logging::SetMinLogLevel(logging::BLOG_WARNING);
            // 2. 生成唯一的测试前缀。
            prefix = "gw" + chat2Data::Utils::GenerateUUID();
            _schema = "gateway_test_" + prefix;
            _admin = OpenMysql(FLAGS_mysql_database);
            INF("准备独立的联调数据库");
            // 3. 创建独立的 MySQL 测试数据库。
            _admin->connection()->execute("CREATE DATABASE `" + _schema + "` CHARACTER SET utf8mb4");
            _owns_schema = true;
            // 4. 连接该数据库。
            database = OpenMysql(_schema);
            // 5. 初始化 Redis 连接。
            // 6. 创建用户表、会话表。
            OpenRedis();
            ASSERT_NO_FATAL_FAILURE(CreateUserTables(false));
            // 7. 设置 etcd 请求超时时间。
            _etcd.set_grpc_timeout(1s);
        }

        // 每一个test_f结束后执行
        void TearDown() override
        {
            // 两个服务进程在用例退出时先回收，此处清理测试数据。
            if (_notification_lease)
                EXPECT_TRUE(_etcd.leaserevoke(_notification_lease).get().is_ok());
            // 调用 DataTestFixture::TearDown() 清理数据库和 Redis 连接
            EXPECT_NO_THROW(DataTestFixture::TearDown());
            database.reset();
            // 删除独立的 MySQL 测试数据库，避免测试数据污染
            if (_owns_schema)
                _admin->connection()->execute("DROP DATABASE `" + _schema + "`");
        }

        // 启动网关服务，等待健康接口就绪
        void StartGateway(ServiceProcess &gateway)
        {
            // 进程启动前保留端口，避免端口被其他进程占用
            butil::fd_guard reservation(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
            const int port = ReservePort(reservation);
            ASSERT_GT(port, 0);
            reservation.reset(-1); // 释放保留的端口，网关进程将绑定该端口
            INF("启动网关，HTTP 端口 {}", port);
            // 启动网关进程，传递参数，包括网关地址、端口、用户服务名称和 etcd 地址
            ASSERT_TRUE(gateway.Start({GATEWAY_TEST_EXECUTABLE, "--gateway_address=127.0.0.1",
                                       "--gateway_port=" + std::to_string(port), "--user_service=" + ServiceName(),
                                       std::string("--etcd_address=") + kEtcdAddress}));
            // 创建 HTTP 客户端，连接到网关的 HTTP 端口
            _http = std::make_unique<httplib::Client>(kLoopback, port);
            _http->set_connection_timeout(1, 0);
            // 密码登录执行哈希校验，给开发容器留出处理时间。
            _http->set_read_timeout(10, 0);
            // 与客户端一样通过健康接口确认网关已开始接收 HTTP 请求。
            ASSERT_TRUE(WaitUntil([&]
                                  {
                const auto health = _http->Get("/health");
                return health && health->status == 200; }))
                << "网关健康接口未就绪";
        }

        // 向 etcd 发布通知服务的注册信息，让用户服务通过真实的 etcd 发现测试接收端
        void PublishNotification(int port)
        {
            const auto lease = _etcd.leasegrant(120).get(); // 向 etcd 请求一个 120 秒的租约，用于注册通知服务的节点
            ASSERT_TRUE(lease.is_ok()) << lease.error_message();
            _notification_lease = lease.value().lease();
            ASSERT_NE(_notification_lease, 0);
            // 将通知服务的地址和端口注册到 etcd 的指定路径下，使用租约保证节点在租约过期后自动删除
            const auto node = _etcd.add("/" + NotificationName() + "/receiver",
                                        "127.0.0.1:" + std::to_string(port), _notification_lease)
                                  .get();
            ASSERT_TRUE(node.is_ok()) << node.error_message();
        }

        void StartUser(ServiceProcess &user, int port)
        {
            INF("启动用户服务，RPC 端口 {}", port);
            // 启动用户子服务，传入相应参数
            ASSERT_TRUE(user.Start({USER_SERVICE_EXECUTABLE, "--user_port=" + std::to_string(port),
                                    "--user_address=127.0.0.1:" + std::to_string(port),
                                    "--user_service=" + ServiceName(), "--watch_service=" + NotificationName(),
                                    std::string("--etcd_address=") + kEtcdAddress, "--mysql_database=" + _schema}));
        }

        // 检查服务是否被发现
        void ExpectDiscovered(int port)
        {
            // 查询能成功说明网关已建立信道，etcd 记录用于核实节点地址和租约。
            ASSERT_TRUE(WaitForHttpStatus(200)) << "网关未能转发用户 HTTP 请求";
            const auto address = "127.0.0.1:" + std::to_string(port);
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get(); // 查询 etcd 中注册的用户服务节点
            ASSERT_TRUE(registration.is_ok()) << registration.error_message();
            ASSERT_EQ(registration.values().size(), 1u);
            const auto &node = registration.value(0);
            ASSERT_EQ(node.as_string(), address);
            ASSERT_NE(node.lease(), 0);
            INF("已发现用户实例 {}，网关转发正常", address);
        }

        // 检查服务下线
        void ExpectOffline(ServiceProcess &user)
        {
            ASSERT_TRUE(user.Stop()) << "用户服务未正常退出";
            // 租约失效后，网关应移除节点并返回 HTTP 503
            ASSERT_TRUE(WaitForHttpStatus(503)) << "用户实例下线后，网关仍未返回 HTTP 503";
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get();
            EXPECT_TRUE(registration.is_ok() || registration.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
                << registration.error_message();
            ASSERT_TRUE(registration.values().empty());
            INF("用户实例已下线，etcd 注册项已清理");
        }

        // 封装http post请求
        Json::Value Post(const std::string &route,
                         std::initializer_list<std::pair<const char *, std::string>> fields,
                         ErrorCode code = ErrorCode::SUCCESS, int status = 200)
        {
            SCOPED_TRACE(route);
            Json::Value body(Json::objectValue);
            const auto request_id = NextRequestId(); // 生成唯一的请求 ID，便于追踪请求和响应
            body["requestId"] = request_id;
            // 将传入的字段拆分为 JSON 对象，便于构造请求体
            for (const auto &[name, value] : fields)
                body[name] = value;
            const auto reply = _http->Post("/api/user/" + route, Json::writeString(Json::StreamWriterBuilder{}, body),
                                           "application/json");
            return ExpectReply(reply, request_id, code, status);
        }

        // 获取用户信息
        Json::Value GetInfo(const std::string &session_id, ErrorCode code = ErrorCode::SUCCESS, int status = 200)
        {
            SCOPED_TRACE("GET /api/user/info");
            const auto request_id = NextRequestId();
            const httplib::Params query{{"requestId", request_id}, {"sessionId", session_id}};
            // 发送get请求到网关的用户信息接口，携带请求 ID 和会话 ID 作为查询参数
            const auto reply = _http->Get("/api/user/info", query, httplib::Headers{});
            return ExpectReply(reply, request_id, code, status)["result"]["userInfo"];
        }

        // 验证无效请求的处理
        void ExpectInvalidRequests()
        {
            ExpectReply(_http->Post("/api/user/valid/email", "{", "application/json"), "",
                        ErrorCode::USER_INVALID_INPUT, 400);
            ExpectReply(_http->Get("/api/user/info?requestId=missing-session"), "missing-session",
                        ErrorCode::USER_INVALID_INPUT, 400);
        }

    private:
        // 生成唯一的测试前缀，便于区分不同测试实例的数据
        std::string ServiceName() const { return "gateway-user-" + prefix; }
        // 生成唯一的通知服务名称，便于用户服务发现通知接收端
        std::string NotificationName() const { return "gateway-notify-" + prefix; }
        // 生成唯一的请求 ID，便于追踪请求和响应
        std::string NextRequestId() { return prefix + "-" + std::to_string(++_request_sequence); }

        // 等待网关 HTTP 服务达到指定状态
        bool WaitForHttpStatus(int status)
        {
            // 轮询昵称校验接口，它会读库且适合重复请求
            return WaitUntil([&]
                             {
                const auto reply = _http->Post("/api/user/valid/nickname",
                                               R"({"requestId":"gateway-ready","nickname":"gateway-readiness"})",
                                               "application/json");
                return reply && reply->status == status; });
        }

        // 九个接口都应关联请求 ID，并用统一错误字段报告结果。
        static Json::Value ExpectReply(const httplib::Result &reply, const std::string &request_id,
                                       ErrorCode code, int status)
        {
            if (!reply)
            {
                ADD_FAILURE() << "网关 HTTP 请求失败，" << httplib::to_string(reply.error());
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
        // 1. 创建通知接收端，启动 RPC 服务，发布通知服务到 etcd，供用户服务发现
        NotificationReceiver notification;
        brpc::Server notification_server;
        ASSERT_EQ(notification_server.AddService(&notification, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(notification_server.Start("127.0.0.1:0", nullptr), 0);
        ASSERT_NO_FATAL_FAILURE(PublishNotification(notification_server.listen_address().port));
        // 2. 启动网关和用户服务，等待网关发现用户服务并转发 HTTP 请求
        ServiceProcess gateway;
        ASSERT_NO_FATAL_FAILURE(StartGateway(gateway));
        butil::fd_guard reservation(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int port = ReservePort(reservation);
        ASSERT_GT(port, 0);
        reservation.reset(-1);
        ServiceProcess user;
        ASSERT_NO_FATAL_FAILURE(StartUser(user, port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(port));
        // 3.测试注册功能
        const auto nickname = prefix;
        const auto email = prefix + "@example.com";
        const std::string password = "GatewayTest!2026";
        // 先校验昵称和邮箱，再注册，后续查询应看到占用状态。
        INF("验证昵称、邮箱校验和注册");
        Post("valid/nickname", {{"nickname", nickname}});
        Post("valid/email", {{"email", email}});
        Post("register", {{"nickname", nickname}, {"email", email}, {"password", password}});
        Post("valid/nickname", {{"nickname", nickname}}, ErrorCode::USER_NICKNAME_TAKEN);
        Post("valid/email", {{"email", email}}, ErrorCode::USER_EMAIL_TAKEN);
        Post("register", {{"nickname", nickname}, {"email", "another-" + email}, {"password", password}},
             ErrorCode::USER_NICKNAME_TAKEN);
        // 4.注册成功后，用户数据应已写入数据库，状态为离线。
        chat2Data::UserData users(database, redis);
        const auto registered = users.FindByEmail(email);
        ASSERT_TRUE(registered);
        ASSERT_FALSE(registered->userId.empty());
        const auto user_id = Track("user", registered->userId);
        EXPECT_EQ(registered->status, 0);
        // 5.测试登录功能
        // 昵称和注册邮箱都能登录，每次登录创建独立会话。
        INF("验证密码登录、会话登录和资料查询");
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
        // 6.会话登录只恢复当前会话，到期时间沿用数据库中的原值，不能擅自延长会话有效期
        chat2Data::SessionData sessions(database, redis);
        // 会话登录只恢复当前会话，到期时间沿用数据库中的原值。
        const auto before_login = sessions.Find(first_id);
        ASSERT_TRUE(before_login.session);
        EXPECT_FALSE(Post("session/login", {{"sessionId", first_id}}).isMember("result"));
        const auto after_login = sessions.Find(first_id);
        ASSERT_TRUE(after_login.session);
        EXPECT_EQ(after_login.session->expiresAt, before_login.session->expiresAt);
        // 7.查询用户资料应返回注册时的昵称和邮箱，且不包含密码
        const auto profile = GetInfo(first_id);
        EXPECT_EQ(profile.size(), 3u);
        EXPECT_EQ(profile["userId"].asString(), user_id);
        EXPECT_EQ(profile["nickname"].asString(), nickname);
        EXPECT_EQ(profile["email"].asString(), email);
        // 8. 验证码功能测试
        //  验证码通过通知 RPC 交付，HTTP 仅返回验证码 ID。
        INF("验证验证码获取、登录和单次消费");
        Post("code", {{"email", "unknown-" + email}}, ErrorCode::USER_NOT_FOUND); // 未注册邮箱无法获取验证码
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
        // 9. 多设备登录和退出，网关应正确维护在线状态
        // 逐个撤销设备会话，最后一次退出后从 MySQL 核对离线状态。
        INF("验证多设备退出和在线状态");
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
        // 1. 启动网关，不启动用户服务，网关应返回 HTTP 503
        ServiceProcess gateway;
        ASSERT_NO_FATAL_FAILURE(StartGateway(gateway));
        ExpectInvalidRequests(); // 先验证无效请求的处理
        Post("valid/nickname", {{"nickname", prefix}}, ErrorCode::GATEWAY_USER_RPC_FAILED, 503);
        // 2. 启动两个用户服务实例，网关应发现并转发请求，随后关闭它们，网关应返回 HTTP 503
        butil::fd_guard first_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        butil::fd_guard second_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        const int first_port = ReservePort(first_socket);
        const int second_port = ReservePort(second_socket);
        ASSERT_GT(first_port, 0);
        ASSERT_GT(second_port, 0);

        ServiceProcess first;
        first_socket.reset(-1);
        ASSERT_NO_FATAL_FAILURE(StartUser(first, first_port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(first_port));
        Post("valid/nickname", {{"nickname", prefix}});
        // 3.下线第一个用户服务实例，网关应返回 HTTP 503
        ASSERT_NO_FATAL_FAILURE(ExpectOffline(first));
        Post("valid/nickname", {{"nickname", prefix}}, ErrorCode::GATEWAY_USER_RPC_FAILED, 503);
        // 4.使用另一个端口启动第二个用户服务实例，网关应发现并转发请求，随后关闭它，网关应返回 HTTP 503
        ServiceProcess second;
        second_socket.reset(-1);
        ASSERT_NO_FATAL_FAILURE(StartUser(second, second_port));
        ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(second_port));
        Post("valid/nickname", {{"nickname", prefix}});
        ASSERT_NO_FATAL_FAILURE(ExpectOffline(second));
        EXPECT_TRUE(gateway.Stop()) << "网关未正常退出";
    }
}
