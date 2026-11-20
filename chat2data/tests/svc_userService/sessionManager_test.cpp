#include "sessionEntity-odb.hxx"
#include "sessionManager.hpp"
#include "testSupport.hpp"

#include <stdexcept>

namespace
{
    using namespace chat2Data;

    class SessionManagerTest : public user_data_test::DataTestFixture
    {
    };

    // 删除回调在缓存清理后运行，并通过尚未提交的事务读取删除结果。
    TEST_F(SessionManagerTest, RunsCallbackAfterDelayInsideDeleteTransaction)
    {
        const auto sessions = std::make_shared<SessionData>(database, redis);
        SessionManager manager(sessions);
        const auto session_id = Track("session", manager.Create(prefix));
        ASSERT_EQ(manager.Find(session_id).status, SessionStatus::Found);
        ASSERT_EQ(sessions->FindCache(session_id).status, SessionStatus::Found);

        const auto started = std::chrono::steady_clock::now();
        bool callback_called = false;
        manager.Delete(session_id, [&](odb::transaction &transaction)
        {
            callback_called = true;
            EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
            EXPECT_EQ(&transaction, &odb::transaction::current());
            EXPECT_EQ(sessions->FindCache(session_id).status, SessionStatus::NotFound);

            using Query = odb::query<SessionEntity>;
            const std::unique_ptr<SessionEntity> row(transaction.database().query_one<SessionEntity>(
                Query::sessionId == session_id));
            EXPECT_FALSE(row);
        });

        EXPECT_TRUE(callback_called);
        EXPECT_EQ(sessions->Find(session_id).status, SessionStatus::NotFound);
        EXPECT_FALSE(manager.GetUserId(session_id));
    }

    // 回调失败时上报异常，未提交的会话删除随事务回滚。
    TEST_F(SessionManagerTest, RollsBackSessionDeletionWhenCallbackThrows)
    {
        const auto sessions = std::make_shared<SessionData>(database, redis);
        SessionManager manager(sessions);
        const auto session_id = Track("session", manager.Create(prefix));

        EXPECT_THROW(manager.Delete(session_id, [](odb::transaction &)
        {
            throw std::runtime_error("模拟提交前回调失败");
        }), std::runtime_error);
        EXPECT_EQ(sessions->FindCache(session_id).status, SessionStatus::NotFound);
        EXPECT_EQ(manager.GetUserId(session_id), prefix);
        EXPECT_EQ(sessions->FindCache(session_id).status, SessionStatus::Found);
    }

    // 第一次删缓存最终失败时报告 Redis 错误，数据库仍保留会话。
    TEST_F(SessionManagerTest, PreservesSessionWhenRedisIsUnavailable)
    {
        const auto sessions = std::make_shared<SessionData>(database, redis);
        SessionManager manager(sessions);
        const auto session_id = Track("session", manager.Create(prefix));
        sw::redis::ConnectionOptions options;
        options.host = "127.0.0.1";
        options.port = 1;
        const auto unavailable = std::make_shared<sw::redis::Redis>(options);
        SessionManager failing(std::make_shared<SessionData>(database, unavailable));

        EXPECT_THROW(failing.Delete(session_id), sw::redis::Error);
        EXPECT_EQ(sessions->Find(session_id).status, SessionStatus::Found);
    }
}
