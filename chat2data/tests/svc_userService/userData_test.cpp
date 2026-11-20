#include "BreezeRPC.hpp"
#include "notifyService.pb.h"
#include "sessionEntity-odb.hxx"
#include "testSupport.hpp"
#include "userBusiness.hpp"
#include "utils.hpp"

#include <brpc/closure_guard.h>

// 开发容器中的服务名和默认端口；凭证由运行参数提供。
DEFINE_string(mysql_host, "dev-mysql", "MySQL 服务地址");
DEFINE_uint32(mysql_port, 3306, "MySQL 服务端口");
DEFINE_string(mysql_user, "root", "MySQL 用户名");
DEFINE_string(mysql_password, "123456", "MySQL 密码");
DEFINE_string(mysql_database, "chat2data", "MySQL 数据库名");
DEFINE_string(redis_host, "dev-redis", "Redis 服务地址");
DEFINE_int32(redis_port, 6379, "Redis 服务端口");
DEFINE_string(redis_user, "default", "Redis 用户名");
DEFINE_string(redis_password, "123456", "Redis 密码");

namespace
{
    using namespace user_data_test;

    class UserDataTest : public DataTestFixture
    {
    };

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

    class UserBusinessTest : public DataTestFixture
    {
    protected:
        void SetUp() override
        {
            DataTestFixture::SetUp();
            if (HasFatalFailure())
                return;
            users = std::make_shared<chat2Data::UserData>(database, redis);
            sessions = std::make_shared<chat2Data::SessionData>(database, redis);
            codes = std::make_shared<chat2Data::VerifyCodeData>(redis);
            channels = std::make_shared<SvcChannels>();
            business = std::make_unique<chat2Data::UserBusiness>(
                std::make_shared<chat2Data::SessionManager>(sessions), users, codes, channels);
            email = prefix + "@example.com";
        }

        std::string RegisterUser()
        {
            business->Register(prefix, email, password);
            return Track("user", users->FindByEmail(email).value().userId);
        }

        std::shared_ptr<chat2Data::UserData> users;
        std::shared_ptr<chat2Data::SessionData> sessions;
        std::shared_ptr<chat2Data::VerifyCodeData> codes;
        std::shared_ptr<SvcChannels> channels;
        std::unique_ptr<chat2Data::UserBusiness> business;
        std::string email;
        const std::string password = "Example731!";
    };

    // 两台设备分别登录，退出一台后另一台仍可使用；访问到期的最后一个会话时校正状态。
    TEST_F(UserBusinessTest, TracksMultipleSessionsAndReconcilesExpiryOnAccess)
    {
        using namespace chat2Data;
        EXPECT_TRUE(business->ValidNickname(prefix));
        EXPECT_TRUE(business->ValidEmail(email));
        EXPECT_THROW(business->Register(prefix, email, "aaaaaaaa"), Chat2DataException);
        const auto userId = RegisterUser();
        const auto user = users->FindById(userId);
        ASSERT_TRUE(user);
        EXPECT_EQ(user->status, 0);
        EXPECT_NE(user->passwordHash, password);
        EXPECT_TRUE(sessions->FindValidByUserId(userId).empty());
        EXPECT_FALSE(business->ValidNickname(prefix));
        EXPECT_FALSE(business->ValidEmail(email));
        EXPECT_THROW(business->PasswdLogin(prefix, "Wrong731!"), Chat2DataException);

        const auto beforeLogin = NowMs();
        const auto desktop = Track("session", business->PasswdLogin(prefix, password));
        const auto phone = Track("session", business->PasswdLogin(email, password));
        EXPECT_NE(desktop, phone);
        ASSERT_EQ(sessions->FindValidByUserId(userId).size(), 2u);
        EXPECT_EQ(users->FindById(userId)->status, 1);
        EXPECT_FALSE(sessions->FindCache(desktop).session);
        const auto expiresAt = sessions->Find(desktop).session.value().expiresAt;
        EXPECT_GE(expiresAt, beforeLogin + kSessionLifetimeMs);
        EXPECT_LE(expiresAt, NowMs() + kSessionLifetimeMs);
        business->SessionLogin(desktop);
        EXPECT_EQ(sessions->FindCache(desktop).session.value().expiresAt, expiresAt);
        EXPECT_EQ(business->GetUserInfo(desktop).email, email);
        ASSERT_TRUE(users->FindCache(userId));

        business->Logout(desktop);
        EXPECT_FALSE(business->IsSessionValid(desktop));
        EXPECT_EQ(business->IsSessionValid(phone), userId);
        EXPECT_EQ(users->FindById(userId)->status, 1);
        business->Logout(phone);
        EXPECT_EQ(users->FindById(userId)->status, 0);

        SessionEntity expired;
        expired.sessionId = prefix + "-expired";
        expired.userId = userId;
        expired.expiresAt = NowMs() + 30000;
        sessions->Save(expired);
        ASSERT_TRUE(users->UpdateStatus(userId));
        ASSERT_EQ(users->FindById(userId)->status, 1);
        users->SaveCache(users->FindById(userId).value());
        expired.expiresAt = NowMs() - 1;
        {
            odb::transaction transaction(database->begin());
            database->update(expired);
            transaction.commit();
        }
        EXPECT_TRUE(sessions->FindValidByUserId(userId).empty());
        EXPECT_FALSE(business->IsSessionValid(expired.sessionId));
        EXPECT_EQ(users->FindById(userId)->status, 0);
        EXPECT_FALSE(users->FindCache(userId));
    }

