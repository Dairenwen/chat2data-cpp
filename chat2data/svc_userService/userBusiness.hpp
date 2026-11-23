#pragma once

#include "sessionManager.hpp"
#include "userData.hpp"
#include "verifyCodeData.hpp"

#include <string>

class SvcChannels;

namespace chat2Data
{
    // 用户服务的默认等待通知 RPC 响应的时间，覆盖邮件任务预算
    inline constexpr int kDefaultNotifyRpcTimeoutMs = 30000;

    // 用户信息接口对外返回的字段。
    struct PublicUserInfo
    {
        std::string userId;
        std::string nickname;
        std::string email;
    };

    // 编排用户注册、登录、退出和会话校验，并按有效会话维护在线状态。
    class UserBusiness
    {
    public:
        /**
         * @brief 使用调用方提供的业务依赖。
         * @param sessions 登录会话管理器。
         * @param users 用户数据访问对象。
         * @param codes 验证码数据访问对象。
         * @param channels 用于选择通知服务节点的 RPC 通道集合。
         * @param notification_service 通知节点在通道集合中的服务名称。
         * @param notify_rpc_timeout_ms 等待通知 RPC 响应的时间，覆盖邮件任务预算。
         */
        UserBusiness(std::shared_ptr<SessionManager> sessions, std::shared_ptr<UserData> users,
                     std::shared_ptr<VerifyCodeData> codes, std::shared_ptr<SvcChannels> channels,
                     std::string notification_service = "notifyService",
                     int notify_rpc_timeout_ms = kDefaultNotifyRpcTimeoutMs);

        /**
         * @brief 校验昵称并查询当前可用性。
         * @param nickname 待校验的昵称。
         * @return 当前未被占用时为 true。注册结果以数据库唯一约束为准。
         */
        bool ValidNickname(const std::string &nickname);
        /**
         * @brief 校验邮箱并查询当前可用性。
         * @param email 待校验的邮箱。
         * @return 当前未被占用时为 true。注册结果以数据库唯一约束为准。
         */
        bool ValidEmail(const std::string &email);
        /**
         * @brief 注册初始状态为离线的用户，保存带盐的密码哈希。
         * @param nickname 用户昵称。
         * @param email 注册邮箱。
         * @param password 用户提交的密码。
         * @note 用户写入提交后使缓存失效。缓存失效失败时报告异常，已提交的用户记录仍保留。
         */
        void Register(const std::string &nickname, const std::string &email, const std::string &password);
        /**
         * @brief 验证密码，创建登录会话并校正在线状态。
         * @param account 用户昵称或注册邮箱。优先按昵称查询。
         * @param password 用户提交的密码。
         * @return 登录成功后签发的会话 ID。
         */
        std::string PasswdLogin(const std::string &account, const std::string &password);
        /**
         * @brief 向注册邮箱发送五分钟内有效的验证码。
         * @param requestId 用于关联通知服务请求与响应的 ID。
         * @param email 与注册记录完全一致的邮箱地址。
         * @return 通知服务确认发送成功后的验证码 ID。
         */
        std::string GetCode(const std::string &requestId, const std::string &email);
        /**
         * @brief 原子校验并消费验证码，再创建登录会话。
         * @param email 注册邮箱。
         * @param codeId 获取验证码时返回的 ID。
         * @param code 六位数字验证码字符串。
         * @return 登录成功后签发的会话 ID。
         * @note 消费成功后遇到后续登录失败时，客户端需要重新获取验证码。
         */
        std::string VcodeLogin(const std::string &email, const std::string &codeId, const std::string &code);
        /**
         * @brief 使用有效会话登录，保留原有效期并校正在线状态。
         * @param sessionId 已有的登录会话 ID。
         */
        void SessionLogin(const std::string &sessionId);
        /**
         * @brief 撤销指定会话，在同一 MySQL 事务内按剩余有效会话校正在线状态。
         * @param sessionId 当前设备的登录会话 ID。
         */
        void Logout(const std::string &sessionId);
        /**
         * @brief 校验会话后，通过旁路缓存查询当前用户的公开信息。
         * @param sessionId 登录会话 ID。
         * @return 用户 ID、昵称和邮箱。
         */
        PublicUserInfo GetUserInfo(const std::string &sessionId);
        /**
         * @brief 校验指定会话，发现会话自然到期时校正用户状态。
         * @param sessionId 待校验的登录会话 ID。
         * @return 有效会话的用户 ID。会话缺失或过期时为空，依赖故障通过异常报告。
         */
        std::optional<std::string> IsSessionValid(const std::string &sessionId);

    private:
        SessionResult CheckSession(const std::string &sessionId);
        std::string RequireSessionOwner(const std::string &sessionId);
        std::string CreateLoginSession(const std::string &userId);
        void RefreshOnlineStatus(const std::string &userId);

        std::shared_ptr<SessionManager> _sessions;
        std::shared_ptr<UserData> _users;
        std::shared_ptr<VerifyCodeData> _codes;
        std::shared_ptr<SvcChannels> _channels;
        std::string _notification_service;
        int _notify_rpc_timeout_ms;
    };
}
