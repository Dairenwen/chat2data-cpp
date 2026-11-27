#include "utils.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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
#include <utility>

namespace chat2Data
{
    namespace
    {
        constexpr std::string_view kPasswordVersion = "pbkdf2-sha256-v1$"; // 密码记录版本号，便于未来升级算法
        constexpr int kPasswordIterations = 600000;                        // 迭代次数，约 0.1 秒计算时间
        constexpr std::size_t kSaltBytes = 16;                             // 16 字节随机盐
        constexpr std::size_t kHashBytes = 32;                             // 32 字节派生哈希
        constexpr char kHexDigits[] = "0123456789abcdef";                  // 十六进制字符表

        bool IsAsciiLetter(char ch) noexcept
        {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        }

        bool IsAsciiAlnum(char ch) noexcept
        {
            return IsAsciiLetter(ch) || Utils::IsAsciiDigit(ch);
        }

        // 生成安全随机字节数组，使用 OpenSSL RAND_bytes()
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
        // 派生密码哈希，使用 PBKDF2-HMAC-SHA256 算法。该算法需要自己设置迭代次数和盐值，返回固定长度的哈希值。
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

    // 派生密码哈希，使用 PBKDF2-HMAC-SHA256 算法，先生成16字节随机盐，再使用盐和迭代次数派生哈希，返回带版本号、盐和哈希的字符串。
    std::string Utils::HashPassword(const std::string &password)
    {
        // 返回格式为 "pbkdf2-sha256-v1$<32 字符的盐>$<64 字符的哈希>"
        const auto salt = HexEncode(SecureBytes<kSaltBytes>());
        return std::string(kPasswordVersion) + salt + '$' + DerivePasswordHash(password, salt);
    }

    // 验证密码是否与记录的哈希匹配，使用 PBKDF2-HMAC-SHA256 算法
    bool Utils::VerifyPassword(const std::string &password, const std::string &record)
    {
        // 将记录拆分为版本号、盐和哈希，验证格式是否正确，若不正确则返回 false。然后使用盐和迭代次数派生哈希，并与记录中的哈希进行比较。
        constexpr auto separator = kPasswordVersion.size() + 2 * kSaltBytes;
        if (record.size() != separator + 1 + 2 * kHashBytes ||
            record.compare(0, kPasswordVersion.size(), kPasswordVersion) != 0 || record[separator] != '$')
            return false;

        const auto salt = std::string_view(record).substr(kPasswordVersion.size(), 2 * kSaltBytes);
        const auto hash = DerivePasswordHash(password, salt);
        return CRYPTO_memcmp(hash.data(), record.data() + separator + 1, hash.size()) == 0;
    }

    bool Utils::IsAsciiDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }

    char Utils::ToAsciiLower(char ch) noexcept
    {
        return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
    }

    std::string Utils::ToAsciiLower(std::string_view text)
    {
        std::string lower(text);
        for (auto &ch : lower)
            ch = ToAsciiLower(ch);
        return lower;
    }

    std::string_view Utils::TrimAsciiWhitespace(std::string_view text) noexcept
    {
        constexpr std::string_view whitespace = " \t\n\r\f\v";
        const auto first = text.find_first_not_of(whitespace);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
    }

    std::size_t Utils::Utf8CharWidth(std::string_view text) noexcept
    {
        if (text.empty())
            return 0;

        const auto first = static_cast<unsigned char>(text.front());
        std::size_t width = 1;
        if (first >= 0xC2 && first <= 0xDF)
            width = 2;
        else if (first >= 0xE0 && first <= 0xEF)
            width = 3;
        else if (first >= 0xF0 && first <= 0xF4)
            width = 4;

        // 检查续字节，使截断的序列可以逐字节继续处理。
        if (width > text.size())
            return 1;
        for (std::size_t i = 1; i < width; ++i)
        {
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                return 1;
        }
        return width;
    }

