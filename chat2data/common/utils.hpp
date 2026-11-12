#pragma once

#include <string>

namespace chat2Data
{
    class Utils
    {
    public:
        // 生成 xxxx-xxxxxxxx-yyyy 格式的标识符；16 位计数器循环递增。
        static std::string GenerateUUID();
    };
}
