#include "testSupport.hpp"
#include "verifyCodeData.hpp"

namespace
{
    using namespace user_data_test;

    class VerifyCodeDataTest : public DataTestFixture {};

    // 错误验证码不能消费记录，并发校验只能有一次成功。
    TEST_F(VerifyCodeDataTest, ConsumesVerificationCodeOnceUnderConcurrency)
    {
        chat2Data::VerifyCodeData codes(redis);
        const auto id = Track("verifyCode", prefix);
        const std::string email = prefix + "@example.com";
        ASSERT_TRUE(codes.Save({id, "001234", email, NowMs()}));
        ASSERT_TRUE(codes.Find(id));
        EXPECT_EQ(codes.Consume(id, email, "999999"), chat2Data::VerifyCodeResult::Mismatch);
        EXPECT_EQ(codes.Consume(id, "other@example.com", "001234"), chat2Data::VerifyCodeResult::Mismatch);
        EXPECT_GT(redis->pttl("verifyCode:" + id), 0);
        EXPECT_LE(redis->pttl("verifyCode:" + id), 300000);

        std::vector<std::future<chat2Data::VerifyCodeResult>> attempts;
        for (int i = 0; i < 8; ++i)
            attempts.push_back(std::async(std::launch::async, [&] { return codes.Consume(id, email, "001234"); }));
        int consumed = 0;
        for (auto &attempt : attempts)
            consumed += attempt.get() == chat2Data::VerifyCodeResult::Consumed;
        EXPECT_EQ(consumed, 1);
        EXPECT_EQ(codes.Consume(id, email, "001234"), chat2Data::VerifyCodeResult::MissingOrExpired);
        EXPECT_FALSE(codes.Find(id));
    }

    // 验证码过期后不能继续使用。
    TEST_F(VerifyCodeDataTest, VerificationCodeExpires)
    {
        chat2Data::VerifyCodeData codes(redis);
        const auto id = Track("verifyCode", prefix);
        const auto email = prefix + "@example.com";
        EXPECT_FALSE(codes.Save({id, "001234", email, NowMs() - 300001}));
        ASSERT_TRUE(codes.Save({id, "001234", email, NowMs()}));
        redis->pexpire("verifyCode:" + id, std::chrono::milliseconds(1));
        for (int i = 0; i < 100 && redis->exists("verifyCode:" + id); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_EQ(codes.Consume(id, email, "001234"), chat2Data::VerifyCodeResult::MissingOrExpired);
    }

    // 主动删除验证码后，查询和消费都应表明记录不存在。
    TEST_F(VerifyCodeDataTest, DeletesVerificationCode)
    {
        chat2Data::VerifyCodeData codes(redis);
        const auto id = Track("verifyCode", prefix);
        const auto email = prefix + "@example.com";
        ASSERT_TRUE(codes.Save({id, "001234", email, NowMs()}));
        ASSERT_TRUE(codes.Find(id));
        codes.Delete(id);
        EXPECT_FALSE(codes.Find(id));
        EXPECT_EQ(codes.Consume(id, email, "001234"), chat2Data::VerifyCodeResult::MissingOrExpired);
    }

    // 验证码缓存结构损坏时应报告错误。
    TEST_F(VerifyCodeDataTest, InvalidCodeCacheDoesNotAppearAsMissing)
    {
        chat2Data::VerifyCodeData codes(redis);
        const auto codeId = Track("verifyCode", prefix);
        redis->hset("verifyCode:" + codeId, "wrong-field", "value");
        EXPECT_THROW(codes.Consume(codeId, "user@example.com", "001234"), chat2Data::Chat2DataException);
    }

}
