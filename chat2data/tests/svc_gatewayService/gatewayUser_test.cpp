#include <brpc/server.h> // brpc 的 Redis 协议头需要先于 hiredis

#include "../svc_notifyService/gatewayUserTestSupport.hpp"
#include "notifyService.pb.h"
#include "sessionData.hpp"

#include <brpc/closure_guard.h>
#include <cstdint>
#include <mutex>

namespace
{
    using namespace gateway_user_test;

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

    class GatewayUserTest : public GatewayUserTestFixture
    {
    protected:
        void TearDown() override
        {
            if (_notification_lease)
                EXPECT_TRUE(_etcd.leaserevoke(_notification_lease).get().is_ok());
            GatewayUserTestFixture::TearDown();
        }

        // 为测试通知接收端注册独立节点，租约在用例结束后撤销。
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

    private:
        std::int64_t _notification_lease = 0;
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
