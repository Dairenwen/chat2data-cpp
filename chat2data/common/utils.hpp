#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Json
{
    class Value;
}

namespace sw::redis
{
    class Redis;
}

namespace chat2Data
{
    class Utils
    {
    public:
        // 生成 xxxx-xxxxxxxx-yyyy 格式的标识符；16 位计数器循环递增。
        static std::string GenerateUUID();

        // 生成 32 位十六进制凭证或六位数字验证码。
        static std::string GenerateSecureId();
        static std::string GenerateVerificationCode();

        // 密码记录包含算法版本、随机盐和派生哈希。
        static std::string HashPassword(const std::string &password);
        static bool VerifyPassword(const std::string &password, const std::string &record);

        // 校验单个 ASCII 邮箱地址及完整域名的格式。
        static bool IsValidEmail(std::string_view email) noexcept;

        // 检查配置端口是否在 1～65535 范围内。
        static bool IsValidPort(std::int64_t port) noexcept;

        // 解析 JSON 对象，格式错误或其他根节点类型统一返回 JSON 空值。
        static Json::Value ParseJsonObject(const std::string &text);

        // 读取对象中的字符串字段，缺失字段或其他类型统一返回空字符串。
        static std::string ReadJsonString(const Json::Value &object, const char *name);

        /**
         * @brief 获取当前系统时间。
         * @return Unix 时间戳，单位为毫秒。
         */
        static std::uint64_t NowMs();

        /**
         * @brief 解析缓存中的 JSON 对象。
         * @param text Redis 中读取的 JSON 字符串。
         * @return 解析后的 JSON 对象。
         * @note 解析失败或内容不是对象时抛出缓存格式异常。
         */
        static Json::Value ParseCacheJson(const std::string &text);

        /**
         * @brief 将 JSON 对象和有效期写入 Redis Hash 的 data 字段。
         * @param redis Redis 客户端。
         * @param key 缓存键。
         * @param value 待写入的 JSON 对象。
         * @param ttlMs 缓存有效期，单位为毫秒。
         * @note 使用 Redis 事务连续执行 HSET 和 PEXPIRE；失败时向调用方报告。
         */
        static void WriteCache(sw::redis::Redis &redis, const std::string &key, const Json::Value &value, std::uint64_t ttlMs);

        /**
         * @brief 读取 Redis Hash 的 data 字段。
         * @param redis Redis 客户端。
         * @param key 缓存键。
         * @return JSON 字符串；键不存在时为空。
         * @note 键存在但缺少 data 字段时抛出缓存格式异常。
         */
        static std::optional<std::string> ReadCache(sw::redis::Redis &redis, const std::string &key);
    };
}
