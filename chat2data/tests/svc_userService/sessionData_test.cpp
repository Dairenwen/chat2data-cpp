#include "testSupport.hpp"
#include "sessionData.hpp"

namespace
{
    using namespace user_data_test;

    class SessionDataTest : public DataTestFixture {};

    // 数据库删除和缓存删除分别调用，供业务层编排退出流程。
    TEST_F(SessionDataTest, DeletesDatabaseAndCacheSeparately)
    {
        chat2Data::SessionData sessions(database, redis);
        chat2Data::SessionEntity session;
        session.sessionId = Track("session", prefix);
        session.userId = prefix + "-user";
        session.expiresAt = NowMs() + 30000;
        sessions.Save(session);
        ASSERT_GT(session.id, 0u);
        ASSERT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::Found);
        ASSERT_TRUE(sessions.SaveCache({session.sessionId, session.userId, session.expiresAt}));
        EXPECT_LE(redis->pttl("session:" + session.sessionId), 30000);
        sessions.Delete(session.sessionId);
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::NotFound);
        EXPECT_EQ(sessions.FindCache(session.sessionId).status, chat2Data::SessionStatus::Found);
        sessions.DeleteCache(session.sessionId);
        EXPECT_FALSE(redis->exists("session:" + session.sessionId));
        EXPECT_EQ(sessions.FindCache(session.sessionId).status, chat2Data::SessionStatus::NotFound);
    }

    // Redis 与 MySQL 统一按原始字节识别会话 ID。
    TEST_F(SessionDataTest, RequiresExactSessionId)
    {
        chat2Data::SessionData sessions(database, redis);
        chat2Data::SessionEntity session;
        session.sessionId = Track("session", prefix + "-a");
        session.userId = prefix + "-user";
        session.expiresAt = NowMs() + 30000;
        sessions.Save(session);

        const auto differentId = prefix + "-A";
        EXPECT_EQ(sessions.Find(differentId, true).status, chat2Data::SessionStatus::NotFound);
        EXPECT_FALSE(sessions.FindOwner(differentId));
        sessions.Delete(differentId);
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::Found);
    }

    // 提交前的失效步骤失败时，数据库回滚会话删除。
    TEST_F(SessionDataTest, RollsBackWhenDeleteCannotCommit)
    {
        chat2Data::SessionData sessions(database, redis);
        chat2Data::SessionEntity session;
        session.sessionId = prefix;
        session.userId = prefix + "-user";
        session.expiresAt = NowMs() + 30000;
        sessions.Save(session);

        EXPECT_THROW(sessions.Delete(session.sessionId, [](odb::transaction &)
        {
            throw chat2Data::Chat2DataException(chat2Data::ErrorCode::USER_CACHE_INVALID);
        }), chat2Data::Chat2DataException);
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::Found);
    }

    // 会话缓存有效期不超过设计规定的三天。
    TEST_F(SessionDataTest, SessionCacheDoesNotExceedThreeDays)
    {
        chat2Data::SessionData sessions(database, redis);
        const auto id = Track("session", prefix);
        ASSERT_TRUE(sessions.SaveCache({id, prefix + "-user", NowMs() + 4ULL * 24 * 3600 * 1000}));
        EXPECT_GT(redis->pttl("session:" + id), 0);
        EXPECT_LE(redis->pttl("session:" + id), 3LL * 24 * 3600 * 1000);
    }

    // Redis 不可用时，保存、查询和删除数据库会话仍可独立完成。
    TEST_F(SessionDataTest, DatabaseOperationsDoNotRequireRedis)
    {
        sw::redis::ConnectionOptions options;
        options.host = "127.0.0.1";
        options.port = 1;
        auto unavailable = std::make_shared<sw::redis::Redis>(options);
        chat2Data::SessionData sessions(database, unavailable);
        chat2Data::SessionEntity session;
        session.sessionId = prefix;
        session.userId = prefix + "-user";
        session.expiresAt = NowMs() + 30000;
        EXPECT_NO_THROW(sessions.Save(session));
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::Found);
        EXPECT_NO_THROW(sessions.Delete(session.sessionId));
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::NotFound);
    }

    // 已过期的数据库会话不能通过认证或写入缓存。
    TEST_F(SessionDataTest, RejectsExpiredSession)
    {
        chat2Data::SessionData sessions(database, redis);
        chat2Data::SessionEntity session;
        session.sessionId = Track("session", prefix);
        session.userId = prefix + "-user";
        session.expiresAt = NowMs() - 1;
        sessions.Save(session);
        EXPECT_EQ(sessions.Find(session.sessionId).status, chat2Data::SessionStatus::Expired);
        EXPECT_FALSE(sessions.SaveCache({session.sessionId, session.userId, session.expiresAt}));
        EXPECT_EQ(sessions.FindCache(session.sessionId).status, chat2Data::SessionStatus::NotFound);
    }

    // 数据库不可用时，Redis 查询仍可独立执行。
    TEST_F(SessionDataTest, ReadsCacheWhenDatabaseUnavailable)
    {
        const auto sessionId = Track("session", prefix);
        chat2Data::SessionData cache(database, redis);
        ASSERT_TRUE(cache.SaveCache({sessionId, prefix + "-user", NowMs() + 30000}));

        BreezeODB::mysql_settings unavailableSettings;
        unavailableSettings.host = "127.0.0.1";
        unavailableSettings.port = 1;
        unavailableSettings.db = "unavailable";
        unavailableSettings.cset = "utf8mb4";
        auto unavailable = BreezeODB::DBFactory::mysql(unavailableSettings);
        chat2Data::SessionData sessions(unavailable, redis);
        EXPECT_EQ(sessions.FindCache(sessionId).status, chat2Data::SessionStatus::Found);
        sessions.DeleteCache(sessionId);
        EXPECT_EQ(sessions.FindCache(sessionId).status, chat2Data::SessionStatus::NotFound);
        EXPECT_THROW(sessions.Find(sessionId), odb::exception);
    }

}
