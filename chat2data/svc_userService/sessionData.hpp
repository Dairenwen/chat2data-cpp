#pragma once

#include "sessionEntity.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sw::redis
{
    class Redis;
}

namespace odb
{
    class database;
    class transaction;
}

namespace chat2Data
{
    inline constexpr std::uint64_t kSessionLifetimeMs = 3ULL * 24 * 60 * 60 * 1000;

    struct CachedSession
    {
        std::string sessionId;       // 登录会话 ID。
        std::string userId;          // 会话所属用户 ID。
        std::uint64_t expiresAt = 0; // 绝对过期时间，Unix 毫秒。
    };

    // 会话有效、未找到和已过期是不同的查询结果。
    enum class SessionStatus
    {
        Found,
        NotFound,
        Expired
    };
    struct SessionResult
    {
        SessionStatus status;                 // 查询状态。
        std::optional<CachedSession> session; // 仅有效会话包含数据。
    };

    // 登录会话数据访问入口。
    class SessionData
    {
    public:
        /**
         * @brief 使用已创建的客户端。
         * @param database MySQL 主库。
         * @param redis Redis 客户端。
         */
        SessionData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis);
        /**
         * @brief 将会话保存到 MySQL。
         * @param session 会话对象；数据库生成的主键写回此对象。
         */
        void Save(SessionEntity &session);
        /**
         * @brief 在 MySQL 中查询会话并检查有效期。
         * @param sessionId 登录会话 ID。
         * @param fillCache 为 true 时，在数据库行锁内回填有效会话。
         * @return 会话状态及有效信息。
         */
        SessionResult Find(const std::string &sessionId, bool fillCache = false);
        /**
         * @brief 从 MySQL 查询指定用户的所有未过期会话。
         * @param userId 用户 ID。
         * @return 查询时有效的会话列表；没有有效会话时为空。
         */
        std::vector<CachedSession> FindValidByUserId(const std::string &userId);
        /** @brief 查询会话归属，包括已过期记录，供业务层校正在线状态。 */
        std::optional<std::string> FindOwner(const std::string &sessionId);
        /**
         * @brief 删除 MySQL 中的会话。
         * @param sessionId 登录会话 ID。
         * @param beforeCommit 删除语句执行后、事务提交前执行的操作。
         */
        void Delete(const std::string &sessionId, const std::function<void(odb::transaction &)> &beforeCommit = {});
        /**
         * @brief 将未过期会话保存到 Redis。
         * @param session 会话信息。
         * @return 保存时为 true，已过期时为 false。
         */
        bool SaveCache(const CachedSession &session);
        /**
         * @brief 读取会话缓存并检查过期时间。
         * @param sessionId 登录会话 ID。
         * @return 会话状态及有效信息。
         */
        SessionResult FindCache(const std::string &sessionId);
        /**
         * @brief 删除会话缓存。
         * @param sessionId 登录会话 ID。
         */
        void DeleteCache(const std::string &sessionId);

    private:
        std::shared_ptr<odb::database> _database;
        std::shared_ptr<sw::redis::Redis> _redis;
    };

}
