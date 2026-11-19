#include "errorHandler.hpp"

#include <unordered_map>

namespace chat2Data
{
    std::string error2String(ErrorCode error_code)
    {
        static const std::unordered_map<ErrorCode, std::string> descriptions = {
            {ErrorCode::SUCCESS, "操作成功"},
            // 用户子服务错误描述。
            {ErrorCode::USER_CACHE_INVALID, "用户缓存数据无效"},
            {ErrorCode::USER_INVALID_INPUT, "用户请求参数无效"},
            {ErrorCode::USER_NICKNAME_TAKEN, "昵称已被使用"},
            {ErrorCode::USER_EMAIL_TAKEN, "邮箱已被使用"},
            {ErrorCode::USER_REGISTER_CONFLICT, "注册信息与已有记录冲突"},
            {ErrorCode::USER_INVALID_LOGIN, "账号或密码错误"},
            {ErrorCode::USER_NOT_FOUND, "用户不存在"},
            {ErrorCode::USER_SESSION_NOT_FOUND, "会话不存在"},
            {ErrorCode::USER_SESSION_EXPIRED, "会话已过期"},
            {ErrorCode::USER_CODE_MISMATCH, "验证码不匹配"},
            {ErrorCode::USER_CODE_MISSING, "验证码不存在或已过期"},
            {ErrorCode::USER_NOTIFICATION_FAILED, "验证码邮件发送失败"},
            {ErrorCode::USER_CREDENTIAL_FAILURE, "安全凭证处理失败"},
            // 文件子服务错误描述。
            // 数据库子服务错误描述。
            // Excel 解析子服务错误描述。
            // 通知子服务错误描述。
            // AI 子服务错误描述。
            // 网关子服务错误描述。
        };

        const auto it = descriptions.find(error_code);
        return it != descriptions.end() ? it->second : "未知错误";
    }

    std::string getServiceName(ErrorCode error_code)
    {
        const int value = static_cast<int>(error_code);
        if (value < 100 || value > 799)
        {
            return "未知服务";
        }

        switch (value / 100)
        {
        case 1:
            return "userService";
        case 2:
            return "fileService";
        case 3:
            return "dbService";
        case 4:
            return "excelService";
        case 5:
            return "notifyService";
        case 6:
            return "aiService";
        case 7:
            return "gatewayService";
        }
        return "未知服务";
    }

    Chat2DataException::Chat2DataException(ErrorCode error_code)
        : _error_code(error_code),
          _error_description(error2String(_error_code)),
          _message(getServiceName(_error_code) + ": " + _error_description)
    {
    }

    const char *Chat2DataException::what() const noexcept
    {
        return _message.c_str();
    }

    ErrorCode Chat2DataException::Code() const noexcept
    {
        return _error_code;
    }
}
