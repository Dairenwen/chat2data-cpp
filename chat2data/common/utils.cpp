#include "utils.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <sw/redis++/redis.h>
#include <sw/redis++/transaction.h>
#include <sw/redis++/queued_redis.h>

namespace chat2Data
{
    std::uint64_t Utils::NowMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    Json::Value Utils::ParseCacheJson(const std::string &text)
    {
        auto value = BreezeUtil::JSON::unserialize(text);
        if (!value || !value->isObject())
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
        return *value;
    }

    void Utils::WriteCache(sw::redis::Redis &redis, const std::string &key, const Json::Value &value, std::uint64_t ttlMs)
    {
        auto text = BreezeUtil::JSON::serialize(value); // 存储序列化之后的JSON字符串
        if (!text)
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);

        // MULTI/EXEC 内连续执行 HSET key data json 和 PEXPIRE key ttlMs。
        auto transaction = redis.transaction(false, false);
        transaction.hset(key, "data", *text).pexpire(key, static_cast<long long>(ttlMs));
        auto replies = transaction.exec();
        replies.get<long long>(0);
        replies.get<long long>(1);
    }

    std::optional<std::string> Utils::ReadCache(sw::redis::Redis &redis, const std::string &key)
    {
        // HGET 与 EXISTS 在同一事务执行，区分缓存未命中和缺少 data 字段。
        auto transaction = redis.transaction(false, false);
        transaction.hget(key, "data").exists(key);
        auto replies = transaction.exec();
        auto value = replies.get<sw::redis::OptionalString>(0); // 获取第一个命令的返回值，即 HGET 的结果
        if (!value && replies.get<long long>(1))
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
        return value ? std::optional<std::string>(*value) : std::nullopt;
    }

    std::string Utils::GenerateUUID()
    {
        // 各线程独立持有随机引擎，计数器在所有线程间共享。
        thread_local std::mt19937 generator(std::random_device{}()); // 使用随机设备初始化随机数生成器
        std::uniform_int_distribution<int> distribution(0, 255);     // 生成 0-255 的随机整数
        static std::atomic<std::uint16_t> counter{0};                // 各线程共享的 16 位计数器
        constexpr char digits[] = "0123456789abcdef";

        std::string uuid;
        uuid.reserve(18);
        for (int i = 0; i < 6; ++i)
        {
            if (i == 2)
            {
                uuid += '-';
            }
            const int byte = distribution(generator);
            uuid += digits[byte >> 4];
            uuid += digits[byte & 0x0f];
        }

        uuid += '-';
        const std::uint16_t value = counter.fetch_add(1, std::memory_order_relaxed); // 16 位计数器循环递增
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            uuid += digits[(value >> shift) & 0x0f];
        }
        return uuid;
    }
}
