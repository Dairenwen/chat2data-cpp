#include "localSmtpReceiver.hpp"

#include "BreezeCode.hpp"
#include "errorHandler.hpp"
#include "notifyBusiness.hpp"
#include "notifyServiceImpl.hpp"

#include <brpc/channel.h>
#include <brpc/server.h>
#include <future>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <utility>

DEFINE_string(smtp_username, "3484725023@qq.com", "真实 SMTP 测试的 QQ 邮箱账号");
DEFINE_string(smtp_password, "lshcqpcfdmilchjj", "真实 SMTP 测试的客户端授权码");
DEFINE_string(smtp_recipient, "1962478161@qq.com", "真实 SMTP 测试的收件人，留空时使用测试默认地址");

namespace
{
    using namespace chat2Data;
    using notify_test::DecodePart;
    using notify_test::LocalSmtpReceiver;
    constexpr char kRecipient[] = "1962478161@qq.com";

    // 两类测试共用实际的通知 RPC 实现和 BreezeCode 队列。
    class NotifyRpcTest : public testing::Test
    {
    protected:
        void StartRpc(const Breezecode::mail_settings &settings, const Breezecode::MailQueueOptions &queue,
                      int rpc_timeout_ms)
        {
            auto mail = std::make_unique<Breezecode::MailClient>(settings, queue);
            auto business = std::make_unique<chat2data::NotifyBusiness>(std::move(mail));
            _service = std::make_unique<chat2data::NotifyServiceImpl>(std::move(business), queue.task_timeout);
            ASSERT_EQ(_server.AddService(_service.get(), brpc::SERVER_DOESNT_OWN_SERVICE), 0);

            // 测试进程持有 SMTP 授权码，监听器只提供通知 RPC。
            brpc::ServerOptions server_options;
            server_options.has_builtin_services = false;
            ASSERT_EQ(_server.Start("127.0.0.1:0", &server_options), 0);

            brpc::ChannelOptions options;
            options.max_retry = 0;
            options.timeout_ms = rpc_timeout_ms;
            ASSERT_EQ(_channel.Init(_server.listen_address(), &options), 0);
        }

        template <typename Request, typename Response>
        void Call(void (NotifyService_Stub::*method)(google::protobuf::RpcController *, const Request *,
                                                     Response *, google::protobuf::Closure *),
                  const Request &request, ErrorCode expected)
        {
            NotifyService_Stub stub(&_channel);
            brpc::Controller controller;
            Response response;
            (stub.*method)(&controller, &request, &response, nullptr);
            // 先核对 RPC 传输状态，再校验业务错误码和请求 ID。
            ASSERT_FALSE(controller.Failed()) << controller.ErrorText();
            EXPECT_EQ(response.request_id(), request.request_id());
            EXPECT_EQ(response.error_code(), static_cast<int>(expected));
            EXPECT_EQ(response.error_msg(), expected == ErrorCode::SUCCESS ? "" : error2String(expected));
        }

    private:
        std::unique_ptr<chat2data::NotifyServiceImpl> _service;
        brpc::Server _server;
        brpc::Channel _channel;
    };

    class NotifyServiceTest : public NotifyRpcTest
    {
    protected:
        void SetUp() override
        {
            Breezecode::mail_settings settings;
            settings.url = _smtp.Url();
            settings.from = "sender@example.test";
            settings.html_path = NOTIFY_TEST_TEMPLATE;

            Breezecode::MailQueueOptions queue;
            queue.task_timeout = std::chrono::seconds(4);
            StartRpc(settings, queue, 6000);
        }

        LocalSmtpReceiver _smtp;
    };

    class NotifyServiceLiveTest : public NotifyRpcTest
    {
    protected:
        void SetUp() override
        {
            if (FLAGS_smtp_password.empty())
                GTEST_SKIP() << "真实投递需传入 --smtp_username 和 --smtp_password";
            ASSERT_FALSE(FLAGS_smtp_username.empty()) << "请通过 --smtp_username 指定发件账号";

            Breezecode::mail_settings settings;
            settings.url = "smtps://smtp.qq.com:465";
            settings.username = FLAGS_smtp_username;
            settings.password = FLAGS_smtp_password;
            settings.from = settings.username;
            settings.login_options = "AUTH=LOGIN";
            settings.html_path = NOTIFY_TEST_TEMPLATE;

            // 时间预算与通知服务默认配置一致，RPC 留出返回响应的余量。
            Breezecode::MailQueueOptions queue;
            queue.task_timeout = std::chrono::seconds(25);
            StartRpc(settings, queue, 30000);
        }

        std::string Recipient() const
        {
            return FLAGS_smtp_recipient.empty() ? kRecipient : FLAGS_smtp_recipient;
        }
    };

