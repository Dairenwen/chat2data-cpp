#include "errorHandler.hpp"

#include <unordered_map>

namespace chat2Data
{
    std::string error2String(ErrorCode error_code)
    {
        static const std::unordered_map<ErrorCode, std::string> descriptions = {
            {ErrorCode::SUCCESS, "操作成功"},
            // 用户子服务错误描述。
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
}