    // 会话登录分别报告缺失和过期，内部会话校验均返回无效。
    TEST_F(UserBusinessTest, DistinguishesMissingAndExpiredSessions)
    {
        using namespace chat2Data;
        const auto user_id = RegisterUser();
        SessionEntity expired;
        expired.sessionId = Track("session", prefix + "-expired");
        expired.userId = user_id;
        expired.expiresAt = NowMs() - 1;
        sessions->Save(expired);

        const auto expect_rejection = [&](const std::string &session_id, ErrorCode expected)
        {
            try
            {
                business->SessionLogin(session_id);
                ADD_FAILURE() << "无效会话应返回登录错误。";
            }
            catch (const Chat2DataException &error)
            {
                EXPECT_EQ(error.Code(), expected);
            }
            EXPECT_FALSE(business->IsSessionValid(session_id));
        };

        expect_rejection(prefix + "-missing", ErrorCode::USER_SESSION_NOT_FOUND);
        expect_rejection(expired.sessionId, ErrorCode::USER_SESSION_EXPIRED);
        EXPECT_FALSE(sessions->FindCache(expired.sessionId).session);
    }

    // 两个请求同时注册相同邮箱，MySQL 唯一约束只允许一条记录写入。
    TEST_F(UserBusinessTest, EnforcesEmailUniquenessAcrossConcurrentRegistrations)
    {
        using namespace chat2Data;
        const auto attempt = [&](const std::string &nickname)
        {
            try
            {
                business->Register(nickname, email, password);
                return ErrorCode::SUCCESS;
            }
            catch (const Chat2DataException &error)
            {
                return error.Code();
            }
        };
        auto first = std::async(std::launch::async, attempt, prefix + "-one");
        auto second = std::async(std::launch::async, attempt, prefix + "-two");
        const auto firstResult = first.get();
        const auto secondResult = second.get();
        EXPECT_NE(firstResult == ErrorCode::SUCCESS, secondResult == ErrorCode::SUCCESS);
        const auto rejected = firstResult == ErrorCode::SUCCESS ? secondResult : firstResult;
        EXPECT_TRUE(rejected == ErrorCode::USER_REGISTER_CONFLICT || rejected == ErrorCode::USER_EMAIL_TAKEN);
        const auto user = users->FindByEmail(email);
        ASSERT_TRUE(user);
        Track("user", user->userId);
    }

