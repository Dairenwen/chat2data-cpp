#include "BreezeRPC.hpp"
#include "notifyService.pb.h"
#include "testSupport.hpp"
#include "userBusiness.hpp"
#include "userServiceImpl.hpp"

#include <brpc/closure_guard.h>

namespace
{
    using namespace chat2Data;

    constexpr char kTestNotificationService[] = "rpcTestNotification";

    // 本地通知节点确认 RPC 请求，供验证码登录流程使用。
    class NotifyReply : public NotifyService
    {
    public:
        void SendVerifyCode(google::protobuf::RpcController *, const SendVerifyCodeRequest *request,
                            SendVerifyCodeResponse *response, google::protobuf::Closure *done) override
        {
            brpc::ClosureGuard guard(done);
            response->set_request_id(request->request_id());
            // 模拟通知服务的发信结果。请求 ID 为 "failed" 时返回失败，其余情况均返回成功。
            // 未真实发送邮件
        }
    };

    class UserServiceImplTest : public user_data_test::DataTestFixture
    {
    protected:
        void SetUp() override
        {
            DataTestFixture::SetUp();
            if (HasFatalFailure())
                return;

            codes = std::make_shared<VerifyCodeData>(redis);
            channels = std::make_shared<SvcChannels>();
            const auto sessions = std::make_shared<SessionManager>(std::make_shared<SessionData>(database, redis));
            auto business = std::make_shared<UserBusiness>(
                sessions, std::make_shared<UserData>(database, redis), codes, channels, kTestNotificationService);
            service = std::make_unique<chat2data::UserServiceImpl>(business);
            ASSERT_EQ(server.AddService(service.get(), brpc::SERVER_DOESNT_OWN_SERVICE), 0);
            ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
            brpc::ChannelOptions options;
            // 密码派生和延迟双删需要比默认 RPC 超时更长的测试窗口。
            options.timeout_ms = 15000;
            options.max_retry = 0;
            ASSERT_EQ(channel.Init(server.listen_address(), &options), 0);
        }

        template <typename Request, typename Response>
        Response Call(void (UserService_Stub::*method)(google::protobuf::RpcController *, const Request *,
                                                       Response *, google::protobuf::Closure *),
                      const Request &request, ErrorCode expected = ErrorCode::SUCCESS)
        {
            UserService_Stub stub(&channel);
            brpc::Controller controller;
            Response response;
            (stub.*method)(&controller, &request, &response, nullptr);
            EXPECT_FALSE(controller.Failed()) << controller.ErrorText();
            EXPECT_EQ(response.request_id(), request.request_id());
            EXPECT_EQ(response.error_code(), static_cast<int>(expected));
            EXPECT_EQ(response.error_msg(), expected == ErrorCode::SUCCESS ? "" : error2String(expected));
            return response;
        }

        std::shared_ptr<VerifyCodeData> codes;
        std::shared_ptr<SvcChannels> channels;
        std::unique_ptr<chat2data::UserServiceImpl> service;
        brpc::Server server;
        brpc::Channel channel;
    };

