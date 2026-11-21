#pragma once

#include <exception>
#include <string>

namespace chat2Data
{
    enum class ErrorCode
    {
        SUCCESS = 0,

        // 用户子服务：100～199。
        USER_CACHE_INVALID = 100, // 缓存数据无法解析或字段不完整。
        USER_INVALID_INPUT,
        USER_NICKNAME_TAKEN,
        USER_EMAIL_TAKEN,
        USER_REGISTER_CONFLICT,
        USER_INVALID_LOGIN,
        USER_NOT_FOUND,
        USER_SESSION_NOT_FOUND,
        USER_SESSION_EXPIRED,
        USER_CODE_MISMATCH,
        USER_CODE_MISSING,
        USER_NOTIFICATION_FAILED,
        USER_CREDENTIAL_FAILURE,
        USER_BUSINESS_FAILED,
        USER_SERVER_CONFIG_INVALID,
        USER_ETCD_CONNECTION_FAILED,
        USER_SERVER_START_FAILED,
        USER_SERVICE_DISCOVERY_FAILED,
        USER_SERVICE_REGISTRATION_FAILED,
        // 文件子服务：200～299。
        // 数据库子服务：300～399。
        // Excel 解析子服务：400～499。
        // 通知子服务：500～599。
        // AI 子服务：600～699。
        // 网关子服务：700～799。
    };

    // 返回错误码对应的描述；未定义的错误码返回“未知错误”。
    std::string error2String(ErrorCode error_code);

    // 根据错误码所属范围返回子服务名称；范围外返回“未知服务”。
    std::string getServiceName(ErrorCode error_code);

    class Chat2DataException : public std::exception
    {
    public:
        explicit Chat2DataException(ErrorCode error_code);
        const char *what() const noexcept override;
        ErrorCode Code() const noexcept;

    private:
        ErrorCode _error_code;
        std::string _error_description;
        std::string _message;
    };
}
