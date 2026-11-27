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
            {ErrorCode::USER_BUSINESS_FAILED, "用户业务处理失败"},
            {ErrorCode::USER_SERVER_CONFIG_INVALID, "用户服务启动配置无效"},
            {ErrorCode::USER_ETCD_CONNECTION_FAILED, "无法连接用户服务的 etcd 端点"},
            {ErrorCode::USER_SERVER_START_FAILED, "用户服务启动失败"},
            {ErrorCode::USER_SERVICE_DISCOVERY_FAILED, "用户服务发现启动失败"},
            {ErrorCode::USER_SERVICE_REGISTRATION_FAILED, "用户服务注册失败"},
            // 文件子服务错误描述。
            {ErrorCode::FILE_INVALID_INPUT, "文件请求参数无效"},
            {ErrorCode::FILE_UNAVAILABLE, "文件不可用"},
            {ErrorCode::FILE_BUSY, "文件处理中，请稍后重试"},
            {ErrorCode::FILE_ALREADY_UPLOADED, "文件已上传"},
            {ErrorCode::FILE_UPLOAD_EXPIRED, "文件上传登记已过期，请重新登记"},
            {ErrorCode::FILE_STORAGE_FAILED, "访问 FastDFS 失败"},
            {ErrorCode::FILE_PARSE_FAILED, "解析 Excel 文件失败"},
            {ErrorCode::FILE_DEPENDENCY_FAILED, "文件处理所需的下游服务不可用或调用失败"},
            {ErrorCode::FILE_SQLITE_IN_USE, "请先断开 SQLite 连接再删除文件"},
            {ErrorCode::FILE_PREVIEW_TOO_LARGE, "预览数据量过大，请指定工作表或减小页容量"},
            {ErrorCode::FILE_CACHE_FAILED, "文件缓存操作失败"},
            {ErrorCode::FILE_BUSINESS_FAILED, "文件业务处理失败"},
            {ErrorCode::FILE_SERVER_CONFIG_INVALID, "文件服务启动配置无效"},
            {ErrorCode::FILE_ETCD_CONNECTION_FAILED, "无法连接文件服务的 etcd 端点"},
            {ErrorCode::FILE_SERVER_START_FAILED, "文件服务启动失败"},
            {ErrorCode::FILE_SERVICE_DISCOVERY_FAILED, "文件服务发现启动失败"},
            {ErrorCode::FILE_SERVICE_REGISTRATION_FAILED, "文件服务注册失败"},
            // 数据库子服务错误描述。
            // Excel 解析子服务错误描述。
            {ErrorCode::EXCEL_INVALID_INPUT, "Excel 解析请求参数无效"},
            {ErrorCode::EXCEL_DOWNLOAD_FAILED, "下载 Excel 工作簿失败"},
            {ErrorCode::EXCEL_WORKBOOK_INVALID, "Excel 工作簿无效"},
            {ErrorCode::EXCEL_WORKSHEET_NOT_FOUND, "Excel 工作表不存在"},
            {ErrorCode::EXCEL_EMPTY_WORKSHEET, "Excel 工作表没有可解析的数据结构"},
            {ErrorCode::EXCEL_PARSE_FAILED, "解析 Excel 工作表失败"},
            {ErrorCode::EXCEL_SERVER_CONFIG_INVALID, "Excel 解析服务启动配置无效"},
            {ErrorCode::EXCEL_ETCD_CONNECTION_FAILED, "无法连接 Excel 解析服务的 etcd 端点"},
            {ErrorCode::EXCEL_SERVER_START_FAILED, "Excel 解析服务启动失败"},
            {ErrorCode::EXCEL_SERVICE_REGISTRATION_FAILED, "Excel 解析服务注册失败"},
            // 通知子服务错误描述。
            {ErrorCode::NOTIFY_INVALID_INPUT, "通知请求参数无效"},
            {ErrorCode::NOTIFY_SEND_FAILED, "邮件发送失败"},
            {ErrorCode::NOTIFY_SEND_TIMEOUT, "等待邮件发送结果超时"},
            {ErrorCode::NOTIFY_BUSINESS_FAILED, "通知业务处理失败"},
            {ErrorCode::NOTIFY_SERVER_CONFIG_INVALID, "通知服务启动配置无效"},
            {ErrorCode::NOTIFY_TEMPLATE_UNREADABLE, "无法读取验证码邮件模板"},
            {ErrorCode::NOTIFY_ETCD_CONNECTION_FAILED, "无法连接通知服务的 etcd 端点"},
            {ErrorCode::NOTIFY_SERVER_START_FAILED, "通知服务启动失败"},
            {ErrorCode::NOTIFY_SERVICE_REGISTRATION_FAILED, "通知服务注册失败"},
            // AI 子服务错误描述。
            // 网关子服务错误描述。
            {ErrorCode::GATEWAY_USER_RPC_FAILED, "网关无法调用用户服务"},
            {ErrorCode::GATEWAY_NOT_IMPLEMENTED, "后端接口暂未接通"},
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