    // 使用本地 brpc 客户端调用全部用户 RPC，检查响应公共字段和业务结果。
    TEST_F(UserServiceImplTest, ServesAllUserRpcMethods)
    {
        NotifyReply notification;
        brpc::Server notify_server;
        ASSERT_EQ(notify_server.AddService(&notification, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(notify_server.Start("127.0.0.1:0", nullptr), 0);
        // 使用非默认服务名，验证验证码接口按配置查找通知通道。
        channels->setWatch(kTestNotificationService);
        channels->addNode(kTestNotificationService, "127.0.0.1:" + std::to_string(notify_server.listen_address().port));
        const auto email = prefix + "@example.com";

        ValidNicknameRequest nickname_request;
        nickname_request.set_request_id(prefix);
        nickname_request.set_nickname(prefix);
        Call(&UserService_Stub::ValidNickname, nickname_request);

        ValidEmailRequest email_request;
        email_request.set_request_id(prefix);
        email_request.set_email(email);
        Call(&UserService_Stub::ValidEmail, email_request);

        UserRegisterRequest registration;
        registration.set_request_id(prefix);
        registration.set_nickname(prefix);
        registration.set_email(email);
        registration.set_password("RpcPassword2026!");
        Call(&UserService_Stub::UserRegister, registration);
        Call(&UserService_Stub::ValidNickname, nickname_request, ErrorCode::USER_NICKNAME_TAKEN);
        Call(&UserService_Stub::ValidEmail, email_request, ErrorCode::USER_EMAIL_TAKEN);

        PasswdLoginRequest password_login;
        password_login.set_request_id(prefix);
        password_login.set_nickname(email);
        password_login.set_password(registration.password());
        const auto password_result = Call(&UserService_Stub::PasswdLogin, password_login);
        ASSERT_FALSE(password_result.session_id().empty());
        const auto session_id = Track("session", password_result.session_id());

        SessionLoginRequest session_login;
        session_login.set_request_id(prefix);
        session_login.set_session_id(session_id);
        Call(&UserService_Stub::SessionLogin, session_login);

        GetUserInfoRequest info_request;
        info_request.set_request_id(prefix);
        info_request.set_session_id(session_id);
        const auto info = Call(&UserService_Stub::GetUserInfo, info_request);
        ASSERT_TRUE(info.has_user_info());
        ASSERT_FALSE(info.user_info().user_id().empty());
        const auto user_id = Track("user", info.user_info().user_id());
        EXPECT_EQ(info.user_info().nickname(), prefix);
        EXPECT_EQ(info.user_info().email(), email);

        IsSessionValidRequest validation;
        validation.set_request_id(prefix);
        validation.set_session_id(session_id);
        const auto valid = Call(&UserService_Stub::IsSessionValid, validation);
        EXPECT_TRUE(valid.valid());
        EXPECT_EQ(valid.user_id(), user_id);

        GetCodeRequest code_request;
        code_request.set_request_id(prefix);
        code_request.set_email(email);
        const auto code_result = Call(&UserService_Stub::GetCode, code_request);
        ASSERT_FALSE(code_result.code_id().empty());
        const auto code_id = Track("verifyCode", code_result.code_id());
        const auto saved = codes->Find(code_id);
        ASSERT_TRUE(saved);

        VcodeLoginRequest code_login;
        code_login.set_request_id(prefix);
        code_login.set_email(email);
        code_login.set_code_id(code_id);
        code_login.set_verify_code(saved->verifyCode);
        const auto code_session = Call(&UserService_Stub::VcodeLogin, code_login);
        ASSERT_FALSE(code_session.session_id().empty());
        Track("session", code_session.session_id());
        EXPECT_NE(code_session.session_id(), session_id);
        const auto consumed = Call(&UserService_Stub::VcodeLogin, code_login, ErrorCode::USER_CODE_MISSING);
        EXPECT_TRUE(consumed.session_id().empty());

        LogoutRequest logout_request;
        logout_request.set_request_id(prefix);
        logout_request.set_session_id(session_id);
        Call(&UserService_Stub::Logout, logout_request);
        const auto invalid = Call(&UserService_Stub::IsSessionValid, validation);
        EXPECT_FALSE(invalid.valid());
        EXPECT_TRUE(invalid.user_id().empty());
        const auto missing_user = Call(&UserService_Stub::GetUserInfo, info_request, ErrorCode::USER_SESSION_NOT_FOUND);
        EXPECT_FALSE(missing_user.has_user_info());
    }

    TEST_F(UserServiceImplTest, EnforcesEmailSyntaxAndLengthLimits)
    {
        const auto domain = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(61, 'c');
        const auto max_length_email = std::string(64, 'a') + "@" + domain;
        ASSERT_EQ(max_length_email.size(), 254u);
        const std::string valid_emails[] = {
            "alice.test+tag@Mail-2.example.test",
            "o'connor@example.test",
            "alice@xn--bcher-kva.example",
            max_length_email,
        };
        const std::string invalid_emails[] = {
            "", "。", "@example.test", "alice@", "alice@@example.test",
            ".alice@example.test", "alice.@example.test", "a..b@example.test",
            "\"alice@example.test", "a,b@example.test", "a;b@example.test",
            "<alice@example.test", "a\\b@example.test", "a b@example.test",
            "alice@.example.test", "alice@example..test", "alice@example.test.",
            "alice@-example.test", "alice@example-.test", "alice@ex_ample.test", "alice@exam/ple.test",
            "alice@...", "alice@localhost", "alice@127.0.0.1", "alice@[127.0.0.1]",
            "alice@example.123", "alice@example。test", "alice@example.test。", "爱丽丝@example.test",
            u8"alice@exam\u00a0ple.test", u8"alice@exam\u200bple.test", "alice@example.test\r\n",
            std::string("alice\0@example.test", sizeof("alice\0@example.test") - 1),
            std::string(65, 'a') + "@example.test",
            "alice@" + std::string(64, 'a') + ".test",
            max_length_email + 'c',
        };

        ValidEmailRequest request;
        request.set_request_id(prefix);
        for (const auto &email : valid_emails)
        {
            SCOPED_TRACE(email);
            request.set_email(email);
            Call(&UserService_Stub::ValidEmail, request);
        }
        for (const auto &email : invalid_emails)
        {
            SCOPED_TRACE(email);
            request.set_email(email);
            Call(&UserService_Stub::ValidEmail, request, ErrorCode::USER_INVALID_INPUT);
        }
    }

    TEST_F(UserServiceImplTest, RejectsMalformedEmailDuringRegistrationAndCodeLogin)
    {
        const std::string email = "alice@example.test。";
        UserRegisterRequest registration;
        registration.set_request_id(prefix);
        registration.set_nickname(prefix);
        registration.set_email(email);
        registration.set_password("RpcPassword2026!");
        Call(&UserService_Stub::UserRegister, registration, ErrorCode::USER_INVALID_INPUT);

        GetCodeRequest get_code;
        get_code.set_request_id(prefix);
        get_code.set_email(email);
        const auto code = Call(&UserService_Stub::GetCode, get_code, ErrorCode::USER_INVALID_INPUT);
        EXPECT_TRUE(code.code_id().empty());

        VcodeLoginRequest login;
        login.set_request_id(prefix);
        login.set_email(email);
        login.set_code_id(prefix);
        login.set_verify_code("123456");
        Call(&UserService_Stub::VcodeLogin, login, ErrorCode::USER_INVALID_INPUT);
    }

    // 参数和底层依赖异常由响应错误码报告，失败响应保留业务字段的默认值。
    TEST_F(UserServiceImplTest, ReturnsProtocolErrorsWithoutResultFields)
    {
        PasswdLoginRequest login;
        login.set_request_id(prefix);
        const auto invalid = Call(&UserService_Stub::PasswdLogin, login, ErrorCode::USER_INVALID_INPUT);
        EXPECT_TRUE(invalid.session_id().empty());

        ValidNicknameRequest nickname;
        nickname.set_nickname(prefix);
        Call(&UserService_Stub::ValidNickname, nickname, ErrorCode::USER_INVALID_INPUT);

        IsSessionValidRequest validation;
        validation.set_request_id(prefix);
        const auto session_id = Track("session", prefix);
        redis->set("session:" + session_id, "unexpected-cache-type");
        validation.set_session_id(session_id);
        const auto failure = Call(&UserService_Stub::IsSessionValid, validation, ErrorCode::USER_BUSINESS_FAILED);
        EXPECT_FALSE(failure.valid());
        EXPECT_TRUE(failure.user_id().empty());
    }
}
