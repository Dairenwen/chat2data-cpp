#include "utils.hpp"

#include <gtest/gtest.h>
#include <regex>

namespace
{
    using chat2Data::Utils;

    // 会话 ID 和验证码 ID 使用固定长度的十六进制字符串，每次独立生成。
    TEST(CredentialUtilsTest, GeneratesDistinctHexIdentifiers)
    {
        const std::regex format("[0-9a-f]{32}");
        const auto first = Utils::GenerateSecureId();
        const auto second = Utils::GenerateSecureId();

        EXPECT_TRUE(std::regex_match(first, format));
        EXPECT_TRUE(std::regex_match(second, format));
        EXPECT_TRUE(first != second);
    }

    // 验证码以六位数字字符串返回，可保留前导零。
    TEST(CredentialUtilsTest, GeneratesSixDigitVerificationCode)
    {
        const auto code = Utils::GenerateVerificationCode();

        EXPECT_EQ(code.size(), 6u);
        EXPECT_EQ(code.find_first_not_of("0123456789"), std::string::npos);
    }

    // 相同密码使用独立盐生成不同记录，密码校验只接受正确的密码。
    TEST(CredentialUtilsTest, SaltsAndVerifiesPasswordRecords)
    {
        const std::string password = "TestPassword2026!";
        const auto first = Utils::HashPassword(password);
        const auto second = Utils::HashPassword(password);
        const std::regex format(R"(pbkdf2-sha256-v1\$[0-9a-f]{32}\$[0-9a-f]{64})");

        ASSERT_TRUE(std::regex_match(first, format));
        EXPECT_TRUE(first != second);
        EXPECT_TRUE(Utils::VerifyPassword(password, first));
        EXPECT_FALSE(Utils::VerifyPassword("DifferentPassword2026!", first));
    }

    // 校验接口拒绝空记录和截断的哈希记录。
    TEST(CredentialUtilsTest, RejectsMalformedPasswordRecords)
    {
        const auto record = Utils::HashPassword("TestPassword2026!");

        EXPECT_FALSE(Utils::VerifyPassword("TestPassword2026!", ""));
        EXPECT_FALSE(Utils::VerifyPassword("TestPassword2026!", record.substr(0, record.size() - 1)));
    }
}
