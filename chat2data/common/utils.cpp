#include "utils.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <random>
#include <string_view>
#include <sw/redis++/redis.h>
#include <sw/redis++/transaction.h>
#include <sw/redis++/queued_redis.h>

namespace chat2Data
{
    namespace
    {
        constexpr std::string_view kPasswordVersion = "pbkdf2-sha256-v1$";
        constexpr int kPasswordIterations = 600000;
        constexpr std::size_t kSaltBytes = 16;
        constexpr std::size_t kHashBytes = 32;
        constexpr char kHexDigits[] = "0123456789abcdef";

        template <std::size_t Size>
        std::array<unsigned char, Size> SecureBytes()
        {
            std::array<unsigned char, Size> bytes{};
            if (RAND_bytes(bytes.data(), bytes.size()) != 1)
                throw Chat2DataException(ErrorCode::USER_CREDENTIAL_FAILURE);
            return bytes;
        }
        // 将字节数组编码为十六进制字符串
        template <std::size_t Size>
        std::string HexEncode(const std::array<unsigned char, Size> &bytes)
        {
            std::string result;
            result.reserve(2 * Size);
            for (const auto byte : bytes)
            {
                result += kHexDigits[byte >> 4];
                result += kHexDigits[byte & 0x0f];
            }
            return result;
        }
        // 派生密码哈希，使用 PBKDF2-HMAC-SHA256 算法。
        std::string DerivePasswordHash(const std::string &password, std::string_view salt)
        {
            std::array<unsigned char, kHashBytes> hash{};
            if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                                  reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
                                  kPasswordIterations, EVP_sha256(), hash.size(), hash.data()) != 1)
                throw Chat2DataException(ErrorCode::USER_CREDENTIAL_FAILURE);
            return HexEncode(hash);
        }
    }

    // 生成 xxxx-xxxxxxxx-yyyy 格式的标识符；16 位计数器循环递增
    std::string Utils::GenerateSecureId()
    {
        return HexEncode(SecureBytes<16>());
    }

    // 生成六位数字验证码，使用安全随机数生成器。
    std::string Utils::GenerateVerificationCode()
    {
        std::string code;
        while (code.size() < 6)
        {
            for (const auto byte : SecureBytes<6>())
            {
                // 前 250 个取值能等概率映射到十个数字。
                if (byte < 250)
                    code += static_cast<char>('0' + byte % 10);
                if (code.size() == 6)
                    break;
            }
        }
        return code;
    }

    // 派生密码哈希，使用 PBKDF2-HMAC-SHA256 算法
    std::string Utils::HashPassword(const std::string &password)
    {
        //
        const auto salt = HexEncode(SecureBytes<kSaltBytes>());
        return std::string(kPasswordVersion) + salt + '$' + DerivePasswordHash(password, salt);
    }

    // 验证密码是否与记录的哈希匹配，使用 PBKDF2-HMAC-SHA256 算法
    bool Utils::VerifyPassword(const std::string &password, const std::string &record)
    {
        constexpr auto separator = kPasswordVersion.size() + 2 * kSaltBytes;
        if (record.size() != separator + 1 + 2 * kHashBytes ||
            record.compare(0, kPasswordVersion.size(), kPasswordVersion) != 0 || record[separator] != '$')
            return false;

        const auto salt = std::string_view(record).substr(kPasswordVersion.size(), 2 * kSaltBytes);
        const auto hash = DerivePasswordHash(password, salt);
        return CRYPTO_memcmp(hash.data(), record.data() + separator + 1, hash.size()) == 0;
    }

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
