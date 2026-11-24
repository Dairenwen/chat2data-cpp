#include "notifyBusiness.hpp"

#include "BreezeCode.hpp"
#include "errorHandler.hpp"
#include "notifyService.pb.h"
#include "utils.hpp"

#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;

        // 先检查邮件头字段，再入队，以便 RPC 返回明确的参数错误。
        bool HasInvalidHeaderBytes(const std::string &value)
        {
            return value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos;
        }

        // 收件人字段只接受一个邮箱地址，以原值交给 SMTP。
        void ValidateRecipient(const std::string &email)
        {
            if (!chat2Data::Utils::IsValidEmail(email))
                throw Chat2DataException(ErrorCode::NOTIFY_INVALID_INPUT);
        }
    }

    NotifyBusiness::NotifyBusiness(std::unique_ptr<Breezecode::MailClient> mail_client)
        : _mail_client(std::move(mail_client))
    {
    }

    NotifyBusiness::~NotifyBusiness() = default;

    std::future<bool> NotifyBusiness::SendVerifyCode(const chat2Data::SendVerifyCodeRequest &request)
    {
        ValidateRecipient(request.email());
        // 验证码按六位 ASCII 数字字符串处理，保留模板替换所需的前导零。
        const auto &code = request.verify_code();
        if (code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos)
            throw Chat2DataException(ErrorCode::NOTIFY_INVALID_INPUT);

        return _mail_client->sendAsync(request.email(), code);
    }

    std::future<bool> NotifyBusiness::SendEmail(const chat2Data::SendEmailRequest &request)
    {
        ValidateRecipient(request.to_email());
        if (request.subject().empty() || HasInvalidHeaderBytes(request.subject()))
            throw Chat2DataException(ErrorCode::NOTIFY_INVALID_INPUT);

        Breezecode::MailMessage message{request.to_email(), request.subject(), request.content(), {}};
        message.attachments.reserve(request.attachments_size());
        for (const auto &attachment : request.attachments())
        {
            if (attachment.filename().empty() || HasInvalidHeaderBytes(attachment.filename()) ||
                HasInvalidHeaderBytes(attachment.content_type()))
                throw Chat2DataException(ErrorCode::NOTIFY_INVALID_INPUT);

            // Protobuf bytes 保存在 std::string 中，直接复制以保留包含 NUL 的完整附件。
            message.attachments.push_back({attachment.filename(), attachment.data(), attachment.content_type()});
        }
        return _mail_client->sendMailAsync(std::move(message));
    }
}
