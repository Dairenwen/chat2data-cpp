#include "sessionData.hpp"
#include "sessionEntity-odb.hxx"

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
        constexpr std::uint64_t kSessionTtlMs = 3ULL * 24 * 3600 * 1000;
        // 会话缓存键和数据库实体到缓存字段的转换。
        std::string SessionKey(const std::string &id) { return "session:" + id; }
        CachedSession ToCache(const SessionEntity &session)
        {
            return {session.sessionId, session.userId, session.expiresAt};
        }
    }

    // 保存调用方传入的数据库和 Redis 客户端。
    SessionData::SessionData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis)
        : _database(std::move(database)), _redis(std::move(redis)) {}

    // 将会话写入 MySQL，缓存操作由业务层负责。
    void SessionData::Save(SessionEntity &session)
    {
        odb::transaction transaction(_database->begin());
        // ODB 生成 INSERT INTO tbl_session (...) VALUES (...)，主键写回 session.id。
        _database->persist(session);
        transaction.commit();
    }

    // 查询 MySQL 中的会话并检查有效期。
    SessionResult SessionData::Find(const std::string &sessionId)
    {
        // 对应查询条件：SELECT ... FROM tbl_session WHERE sessionId = ?。
        odb::transaction transaction(_database->begin());
        std::unique_ptr<SessionEntity> session(_database->query_one<SessionEntity>(odb::query<SessionEntity>::sessionId == sessionId));
        transaction.commit();
        if (!session)
            return {SessionStatus::NotFound, std::nullopt};
        if (session->expiresAt <= Utils::NowMs())
            return {SessionStatus::Expired, std::nullopt};
        return {SessionStatus::Found, ToCache(*session)};
    }

    // 直接删除 MySQL 中的会话；延迟双删由业务层组织。
    void SessionData::Delete(const std::string &sessionId)
    {
        odb::transaction transaction(_database->begin());
        // ODB 生成 DELETE FROM tbl_session WHERE sessionId = ?，无需先查询记录。
        _database->erase_query<SessionEntity>(odb::query<SessionEntity>::sessionId == sessionId);
        transaction.commit();
    }

    // TTL 取三天和会话剩余时间中的较小值，避免回填延长会话有效期。
    bool SessionData::SaveCache(const CachedSession &session)
    {
        const auto now = Utils::NowMs();
        if (session.expiresAt <= now)
            return false;
        Json::Value value(Json::objectValue);
        value["sessionId"] = session.sessionId;
        value["userId"] = session.userId;
        value["expiresAt"] = Json::UInt64(session.expiresAt);
        Utils::WriteCache(*_redis, SessionKey(session.sessionId), value, std::min(kSessionTtlMs, session.expiresAt - now));
        return true;
    }

    // 读取会话并检查字段、会话 ID 和绝对过期时间。
    SessionResult SessionData::FindCache(const std::string &sessionId)
    {
        const auto text = Utils::ReadCache(*_redis, SessionKey(sessionId));
        if (!text)
            return {SessionStatus::NotFound, std::nullopt};
        const auto value = Utils::ParseCacheJson(*text);
        if (!value["sessionId"].isString() || value["sessionId"].asString() != sessionId ||
            !value["userId"].isString() || !value["expiresAt"].isUInt64())
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
        const auto expiresAt = value["expiresAt"].asUInt64();
        if (expiresAt <= Utils::NowMs())
            return {SessionStatus::Expired, std::nullopt};
        return {SessionStatus::Found, CachedSession{sessionId, value["userId"].asString(), expiresAt}};
    }

    // 执行 DEL session:{sessionId}，删除该会话的缓存键。
    void SessionData::DeleteCache(const std::string &sessionId) { _redis->del(SessionKey(sessionId)); }

}
