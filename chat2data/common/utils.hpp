#pragma once

#include <cstdint>
#include <optional>
#include <string>

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
