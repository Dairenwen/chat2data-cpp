#include "BreezeCode.hpp"
#include "errorHandler.hpp"
#include "notifyBusiness.hpp"
#include "notifyServiceImpl.hpp"

#include <arpa/inet.h>
#include <brpc/channel.h>
#include <brpc/server.h>
#include <butil/base64.h>
#include <butil/fd_guard.h>
#include <cstdio>
#include <future>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <utility>

DEFINE_string(smtp_username, "3484725023@qq.com", "真实 SMTP 测试的 QQ 邮箱账号");
DEFINE_string(smtp_password, "lshcqpcfdmilchjj", "真实 SMTP 测试的客户端授权码");
DEFINE_string(smtp_recipient, "1962478161@qq.com", "真实 SMTP 测试的收件人，留空时使用测试默认地址");

namespace
{
    using namespace chat2Data;
    constexpr char kRecipient[] = "1962478161@qq.com";

    // 在回环地址接收 libcurl 实际发送的 SMTP 数据，每次接收一封邮件。
    class LocalSmtpReceiver
    {
    public:
        LocalSmtpReceiver() : _listener(socket(AF_INET, SOCK_STREAM, 0))
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t size = sizeof(address);
            if (_listener < 0 || bind(_listener, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
                getsockname(_listener, reinterpret_cast<sockaddr *>(&address), &size) != 0 ||
                listen(_listener, 1) != 0)
                throw std::runtime_error("启动本地 SMTP 接收端失败");
            _port = ntohs(address.sin_port);
        }

        std::string Url() const
        {
            return "smtp://127.0.0.1:" + std::to_string(_port);
        }

        std::string Receive(bool accept_mail)
        {
            // 给连接和读写设置等待上限，以便投递流程异常时测试能够退出。
            pollfd waiting{_listener, POLLIN, 0};
            if (poll(&waiting, 1, 4000) <= 0)
                throw std::runtime_error("等待 SMTP 连接超时");

            butil::fd_guard connection(accept(_listener, nullptr, nullptr));
            const timeval timeout{3, 0};
            if (connection < 0 || setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
                setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
                throw std::runtime_error("无法接收 SMTP 连接");

            std::unique_ptr<std::FILE, decltype(&std::fclose)> input(fdopen(connection, "r"), &std::fclose);
            if (!input)
                throw std::runtime_error("无法创建 SMTP 输入流");
            const int fd = connection.release();
            Reply(fd, "220 localhost SMTP test\r\n");

            char buffer[1024];
            std::string message;
            bool reading_data = false;
            while (std::fgets(buffer, sizeof(buffer), input.get()))
            {
                const std::string_view line(buffer);
                if (reading_data)
                {
                    // 正文传输结束后再返回接收结果，覆盖 SMTP 对整封邮件的确认阶段。
                    if (line == ".\r\n")
                    {
                        Reply(fd, accept_mail ? "250 accepted\r\n" : "550 rejected\r\n");
                        reading_data = false;
                    }
                    else
                    {
                        message += line;
                    }
                }
                else if (line == "DATA\r\n")
                {
                    Reply(fd, "354 end with a dot\r\n");
                    reading_data = true;
                }
                else if (line == "QUIT\r\n")
                {
                    Reply(fd, "221 closing\r\n");
                    break;
                }
                else
                {
                    Reply(fd, "250 OK\r\n");
                }
            }

            return message;
        }

    private:
        static void Reply(int fd, std::string_view text)
        {
            while (!text.empty())
            {
                const auto sent = send(fd, text.data(), text.size(), MSG_NOSIGNAL);
                if (sent <= 0)
                    throw std::runtime_error("无法返回 SMTP 响应");
                text.remove_prefix(sent);
            }
        }

        butil::fd_guard _listener;
        int _port = 0;
    };

    // 从 SMTP 收到的原始邮件解码 MIME 分段，按字节验证实际投递内容。
    std::string DecodePart(const std::string &wire, std::string_view header)
    {
        const auto part = wire.find(header);
        const auto headers_end = wire.find("\r\n\r\n", part);
        if (headers_end == std::string::npos)
            throw std::runtime_error("缺少预期邮件分段");

        const auto body_start = headers_end + 4;
        const auto body_end = wire.find("\r\n--", body_start);
        std::string encoded;
        for (const char ch : std::string_view(wire).substr(body_start, body_end - body_start))
        {
            if (ch != '\r' && ch != '\n')
                encoded.push_back(ch);
        }
        std::string decoded;
        if (!butil::Base64Decode(encoded, &decoded))
            throw std::runtime_error("邮件分段的 Base64 内容无效");
        return decoded;
    }

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
