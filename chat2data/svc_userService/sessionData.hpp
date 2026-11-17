#pragma once

#include "sessionEntity.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sw::redis
{
    class Redis;
}

namespace odb
{
    class database;
}

namespace chat2Data
{
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

    // 登录会话数据访问入口，MySQL 与 Redis 操作分别由独立接口提供。
    // 退出流程由业务层依次删除缓存、删除数据库记录、等待 500 毫秒并再次删除缓存。
    // 业务层还需处理并发回填和缓存删除失败，避免已退出的会话重新生效。
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
         * @return 会话状态及有效信息。
         */
        SessionResult Find(const std::string &sessionId);
        /**
         * @brief 删除 MySQL 中的会话。
         * @param sessionId 登录会话 ID。
         */
        void Delete(const std::string &sessionId);
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
