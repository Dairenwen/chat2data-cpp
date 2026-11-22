#include "sessionManager.hpp"

#include "utils.hpp"

#include <chrono>
#include <sw/redis++/errors.h>
#include <thread>
#include <utility>

namespace chat2Data
{
    SessionManager::SessionManager(std::shared_ptr<SessionData> sessions)
        : _sessions(std::move(sessions))
    {
    }

    std::string SessionManager::Create(const std::string &userId)
    {
        SessionEntity session;
        session.sessionId = Utils::GenerateSecureId(); // 调用 Utils::GenerateSecureId() 生成一个安全的会话 ID
        session.userId = userId;
        session.expiresAt = Utils::NowMs() + kSessionLifetimeMs;
        _sessions->Save(session);
        try
        {
            _sessions->DeleteCache(session.sessionId);
        }
        catch (const sw::redis::Error &)
        {
            // 撤销尚未交付的数据库记录，避免失败的创建留下有效会话。
            DiscardUnissued(session.sessionId);
            throw;
        }
        return session.sessionId;
    }

    SessionResult SessionManager::Find(const std::string &sessionId)
    {
        auto result = _sessions->FindCache(sessionId); // 直接去访问redis
        if (result.status == SessionStatus::NotFound)  // 如果没有命中缓存，则查询数据库并回填缓存，会采用行锁避免与删除争用
            return _sessions->Find(sessionId, true);
        return result;
    }

    std::optional<std::string> SessionManager::GetUserId(const std::string &sessionId)
    {
        const auto result = Find(sessionId);
        if (result.session)
            return result.session->userId;
        return std::nullopt;
    }

    std::optional<std::string> SessionManager::FindOwner(const std::string &sessionId)
    {
        return _sessions->FindOwner(sessionId);
    }

    void SessionManager::Delete(const std::string &sessionId, const std::function<void(odb::transaction &)> &updateStatus)
    {
        // 先删除缓存
        InvalidateCache(sessionId);
        // 再来删除数据库，事务提交前可执行回调操作校正用户在线状态
        _sessions->Delete(sessionId, [&](odb::transaction &transaction)
                          {
                            // 删除事务持有会话行锁，使并发回填查询等待事务结束。
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            // 第二次删缓存成功后提交删除，失败则由数据库事务回滚。
                            InvalidateCache(sessionId);
                            // 如果提供了回调，在事务提交前执行，使用同一事务校正用户状态
                            if (updateStatus)
                                updateStatus(transaction); });
    }

    // 删除尚未交付的会话，避免失败的创建留下有效会话。
    void SessionManager::DiscardUnissued(const std::string &sessionId)
    {
        _sessions->Delete(sessionId);
    }

    void SessionManager::InvalidateCache(const std::string &sessionId)
    {
        try
        {
            _sessions->DeleteCache(sessionId);
        }
        catch (const sw::redis::Error &)
        {
            // 删除缓存失败时默认再次尝试，避免影响数据库事务提交。若仍失败则继续抛出。
            _sessions->DeleteCache(sessionId);
        }
    }
}
