#pragma once

#include <butil/endpoint.h>
#include <butil/fd_guard.h>
#include <butil/logging.h>

#include "../svc_userService/testSupport.hpp"
#include "BreezeLog.hpp"
#include "utils.hpp"

#include <etcd/Client.hpp>
#include <etcd/v3/action_constants.hpp>
#include <httplib.h>
#include <json/json.h>

#include <arpa/inet.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char **environ;

namespace gateway_user_test
{
    using namespace std::chrono_literals;
    using chat2Data::ErrorCode;

    constexpr char kLoopback[] = "127.0.0.1";
    constexpr char kEtcdAddress[] = "http://dev-etcd:2379";

    // 保留系统分配的端口，直到对应的服务即将启动。
    inline int ReservePort(int fd)
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

    // 启动实际服务进程，并在用例退出时回收。
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
            args.emplace_back("--log_level=off");
            args.emplace_back("--minloglevel=2");

            std::vector<char *> argv;
            for (auto &arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);

            pid_t pid = -1;
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

            // 先给服务机会结束监听并释放租约。
            kill(_pid, SIGTERM);
            int status = 0;
            if (!WaitUntil([&]
                           { return waitpid(_pid, &status, WNOHANG) == _pid; }))
            {
                ERR("等待测试进程 {} 退出超时", _pid);
                return false;
            }

            _pid = -1;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

    private:
        pid_t _pid = -1;
    };

    // 用户链路测试共用独立数据库、真实网关和用户进程，以及 HTTP 响应校验。
    class GatewayUserTestFixture : public user_data_test::DataTestFixture
    {
    protected:
        void SetUp() override
        {
            BreezeLog::Logger::setLevel(spdlog::level::info);
            logging::SetMinLogLevel(logging::BLOG_WARNING);
            prefix = "gw" + chat2Data::Utils::GenerateUUID();
            _schema = "gateway_test_" + prefix;
            _admin = OpenMysql(FLAGS_mysql_database);
            INF("准备独立的联调数据库");
            // 用户服务在独立进程中读取普通表，测试使用专用数据库隔离数据。
            _admin->connection()->execute("CREATE DATABASE `" + _schema + "` CHARACTER SET utf8mb4");
            _owns_schema = true;
            database = OpenMysql(_schema);
            OpenRedis();
            ASSERT_NO_FATAL_FAILURE(CreateUserTables(false));
            _etcd.set_grpc_timeout(1s);
        }

        void TearDown() override
        {
            // 用例内的服务进程先退出，再清理测试数据库和 Redis 键。
            EXPECT_NO_THROW(DataTestFixture::TearDown());
            database.reset();
            if (_owns_schema)
                _admin->connection()->execute("DROP DATABASE `" + _schema + "`");
        }

        // 启动网关服务，等待健康接口就绪。
        void StartGateway(ServiceProcess &gateway)
        {
            butil::fd_guard reservation(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
            const int port = ReservePort(reservation);
            ASSERT_GT(port, 0);
            reservation.reset(-1);
            INF("启动网关，HTTP 端口 {}", port);
            ASSERT_TRUE(gateway.Start({GATEWAY_TEST_EXECUTABLE, "--gateway_address=127.0.0.1",
                                       "--gateway_port=" + std::to_string(port), "--user_service=" + ServiceName(),
                                       std::string("--etcd_address=") + kEtcdAddress}));
            _http = std::make_unique<httplib::Client>(kLoopback, port);
            _http->set_connection_timeout(1, 0);
            // 密码登录执行哈希校验，给开发容器留出处理时间。
            _http->set_read_timeout(10, 0);
            ASSERT_TRUE(WaitUntil([&]
                                  {
                const auto health = _http->Get("/health");
                return health && health->status == 200; }))
                << "网关健康接口未就绪";
        }

        void StartUser(ServiceProcess &user, int port)
        {
            INF("启动用户服务，RPC 端口 {}", port);
            ASSERT_TRUE(user.Start({USER_SERVICE_EXECUTABLE, "--user_port=" + std::to_string(port),
                                    "--user_address=127.0.0.1:" + std::to_string(port),
                                    "--user_service=" + ServiceName(), "--watch_service=" + NotificationName(),
                                    std::string("--etcd_address=") + kEtcdAddress, "--mysql_database=" + _schema}));
        }

        void ExpectDiscovered(int port)
        {
            // 查询能成功说明网关已建立信道，etcd 记录用于核实节点地址和租约。
            ASSERT_TRUE(WaitForHttpStatus(200)) << "网关未能转发用户 HTTP 请求";
            const auto address = "127.0.0.1:" + std::to_string(port);
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get();
            ASSERT_TRUE(registration.is_ok()) << registration.error_message();
            ASSERT_EQ(registration.values().size(), 1u);
            const auto &node = registration.value(0);
            ASSERT_EQ(node.as_string(), address);
            ASSERT_NE(node.lease(), 0);
            INF("已发现用户实例 {}，网关转发正常", address);
        }

        void ExpectOffline(ServiceProcess &user)
        {
            ASSERT_TRUE(user.Stop()) << "用户服务未正常退出";
            // 租约失效后，网关应移除节点并返回 HTTP 503。
            ASSERT_TRUE(WaitForHttpStatus(503)) << "用户实例下线后，网关仍未返回 HTTP 503";
            const auto registration = _etcd.ls("/" + ServiceName() + "/").get();
            EXPECT_TRUE(registration.is_ok() || registration.error_code() == etcdv3::ERROR_KEY_NOT_FOUND)
                << registration.error_message();
            ASSERT_TRUE(registration.values().empty());
            INF("用户实例已下线，etcd 注册项已清理");
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
            ExpectReply(_http->Post("/api/user/valid/email", "[]", "application/json"), "",
                        ErrorCode::USER_INVALID_INPUT, 400);
            ExpectReply(_http->Post("/api/user/valid/email",
                                    R"({"requestId":"numeric-email","email":42})", "application/json"),
                        "numeric-email", ErrorCode::USER_INVALID_INPUT, 400);
            ExpectReply(_http->Get("/api/user/info?requestId=missing-session"), "missing-session",
                        ErrorCode::USER_INVALID_INPUT, 400);
        }

        // 每个用例使用独立的服务名称，让发现范围与本用例的实例对应。
        std::string ServiceName() const { return "gateway-user-" + prefix; }
        std::string NotificationName() const { return "gateway-notify-" + prefix; }
        etcd::Client _etcd{kEtcdAddress};

    private:
        std::string NextRequestId() { return prefix + "-" + std::to_string(++_request_sequence); }

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

        std::shared_ptr<odb::database> _admin;
        std::string _schema;
        bool _owns_schema = false;
        unsigned _request_sequence = 0;
        std::unique_ptr<httplib::Client> _http;
    };
}
