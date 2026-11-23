#include "../svc_notifyService/gatewayUserTestSupport.hpp"
#include "../svc_notifyService/localSmtpReceiver.hpp"

#include <future>
#include <iterator>
#include <regex>

namespace
{
    using namespace gateway_user_test;

    class GatewayUserNotifyTest : public GatewayUserTestFixture
    {
    protected:
        // 启动全链路依赖服务：通知服务、网关服务、用户服务，入参为三个服务进程对象，由测试用例传入，用于控制进程生命周期
        void StartServices(ServiceProcess &notification, ServiceProcess &gateway, ServiceProcess &user)
        {
            // 1. 创建临时socket并绑定端口，预占用端口避免被其他进程抢占，SOCK_CLOEXEC 保证子进程不继承该文件描述符
            butil::fd_guard notify_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
            butil::fd_guard user_socket(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));

            // 2. 获取socket绑定的随机可用端口
            const int notify_port = ReservePort(notify_socket);
            const int user_port = ReservePort(user_socket);

            // 断言端口有效，确保端口分配成功
            ASSERT_GT(notify_port, 0);
            ASSERT_GT(user_port, 0);

            // 拼接通知服务的完整监听地址（回环地址 + 端口）
            const auto notify_address = std::string(kLoopback) + ":" + std::to_string(notify_port);

            // 3. 释放通知服务的临时socket，让后续启动的服务进程可以绑定该端口
            notify_socket.reset(-1);

            // 4. 启动通知服务子进程，传入完整启动参数
            ASSERT_TRUE(notification.Start({
                NOTIFY_SERVICE_EXECUTABLE,                         // 通知服务可执行文件路径
                "--notify_port=" + std::to_string(notify_port),    // 服务监听端口
                "--notify_address=" + notify_address,              // 服务对外暴露的地址
                "--notify_service=" + NotificationName(),          // 服务名称（用于etcd服务发现）
                std::string("--registry_address=") + kEtcdAddress, // etcd注册中心地址
                "--smtp_url=" + _smtp.Url(),                       // SMTP服务器地址（指向本地模拟服务）
                "--smtp_from=sender@example.test",                 // 邮件发件人地址
                "--smtp_username=", "--smtp_password=",            // SMTP认证账号（本地模拟无需认证）
                "--smtp_login_options="                            // SMTP登录扩展选项
            }));

            // 5. 轮询等待通知服务成功注册到etcd
            // 确保用户服务启动时，能通过etcd的初始节点列表发现通知服务
            ASSERT_TRUE(WaitUntil([&]
                                  {
            // 查询etcd中通知服务目录下的所有注册节点
            const auto registration = _etcd.ls("/" + NotificationName() + "/").get();
            // 校验条件：请求成功、仅1个注册节点、节点地址匹配、持有有效租约（心跳保活）
            return registration.is_ok() && registration.values().size() == 1 &&
                   registration.value(0).as_string() == notify_address &&
                   registration.value(0).lease() != 0; }))
                << "通知服务未注册";

            // 6. 启动网关服务（基类封装的通用启动逻辑，负责请求路由与服务发现）
            ASSERT_NO_FATAL_FAILURE(StartGateway(gateway));

            // 7. 释放用户服务的临时socket，启动用户服务
            user_socket.reset(-1);
            ASSERT_NO_FATAL_FAILURE(StartUser(user, user_port));

            // 8. 等待网关成功发现用户服务，确保HTTP请求可正常路由到用户服务
            ASSERT_NO_FATAL_FAILURE(ExpectDiscovered(user_port));
        }

        // 工具方法：注册测试账号，返回注册使用的邮箱
        std::string RegisterAccount()
        {
            // 构造测试邮箱：统一前缀 + 测试域名
            const auto email = prefix + "@example.test";
            // 通过网关调用注册接口，传入昵称、邮箱、密码参数
            Post("register", {{"nickname", prefix}, {"email", email}, {"password", "GatewayTest!2026"}});
            return email;
        }

        // 工具方法：校验Redis中不存在指定邮箱的验证码缓存，用于验证SMTP拒收场景下，验证码缓存被正确清理
        void ExpectNoCachedCode(const std::string &email)
        {
            // Redis为多测试共用，仅扫描当前测试邮箱的验证码，避免干扰其他用例
            long long cursor = 0;
            do
            {
                std::vector<std::string> code_keys;
                // 游标式扫描Redis，匹配所有 verifyCode: 前缀的key，每次扫描100个
                cursor = redis->scan(cursor, "verifyCode:*", 100, std::back_inserter(code_keys));

                // 遍历扫描到的验证码key，逐一校验邮箱
                for (const auto &key : code_keys)
                {
                    // 读取hash结构中的data字段（存储验证码的JSON数据）
                    const auto cached = redis->hget(key, "data");
                    // 解析JSON，对比邮箱是否为当前测试账号
                    if (cached && chat2Data::Utils::ParseCacheJson(*cached)["email"].asString() == email)
                    {
                        keys.push_back(key); // 加入基类清理列表，测试结束统一删除
                        // 断言失败：SMTP拒收后不应残留验证码记录
                        ADD_FAILURE() << "SMTP 拒收后仍保留了验证码记录";
                    }
                }
            } while (cursor != 0); // 游标归0表示全量扫描完成
        }

