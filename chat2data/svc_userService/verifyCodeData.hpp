#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sw::redis
{
    class Redis;
}

namespace chat2Data
{
    struct CachedVerifyCode
    {
        std::string codeId;          // 验证码 ID。
        std::string verifyCode;      // 字符串格式验证码，保留前导零。
        std::string email;           // 接收验证码的邮箱。
        std::uint64_t createdAt = 0; // 创建时间，Unix 毫秒。
    };

    // 验证成功并消费、内容不匹配、键不存在或已过期。Redis 自动删除过期键，因此不能区分已过期和从未存在。
    enum class VerifyCodeResult
    {
        Consumed,
        Mismatch,
        MissingOrExpired
    };

    // 验证码仅保存在 Redis，通过 WATCH 和事务保证成功消费一次。
    class VerifyCodeData
    {
    public:
        /**
         * @brief 使用已创建的客户端。
         * @param redis Redis 客户端。
         */
        explicit VerifyCodeData(std::shared_ptr<sw::redis::Redis> redis);
        /**
         * @brief 保存验证码及剩余有效期。
         * @param code 验证码信息。
         * @return 保存时为 true，已过期时为 false。
         */
        bool Save(const CachedVerifyCode &code);
        /**
         * @brief 查询验证码。
         * @param codeId 验证码 ID。
         * @return 验证码信息；不存在或过期时为空。
         */
        std::optional<CachedVerifyCode> Find(const std::string &codeId);
        /**
         * @brief 删除验证码。
         * @param codeId 验证码 ID。
         */
        void Delete(const std::string &codeId);
        /**
         * @brief 原子校验并消费验证码。
         * @param codeId 验证码 ID。
         * @param email 邮箱。
         * @param verifyCode 验证码。
         * @return 校验与消费结果。
         */
        VerifyCodeResult Consume(const std::string &codeId, const std::string &email,
                                 const std::string &verifyCode);

    private:
        std::shared_ptr<sw::redis::Redis> _redis;
    };
}
