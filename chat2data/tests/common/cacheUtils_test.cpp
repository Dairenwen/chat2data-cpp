#include "../svc_userService/testSupport.hpp"
#include "utils.hpp"
#include <json/json.h>

namespace
{
    using namespace user_data_test;

    class CacheUtilsTest : public DataTestFixture {};

    // 验证缓存读写、过期时间和缺少字段时的错误处理。
    TEST_F(CacheUtilsTest, ReadsAndWritesHashWithExpiry)
    {
        const auto id = Track("utility", prefix);
        const auto key = "utility:" + id;
        EXPECT_FALSE(chat2Data::Utils::ReadCache(*redis, key));

        Json::Value value(Json::objectValue);
        value["name"] = u8"测试";
        chat2Data::Utils::WriteCache(*redis, key, value, 60000);
        const auto text = chat2Data::Utils::ReadCache(*redis, key);
        ASSERT_TRUE(text);
        EXPECT_EQ(chat2Data::Utils::ParseCacheJson(*text)["name"].asString(), u8"测试");
        EXPECT_GT(redis->pttl(key), 0);
        EXPECT_LE(redis->pttl(key), 60000);

        redis->del(key);
        redis->hset(key, "other", "value");
        EXPECT_THROW(chat2Data::Utils::ReadCache(*redis, key), chat2Data::Chat2DataException);
        EXPECT_TRUE(chat2Data::Utils::ParseCacheJson("{}").isObject());
        EXPECT_THROW(chat2Data::Utils::ParseCacheJson("[]"), chat2Data::Chat2DataException);
    }

    // 工具函数返回 Unix 毫秒，能够用于现有会话和验证码的有效期比较。
    TEST_F(CacheUtilsTest, ReturnsCurrentUnixMilliseconds)
    {
        const auto before = NowMs();
        const auto current = chat2Data::Utils::NowMs();
        const auto after = NowMs();
        EXPECT_GE(current, before);
        EXPECT_LE(current, after);
    }
}