        // 成员变量：本地SMTP接收器实例，模拟邮件服务器行为
        notify_test::LocalSmtpReceiver _smtp;
    };

    // 测试用例1：验证HTTP全链路 - 验证码正常下发、可登录、且仅能消费一次
    TEST_F(GatewayUserNotifyTest, DeliversVerificationCodeAndLogsInOverHttp)
    {
        // 1. 声明服务进程对象，启动全链路服务
        ServiceProcess notification;
        ServiceProcess gateway;
        ServiceProcess user;
        ASSERT_NO_FATAL_FAILURE(StartServices(notification, gateway, user));

        // 2. 注册测试账号，获取测试邮箱
        const auto email = RegisterAccount();

        // 3. 异步启动邮件接收任务：后台等待通知服务发送验证码邮件
        // 异步执行避免阻塞HTTP请求，true表示SMTP正常接收并返回成功
        auto delivery = std::async(std::launch::async, [&]
                                   { return _smtp.Receive(true); });

        // 4. 通过网关调用获取验证码接口，触发邮件发送流程
        const auto reply = Post("code", {{"email", email}});
        const auto &result = reply["result"];
        const auto code_id = result["codeId"].asString();

        // 校验：返回的验证码ID非空
        ASSERT_FALSE(code_id.empty());
        // 标记该验证码key，测试结束后自动清理
        Track("verifyCode", code_id);
        // 校验：返回结果仅包含codeId，无验证码明文等敏感信息泄露
        EXPECT_EQ(result.getMemberNames(), Json::Value::Members{"codeId"});

        // 5. 等待异步邮件接收完成，获取完整邮件原文
        const auto message = delivery.get();
        // 校验：邮件收件人与测试邮箱一致
        EXPECT_NE(message.find("To: " + email + "\r\n"), std::string::npos);

        // 6. 解析邮件HTML正文，通过正则提取6位数字验证码
        const auto html = notify_test::DecodePart(message, "Content-Type: text/html");
        const std::regex preview("您的 Chat2Data 登录验证码为 ([0-9]{6})，");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(html, match, preview)) << "邮件预览中缺少六位验证码";
        const auto code = match[1].str();

        // 7. 使用邮箱、验证码ID、验证码明文调用登录接口，完成登录
        const auto login = Post("vcode/login", {{"email", email}, {"codeId", code_id}, {"verifyCode", code}});
        const auto session_id = login["result"]["sessionId"].asString();
        // 校验：登录成功，返回有效会话ID
        ASSERT_FALSE(session_id.empty());
        // 标记会话ID，测试结束后自动清理
        Track("session", session_id);

        // 8. 携带会话ID查询用户信息，校验账号数据正确性
        const auto profile = GetInfo(session_id);
        const auto user_id = profile["userId"].asString();
        ASSERT_FALSE(user_id.empty());
        // 标记用户ID，测试结束后自动清理
        Track("user", user_id);
        // 校验邮箱、昵称与注册时完全一致
        EXPECT_EQ(profile["email"].asString(), email);
        EXPECT_EQ(profile["nickname"].asString(), prefix);

        // 9. 重复使用同一验证码登录，校验返回「验证码不存在/已失效」错误
        // 验证验证码一次性消费的安全特性
        Post("vcode/login", {{"email", email}, {"codeId", code_id}, {"verifyCode", code}},
             ErrorCode::USER_CODE_MISSING);
    }

    // 测试用例2：验证SMTP拒收场景 - 网关透传错误码，用户服务清理Redis缓存
    TEST_F(GatewayUserNotifyTest, ReportsSmtpRejectionAndDeletesCode)
    {
        // 1. 启动全链路服务，注册测试账号
        ServiceProcess notification;
        ServiceProcess gateway;
        ServiceProcess user;
        ASSERT_NO_FATAL_FAILURE(StartServices(notification, gateway, user));
        const auto email = RegisterAccount();

        // 2. 异步启动邮件接收任务，false表示模拟SMTP拒收邮件（返回投递失败）
        auto delivery = std::async(std::launch::async, [&]
                                   { return _smtp.Receive(false); });

        // 3. 调用获取验证码接口，预期返回「通知发送失败」业务错误码
        Post("code", {{"email", email}}, ErrorCode::USER_NOTIFICATION_FAILED);

        // 4. 校验：SMTP确实收到了投递请求（确认是拒收而非未发送）
        EXPECT_NE(delivery.get().find("To: " + email + "\r\n"), std::string::npos);

        // 5. 校验Redis中无对应验证码缓存，验证失败场景下数据一致性
        ExpectNoCachedCode(email);
    }
}
