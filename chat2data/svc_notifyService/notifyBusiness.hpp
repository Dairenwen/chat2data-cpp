#pragma once

#include <future>
#include <memory>

namespace Breezecode
{
    class MailClient;
}

namespace chat2Data
{
    class SendEmailRequest;
    class SendVerifyCodeRequest;
}

namespace chat2data
{
    // 校验通知请求并将两类邮件提交给同一个发送队列
    class NotifyBusiness
    {
    public:
        /**
         * @brief 接管调用方已经构造的邮件客户端。
         * @param mail_client 已启用队列的 BreezeCode 邮件客户端。
         */
        explicit NotifyBusiness(std::unique_ptr<Breezecode::MailClient> mail_client);

        /**
         * @brief 释放邮件客户端，由客户端收尾已接收的任务并回收工作线程。
         */
        ~NotifyBusiness();

        /**
         * @brief 校验收件人和验证码，将验证码邮件提交到队列。
         * @param request 包含收件邮箱和保留前导零的六位数字验证码。
         * @return 本次任务的 future。SMTP 接收邮件后结果为 true，任务拒绝或发送失败时为 false。
         * @throws chat2Data::Chat2DataException 收件邮箱或验证码无效。
         */
        std::future<bool> SendVerifyCode(const chat2Data::SendVerifyCodeRequest &request);

        /**
         * @brief 校验并提交包含 HTML 正文和可选附件的普通邮件。
         * @param request 包含收件邮箱、主题、HTML 正文及附件的原始字节。
         * @return 本次任务的 future。SMTP 接收邮件后结果为 true，任务拒绝或发送失败时为 false。
         * @throws chat2Data::Chat2DataException 收件邮箱、主题、附件文件名或 MIME 类型无效。
         */
        std::future<bool> SendEmail(const chat2Data::SendEmailRequest &request);

    private:
        std::unique_ptr<Breezecode::MailClient> _mail_client;
    };
}
