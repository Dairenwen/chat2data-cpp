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
        session.sessionId = Utils::GenerateSecureId();
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
        auto result = _sessions->FindCache(sessionId);
        if (result.status == SessionStatus::NotFound)
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

    void SessionManager::Delete(const std::string &sessionId,
                                const std::function<void(odb::transaction &)> &updateStatus)
    {
        InvalidateCache(sessionId);
        _sessions->Delete(sessionId, [&](odb::transaction &transaction)
                          {
            // 删除事务持有会话行锁，使并发回填查询等待事务结束。
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // 第二次删缓存成功后提交删除，失败则由数据库事务回滚。
            InvalidateCache(sessionId);
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
            // DEL 可以安全重试，重试失败由调用方处理。
            _sessions->DeleteCache(sessionId);
        }
    }
}