    // 通过 brpc 发送验证码，并从实际 SMTP 正文检查模板替换和前导零。
    TEST_F(NotifyServiceTest, DeliversVerificationCodeWithoutAttachment)
    {
        auto wire = std::async(std::launch::async, [&]
                               { return _smtp.Receive(true); });
        SendVerifyCodeRequest request;
        request.set_request_id("verify-code");
        request.set_email(kRecipient);
        request.set_verify_code("000123");
        Call(&NotifyService_Stub::SendVerifyCode, request, ErrorCode::SUCCESS);

        const auto message = wire.get();
        EXPECT_NE(message.find("To: " + request.email()), std::string::npos);
        const auto html = DecodePart(message, "Content-Type: text/html");
        EXPECT_NE(html.find("000123"), std::string::npos);
        EXPECT_EQ(html.find("{{code}}"), std::string::npos);
        EXPECT_EQ(message.find("Content-Disposition: attachment"), std::string::npos);
    }

    // 普通邮件的 HTML、空 MIME 类型及多个附件经过 brpc 和 SMTP 完整传递。
    TEST_F(NotifyServiceTest, DeliversHtmlAndBinaryAttachments)
    {
        auto wire = std::async(std::launch::async, [&]
                               { return _smtp.Receive(true); });
        SendEmailRequest request;
        request.set_request_id("report");
        request.set_to_email(kRecipient);
        request.set_subject("分析报告");
        request.set_content("<p>结果</p>");
        const std::string binary("\x00\xff\r\n", 4);
        auto *attachment = request.add_attachments();
        attachment->set_filename("report.bin");
        attachment->set_data(binary);
        attachment = request.add_attachments();
        attachment->set_filename("report.csv");
        attachment->set_data("name,value\nitem,42\n");
        attachment->set_content_type("text/csv");
        Call(&NotifyService_Stub::SendEmail, request, ErrorCode::SUCCESS);

        const auto message = wire.get();
        EXPECT_EQ(DecodePart(message, "Content-Type: text/html"), request.content());
        EXPECT_EQ(DecodePart(message, "filename=\"report.bin\""), binary);
        EXPECT_EQ(DecodePart(message, "filename=\"report.csv\""), request.attachments(1).data());
        EXPECT_NE(message.find("Content-Type: application/octet-stream"), std::string::npos);
        EXPECT_NE(message.find("Content-Type: text/csv"), std::string::npos);
    }

    // SMTP 最终拒收时，调用方收到发送失败，证明入队与投递结果分别处理。
    TEST_F(NotifyServiceTest, ReportsSmtpRejection)
    {
        auto wire = std::async(std::launch::async, [&]
                               { return _smtp.Receive(false); });
        SendEmailRequest request;
        request.set_request_id("rejected-report");
        request.set_to_email(kRecipient);
        request.set_subject("报告");
        request.set_content("<p>结果</p>");
        Call(&NotifyService_Stub::SendEmail, request, ErrorCode::NOTIFY_SEND_FAILED);
        EXPECT_FALSE(wire.get().empty());
    }

    // 业务拒绝无效验证码和附件头，RPC 仍返回原始请求 ID。
    TEST_F(NotifyServiceTest, RejectsInvalidInputs)
    {
        SendVerifyCodeRequest code;
        code.set_request_id("invalid-code");
        code.set_email(kRecipient);
        code.set_verify_code("12345A");
        Call(&NotifyService_Stub::SendVerifyCode, code, ErrorCode::NOTIFY_INVALID_INPUT);

        SendEmailRequest report;
        report.set_request_id("invalid-report");
        report.set_to_email(kRecipient);
        report.set_subject("报告");
        report.add_attachments()->set_filename(std::string("bad\0name", 8));
        Call(&NotifyService_Stub::SendEmail, report, ErrorCode::NOTIFY_INVALID_INPUT);
    }

    // 通过真实 QQ SMTP 发送现有 HTML 模板渲染的验证码邮件。
    TEST_F(NotifyServiceLiveTest, SendsVerificationCode)
    {
        SendVerifyCodeRequest request;
        request.set_request_id("live-verify-code");
        request.set_email(Recipient());
        request.set_verify_code("000123");
        Call(&NotifyService_Stub::SendVerifyCode, request, ErrorCode::SUCCESS);
    }

    // 通过真实 QQ SMTP 发送 HTML 邮件和附件，并断言服务已收到 SMTP 确认。
    TEST_F(NotifyServiceLiveTest, SendsHtmlWithAttachment)
    {
        SendEmailRequest request;
        request.set_request_id("live-html-mail");
        request.set_to_email(Recipient());
        request.set_subject("[Chat2Data SMTP 测试] 普通邮件");
        request.set_content("<p>这是一封通知服务的联调邮件，附件用于验证真实 SMTP 投递。</p>");
        auto *attachment = request.add_attachments();
        attachment->set_filename("chat2data-smtp-test.txt");
        attachment->set_content_type("text/plain");
        attachment->set_data("Chat2Data SMTP integration test\n");
        Call(&NotifyService_Stub::SendEmail, request, ErrorCode::SUCCESS);
    }
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}