    // 退出过程中用户缓存失效失败时，会话和在线状态一起回滚。
    TEST_F(UserBusinessTest, RollsBackLogoutWhenUserCacheIsUnavailable)
    {
        using namespace chat2Data;
        UserEntity user;
        user.userId = Track("user", prefix);
        user.nickname = prefix;
        user.email = email;
        user.password = "test-hash";
        users->Save(user);

        const auto manager = std::make_shared<SessionManager>(sessions);
        const auto sessionId = Track("session", manager->Create(user.userId));
        ASSERT_TRUE(users->UpdateStatus(user.userId));

        sw::redis::ConnectionOptions options;
        options.host = "127.0.0.1";
        options.port = 1;
        const auto unavailable = std::make_shared<sw::redis::Redis>(options);
        UserBusiness failing(manager, std::make_shared<UserData>(database, unavailable), codes, channels);
        EXPECT_THROW(failing.Logout(sessionId), sw::redis::Error);
        EXPECT_EQ(users->FindById(user.userId)->status, 1);
        EXPECT_EQ(manager->GetUserId(sessionId), user.userId);

        business->Logout(sessionId);
        EXPECT_FALSE(manager->GetUserId(sessionId));
        EXPECT_EQ(users->FindById(user.userId)->status, 0);
    }

    // 本地 RPC 服务记录验证码，测试期间只使用回环接口。
    class TestNotification : public chat2Data::NotifyService
    {
    public:
        void SendVerifyCode(google::protobuf::RpcController *, const chat2Data::SendVerifyCodeRequest *request,
                            chat2Data::SendVerifyCodeResponse *response, google::protobuf::Closure *done) override
        {
            brpc::ClosureGuard guard(done);
            const std::lock_guard<std::mutex> lock(mutex);
            code = request->verify_code();
            email = request->email();
            response->set_request_id(request->request_id());
            response->set_error_code(request->request_id() == "failed" ? 1 : 0);
        }

        std::mutex mutex;
        std::string code;
        std::string email;
    };

    // 验证码通过通知 RPC 投递，并在查询用户前完成一次性消费。
    TEST_F(UserBusinessTest, SendsAndConsumesVerificationCode)
    {
        using namespace chat2Data;
        const auto userId = RegisterUser();
        EXPECT_THROW(business->GetCode(prefix, email), Chat2DataException);

        TestNotification notification;
        brpc::Server server;
        ASSERT_EQ(server.AddService(&notification, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(server.Start("127.0.0.1:0", nullptr), 0);
        channels->setWatch("notifyService");
        channels->addNode("notifyService", "127.0.0.1:" + std::to_string(server.listen_address().port));
        EXPECT_THROW(business->GetCode(prefix, "unknown-" + email), Chat2DataException);
        EXPECT_THROW(business->GetCode("failed", email), Chat2DataException);

        const auto codeId = Track("verifyCode", business->GetCode(prefix, email));
        std::string code;
        {
            const std::lock_guard<std::mutex> lock(notification.mutex);
            code = notification.code;
            EXPECT_EQ(notification.email, email);
        }
        ASSERT_EQ(code.size(), 6u);
        EXPECT_EQ(code.find_first_not_of("0123456789"), std::string::npos);
        EXPECT_THROW(business->VcodeLogin(email, codeId, "invalid"), Chat2DataException);
        const auto session = Track("session", business->VcodeLogin(email, codeId, code));
        EXPECT_EQ(business->IsSessionValid(session), userId);
        EXPECT_FALSE(codes->Find(codeId));

        const auto unknownEmail = "unknown-" + email;
        const auto unknownId = Track("verifyCode", prefix);
        ASSERT_TRUE(codes->Save({unknownId, "000123", unknownEmail, NowMs()}));
        EXPECT_THROW(business->VcodeLogin(unknownEmail, unknownId, "000123"), Chat2DataException);
        EXPECT_FALSE(codes->Find(unknownId));
    }
}

int main(int argc, char **argv)
{
    // 先解析 Google Test 参数，再将连接参数交给 gflags。
    ::testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}
