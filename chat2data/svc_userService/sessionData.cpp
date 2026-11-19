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
        // 会话缓存键和数据库实体到缓存字段的转换。
        std::string SessionKey(const std::string &id) { return "session:" + id; }
        CachedSession ToCache(const SessionEntity &session)
        {
            return {session.sessionId, session.userId, session.expiresAt};
        }

        // 构造ODB查询，先用索引过滤sessionId，再通过二进制比对实现字节级严格匹配，对齐Redis Key的大小写敏感语义，防止MySQL排序规则造成错误命中
        odb::query<SessionEntity> ExactSessionId(const std::string &sessionId)
        {
            using Query = odb::query<SessionEntity>;
            // MySQL 的文本排序规则与 Redis 的键比较不同，凭证按原始字节匹配。
            return (Query::sessionId == sessionId) && (Query("CAST(`tbl_session`.`sessionId` AS BINARY) =") + Query::_val(sessionId));
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
    SessionResult SessionData::Find(const std::string &sessionId, bool fillCache)
    {
        auto query = ExactSessionId(sessionId);
        // 与退出删除争用同一会话行锁，已开始的回填会在删除语句执行前结束。
        if (fillCache)
            query += " FOR UPDATE";

        odb::transaction transaction(_database->begin());
        const std::unique_ptr<SessionEntity> session(_database->query_one<SessionEntity>(query));
        SessionResult result{SessionStatus::NotFound, std::nullopt};
        // 如果查询到会话，检查过期时间；如果未过期，回填缓存，如果过期
        if (session)
        {
            result.status = SessionStatus::Expired;
            if (session->expiresAt > Utils::NowMs())
            {
                auto value = ToCache(*session);
                if ((!fillCache || SaveCache(value)) && value.expiresAt > Utils::NowMs())
                    result = {SessionStatus::Found, std::move(value)};
            }
        }
        transaction.commit();
        return result;
    }

    // 查询指定用户的所有未过期会话
    std::vector<CachedSession> SessionData::FindValidByUserId(const std::string &userId)
    {
        using Query = odb::query<SessionEntity>;
        odb::transaction transaction(_database->begin());
        std::vector<CachedSession> sessions;
        for (const auto &session : _database->query<SessionEntity>(
                 Query::userId == userId && Query::expiresAt > Utils::NowMs()))
            sessions.push_back(ToCache(session));
        transaction.commit();
        return sessions;
    }

    // 查询会话归属，包括已过期记录，供业务层校正在线状态
    std::optional<std::string> SessionData::FindOwner(const std::string &sessionId)
    {
        odb::transaction transaction(_database->begin());
        const std::unique_ptr<SessionEntity> session(_database->query_one<SessionEntity>(ExactSessionId(sessionId)));
        transaction.commit();
        return session ? std::optional<std::string>(session->userId) : std::nullopt;
    }

    // 删除 MySQL 中的会话，事务提交前可执行回调操作
    void SessionData::Delete(const std::string &sessionId, const std::function<void(odb::transaction &)> &beforeCommit)
    {
        odb::transaction transaction(_database->begin());
        // ODB 生成 DELETE FROM tbl_session WHERE sessionId = ?
        _database->erase_query<SessionEntity>(ExactSessionId(sessionId));
        // 若设置了回调，则在删除语句执行后、事务提交前执行，允许业务层在事务内处理其他数据。
        if (beforeCommit)
            beforeCommit(transaction);
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
        // 这里返回剩余时间和三天的较小值，避免回填延长会话有效期
        Utils::WriteCache(*_redis, SessionKey(session.sessionId), value, std::min(kSessionLifetimeMs, session.expiresAt - now));
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