    std::string_view Utils::Utf8Prefix(std::string_view text, std::size_t character_count) noexcept
    {
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < character_count && bytes < text.size(); ++i)
            bytes += Utf8CharWidth(text.substr(bytes));
        return text.substr(0, bytes);
    }

    bool Utils::IsValidUtf8Length(std::string_view text, std::size_t max_characters) noexcept
    {
        while (!text.empty())
        {
            if (max_characters == 0)
                return false;
            --max_characters;

            const auto width = Utf8CharWidth(text);
            const auto first = static_cast<unsigned char>(text.front());
            if (width == 1 && first >= 0x80)
                return false;
            if (width >= 3)
            {
                // 限制编码范围，排除过长编码、代理区和 Unicode 范围外的值。
                const auto second = static_cast<unsigned char>(text[1]);
                if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0) ||
                    (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90))
                    return false;
            }
            text.remove_prefix(width);
        }
        return true;
    }

    bool Utils::IsCompleteFastDfsId(std::string_view file_id) noexcept
    {
        if (file_id.find('/') == std::string_view::npos)
            return false;
        for (const unsigned char ch : file_id)
        {
            if (ch <= 0x20 || ch == 0x7f || ch == '\\')
                return false;
        }

        // 完整 ID 由组名和远端路径组成，每段都应是有效的名称。
        for (;;)
        {
            const auto slash = file_id.find('/');
            const auto part = file_id.substr(0, slash);
            if (part.empty() || part == "." || part == "..")
                return false;
            if (slash == std::string_view::npos)
                return true;
            file_id.remove_prefix(slash + 1);
        }
    }

    bool Utils::ParseFiniteDouble(std::string_view text, double &value) noexcept
    {
        // from_chars 自身支持负号。单独跳过前导加号，并要求数字只使用一个符号。
        if (!text.empty() && text.front() == '+')
        {
            text.remove_prefix(1);
            if (!text.empty() && text.front() == '-')
                return false;
        }
        if (text.empty())
            return false;

        double number;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, number, std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(number))
            return false;
        value = number;
        return true;
    }

    bool Utils::IsValidEmail(std::string_view email) noexcept
    {
        constexpr std::size_t kMaxAddressBytes = 254;
        constexpr std::size_t kMaxLocalPartBytes = 64;
        constexpr std::size_t kMaxDomainLabelBytes = 63;
        constexpr std::string_view kLocalSymbols = "!#$%&'*+-/=?^_`{|}~.";

        const auto at = email.find('@');
        if (email.size() > kMaxAddressBytes || at == std::string_view::npos ||
            at == 0 || at > kMaxLocalPartBytes)
            return false;

        // 本地部分由非空的 dot-atom 段组成，保留常见的加号别名。
        const auto local = email.substr(0, at);
        if (local.front() == '.' || local.back() == '.' || local.find("..") != std::string_view::npos ||
            !std::all_of(local.begin(), local.end(), [kLocalSymbols](char ch)
                         { return IsAsciiAlnum(ch) || kLocalSymbols.find(ch) != std::string_view::npos; }))
            return false;

        auto domain = email.substr(at + 1);
        if (domain.find('.') == std::string_view::npos)
            return false;

        // 逐段校验 DNS 主机名，并用顶级域规则排除纯数字结尾和裸 IP。
        for (;;)
        {
            const auto dot = domain.find('.');
            const auto label = domain.substr(0, dot);
            if (label.empty() || label.size() > kMaxDomainLabelBytes ||
                !IsAsciiAlnum(label.front()) || !IsAsciiAlnum(label.back()) ||
                !std::all_of(label.begin(), label.end(), [](char ch)
                             { return IsAsciiAlnum(ch) || ch == '-'; }))
                return false;
            if (dot == std::string_view::npos)
                return std::any_of(label.begin(), label.end(), IsAsciiLetter);
            domain.remove_prefix(dot + 1);
        }
    }

    std::uint64_t Utils::NowMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    bool Utils::IsValidPort(std::int64_t port) noexcept
    {
        return port > 0 && port <= std::numeric_limits<std::uint16_t>::max();
    }

    bool Utils::IsValidHostPort(std::string_view address) noexcept
    {
        const auto colon = address.rfind(':');
        if (colon == std::string_view::npos || colon == 0 ||
            address.find_first_of("/ \t\r\n") != std::string_view::npos ||
            address.find('\0') != std::string_view::npos)
            return false;

        int port = 0;
        const auto end = address.data() + address.size();
        const auto parsed = std::from_chars(address.data() + colon + 1, end, port);
        return parsed.ec == std::errc{} && parsed.ptr == end && IsValidPort(port);
    }

    Json::Value Utils::ParseJsonObject(const std::string &text)
    {
        auto value = BreezeUtil::JSON::unserialize(text);
        return value && value->isObject() ? std::move(*value) : Json::Value{};
    }

    std::string Utils::ReadJsonString(const Json::Value &object, const char *name)
    {
        if (!object.isObject())
            return {};
        const auto &field = object[name];
        return field.isString() ? field.asString() : std::string{};
    }

    Json::Value Utils::ParseCacheJson(const std::string &text)
    {
        auto value = ParseJsonObject(text);
        if (value.isNull())
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
        return value;
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
