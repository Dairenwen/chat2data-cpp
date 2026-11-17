#include "testSupport.hpp"

// 开发容器中的服务名和默认端口；凭证由运行参数提供。
DEFINE_string(mysql_host, "dev-mysql", "MySQL 服务地址");
DEFINE_uint32(mysql_port, 3306, "MySQL 服务端口");
DEFINE_string(mysql_user, "root", "MySQL 用户名");
DEFINE_string(mysql_password, "", "MySQL 密码");
DEFINE_string(mysql_database, "chat2data", "MySQL 数据库名");
DEFINE_string(redis_host, "dev-redis", "Redis 服务地址");
DEFINE_int32(redis_port, 6379, "Redis 服务端口");
DEFINE_string(redis_user, "default", "Redis 用户名");
DEFINE_string(redis_password, "", "Redis 密码");

namespace
{
    using namespace user_data_test;

    class UserDataTest : public DataTestFixture {};

    // 验证中文用户信息查询、缓存有效期及数据库唯一约束。
    TEST_F(UserDataTest, SavesQueriesAndEnforcesUniqueNickname)
    {
        chat2Data::UserData users(database, redis);
        chat2Data::UserEntity user;
        user.userId = Track("user", prefix);
        user.nickname = u8"测试用户" + prefix;
        user.email = prefix + "@example.com";
        user.password = "stored-hash";
        users.Save(user);
        ASSERT_GT(user.id, 0u);

        const auto byId = users.FindById(user.userId);
        ASSERT_TRUE(byId);
        EXPECT_EQ(byId->nickname, user.nickname);
        ASSERT_TRUE(users.FindByNickname(user.nickname));
        ASSERT_TRUE(users.FindByEmail(user.email));
        EXPECT_TRUE(users.NicknameExists(user.nickname));
        EXPECT_TRUE(users.EmailExists(user.email));
        EXPECT_FALSE(users.NicknameExists(prefix + "-missing"));
        EXPECT_FALSE(users.FindById(prefix + "-missing"));
        EXPECT_FALSE(users.FindCache(user.userId));
        users.SaveCache(*byId);
        ASSERT_TRUE(users.FindCache(user.userId));
        EXPECT_GT(redis->pttl("user:" + user.userId), 0);
        EXPECT_LE(redis->pttl("user:" + user.userId), 3600000);

        chat2Data::UserEntity duplicate;
        duplicate.userId = prefix + "-other";
        duplicate.nickname = user.nickname;
        duplicate.email = prefix + "-other@example.com";
        duplicate.password = "stored-hash";
        EXPECT_THROW(users.Save(duplicate), odb::exception);
        duplicate.userId = user.userId;
        duplicate.nickname = prefix + "-other";
        EXPECT_THROW(users.Save(duplicate), odb::exception);
        duplicate.userId = prefix + "-other";
        duplicate.email = user.email;
        EXPECT_THROW(users.Save(duplicate), odb::exception);
    }

    // 损坏的缓存应报告错误，不能当成缓存未命中。
    TEST_F(UserDataTest, InvalidUserCacheDoesNotAppearAsMissing)
    {
        chat2Data::UserData users(database, redis);
        const auto id = Track("user", prefix);
        redis->hset("user:" + id, "data", "{");
        redis->expire("user:" + id, std::chrono::seconds(60));
        EXPECT_THROW(users.FindCache(id), chat2Data::Chat2DataException);
        redis->del("user:" + id);
        redis->hset("user:" + id, "wrong-field", "value");
        EXPECT_THROW(users.FindCache(id), chat2Data::Chat2DataException);
    }

    // Redis 不可用时，MySQL 操作仍可独立执行；缓存错误向调用方报告。
    TEST_F(UserDataTest, DatabaseAndCacheOperationsAreIndependent)
    {
        sw::redis::ConnectionOptions options;
        options.host = "127.0.0.1";
        options.port = 1;
        auto unavailable = std::make_shared<sw::redis::Redis>(options);
        chat2Data::UserData users(database, unavailable);
        chat2Data::UserEntity user;
        user.userId = Track("user", prefix);
        user.nickname = prefix;
        user.email = prefix + "@example.com";
        user.password = "stored-hash";
        EXPECT_NO_THROW(users.Save(user));
        const auto found = users.FindById(user.userId);
        ASSERT_TRUE(found);
        EXPECT_THROW(users.SaveCache(*found), sw::redis::Error);
    }
}

int main(int argc, char **argv)
{
    // 先解析 Google Test 参数，再将连接参数交给 gflags。
    ::testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}
