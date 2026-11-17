#include "verifyCodeData.hpp"
#include <sw/redis++/transaction.h>
#include <sw/redis++/queued_redis.h>

#include "errorHandler.hpp"
#include "utils.hpp"

#include <json/json.h>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <sw/redis++/redis.h>
#include <algorithm>
#include <utility>

namespace chat2Data
{
    namespace
    {
        constexpr std::uint64_t kCodeTtlMs = 300ULL * 1000;
        std::string CodeKey(const std::string &id) { return "verifyCode:" + id; }

        // 按创建时间判断验证码是否超过五分钟。
        bool CodeExpired(std::uint64_t createdAt, std::uint64_t now)
        {
            return createdAt <= now && now - createdAt >= kCodeTtlMs;
        }

        // 查询和消费共用字段校验，避免将损坏的数据当成有效验证码。
        CachedVerifyCode ParseCode(const std::string &text, const std::string &codeId)
        {
            const auto value = Utils::ParseCacheJson(text);
            if (!value["codeId"].isString() || value["codeId"].asString() != codeId ||
                !value["verifyCode"].isString() || !value["email"].isString() || !value["createdAt"].isUInt64())
                throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
            return {codeId, value["verifyCode"].asString(), value["email"].asString(), value["createdAt"].asUInt64()};
        }
    }

    // 验证码仅依赖 Redis，不使用数据库。
    VerifyCodeData::VerifyCodeData(std::shared_ptr<sw::redis::Redis> redis) : _redis(std::move(redis)) {}

    // 已过期的验证码不写入缓存；其余情况按剩余有效期设置 TTL。
    bool VerifyCodeData::Save(const CachedVerifyCode &code)
    {
        const auto now = Utils::NowMs();
        if (CodeExpired(code.createdAt, now))
            return false;
        const auto remaining = code.createdAt > now ? kCodeTtlMs : kCodeTtlMs - (now - code.createdAt);
        Json::Value value(Json::objectValue);
        value["codeId"] = code.codeId;
        value["verifyCode"] = code.verifyCode;
        value["email"] = code.email;
        value["createdAt"] = Json::UInt64(code.createdAt);
        Utils::WriteCache(*_redis, CodeKey(code.codeId), value, remaining);
        return true;
    }

    // 返回尚未过期的验证码；缓存缺失和已过期均返回空值。
    std::optional<CachedVerifyCode> VerifyCodeData::Find(const std::string &codeId)
    {
        const auto text = Utils::ReadCache(*_redis, CodeKey(codeId));
        if (!text)
            return std::nullopt;
        auto code = ParseCode(*text, codeId);
        if (CodeExpired(code.createdAt, Utils::NowMs()))
            return std::nullopt;
        return code;
    }

    // 执行 DEL verifyCode:{codeId}，删除该验证码的缓存键。
    void VerifyCodeData::Delete(const std::string &codeId) { _redis->del(CodeKey(codeId)); }

    // WATCH 监视验证码键；并发修改时重新读取，确保只有一次成功消费。
    VerifyCodeResult VerifyCodeData::Consume(const std::string &codeId, const std::string &email, const std::string &verifyCode)
    {
        const auto key = CodeKey(codeId);
        while (true)
        {
            // 独立连接确保 WATCH、读取和事务使用同一连接。
            auto transaction = _redis->transaction();
            auto connection = transaction.redis();
            // WATCH key 后读取 HGET key data；在 EXEC 前发生修改会导致事务冲突。
            connection.watch(key);
            auto text = connection.hget(key, "data");
            if (!text)
            {
                // EXISTS key 区分验证码不存在和 Hash 缺少 data 字段。
                if (connection.exists(key))
                    throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
                return VerifyCodeResult::MissingOrExpired;
            }
            const auto code = ParseCode(*text, codeId);
            // PTTL key 的 -2 表示键已消失，-1 表示键未设置过期时间。
            const auto ttl = connection.pttl(key);
            if (ttl == -2)
                return VerifyCodeResult::MissingOrExpired;
            if (ttl < 0)
                throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
            const bool expired = CodeExpired(code.createdAt, Utils::NowMs());
            if (!expired && (code.email != email || code.verifyCode != verifyCode))
                return VerifyCodeResult::Mismatch;
            // MULTI、DEL key、EXEC：仅当 WATCH 的键未变化时提交删除。
            transaction.del(key);
            try
            {
                transaction.exec();
                return expired ? VerifyCodeResult::MissingOrExpired : VerifyCodeResult::Consumed;
            }
            catch (const sw::redis::WatchError &)
            {
                // 期间键已变化，重新检查最新验证码。
            }
        }
    }
}
