#pragma once

#include "sessionData.hpp"

namespace chat2Data
{
    // 编排登录会话的持久化、旁路缓存和延迟双删。
    class SessionManager
    {
    public:
        explicit SessionManager(std::shared_ptr<SessionData> sessions);

        /**
         * @brief 创建三天有效的登录会话。
         * @param userId 会话所属用户 ID。
         * @return 数据库写入和缓存失效均成功后的会话 ID。
         */
        std::string Create(const std::string &userId);
        /**
         * @brief 先查 Redis，未命中时在 MySQL 会话行锁内查询并回填。
         * @param sessionId 登录会话 ID。
         * @return 会话状态及有效会话信息。依赖故障通过异常报告。
         */
        SessionResult Find(const std::string &sessionId);
        /**
         * @brief 根据有效会话获取所属用户 ID。
         * @param sessionId 登录会话 ID。
         * @return 有效会话的用户 ID。会话缺失或过期时为空。
         */
        std::optional<std::string> GetUserId(const std::string &sessionId);
        /**
         * @brief 查询会话归属，包含已过期记录，供在线状态校正使用。
         * @param sessionId 登录会话 ID。
         * @return 会话记录中的用户 ID。记录缺失时为空。
         */
        std::optional<std::string> FindOwner(const std::string &sessionId);
        /**
         * @brief 按延迟双删撤销会话。
         * @param sessionId 待撤销的登录会话 ID。
         * @param updateStatus 可选回调，在第二次删除缓存后使用同一事务校正用户状态。
         * @note 回调同步执行，事务引用的使用范围限于回调。提交前发生异常时回滚 MySQL 变更。
         */
        void Delete(const std::string &sessionId,
                    const std::function<void(odb::transaction &)> &updateStatus = {});
        /**
         * @brief 直接删除尚未交付的会话记录，用于补偿登录失败。
         * @param sessionId 尚未返回给客户端的会话 ID。
         */
        void DiscardUnissued(const std::string &sessionId);

    private:
        void InvalidateCache(const std::string &sessionId);

        std::shared_ptr<SessionData> _sessions;
    };
}
