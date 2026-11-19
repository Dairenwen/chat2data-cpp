#pragma once

#include "userEntity.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sw::redis
{
    class Redis;
}

namespace odb
{
    class database;
    class transaction;
}

namespace chat2Data
{
    // 用户缓存只保存业务字段，不包含数据库自增主键。
    struct CachedUser
    {
        std::string userId;       // 用户业务 ID。
        std::string nickname;     // 用户昵称。
        std::string passwordHash; // 已加盐的密码哈希，不是明文密码。
        std::string email;        // 用户邮箱。
        int status = 0;           // 用户状态。
    };

    // 用户信息的数据访问入口；数据库和 Redis 客户端由调用方创建并共享。
    // 业务层编排缓存查询、回填和失效。数据库、Redis 和缓存数据格式错误均向调用方报告。
    class UserData
    {
    public:
        /**
         * @brief 使用已创建的客户端。
         * @param database MySQL 主库。
         * @param redis Redis 客户端。
         */
        UserData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis);

        /**
         * @brief 将用户保存到 MySQL。
         * @param user 用户对象；数据库生成的主键写回此对象。
         * @note 提交后由业务层负责使缓存失效。
         */
        void Save(UserEntity &user);
        /**
         * @brief 在 MySQL 中按用户 ID 查找。
         * @param userId 用户 ID。
         * @param fillCache 为 true 时，在用户行锁内回填缓存。
         * @return 用户缓存信息；不存在时为空。
         */
        std::optional<CachedUser> FindById(const std::string &userId, bool fillCache = false);
        /**
         * @brief 在 MySQL 中按昵称查找。
         * @param nickname 用户昵称。
         * @return 用户缓存信息；不存在时为空。
         */
        std::optional<CachedUser> FindByNickname(const std::string &nickname);
        /**
         * @brief 在 MySQL 中按邮箱查找。
         * @param email 用户邮箱。
         * @return 用户缓存信息；不存在时为空。
         */
        std::optional<CachedUser> FindByEmail(const std::string &email);
        /**
         * @brief 查询昵称是否存在。
         * @param nickname 用户昵称。
         * @return 存在时为 true。
         */
        bool NicknameExists(const std::string &nickname);
        /**
         * @brief 查询邮箱是否存在。
         * @param email 用户邮箱。
         * @return 存在时为 true。
         */
        bool EmailExists(const std::string &email);
        /**
         * @brief 锁定用户记录，按有效会话更新状态。
         * @param userId 用户 ID。
         * @param transaction 退出登录时传入会话事务，由调用方提交。
         * @return 用户存在时为 true；不存在时为 false。
         * @note 用户缓存由业务层失效。
         */
        bool UpdateStatus(const std::string &userId, odb::transaction *transaction = nullptr);
        /**
         * @brief 保存用户缓存并设置一小时 TTL。
         * @param user 用户缓存信息。
         */
        void SaveCache(const CachedUser &user);
        /**
         * @brief 读取用户缓存。
         * @param userId 用户 ID。
         * @return 缓存信息；未命中时为空。
         */
        std::optional<CachedUser> FindCache(const std::string &userId);
        /**
         * @brief 删除用户缓存。
         * @param userId 用户 ID。
         */
        void DeleteCache(const std::string &userId);

    private:
        /**
         * @brief 将数据库用户转换为缓存信息。
         * @param user 数据库查询结果。
         * @return 缓存信息或空值。
         */
        std::optional<CachedUser> FromDatabase(const std::optional<UserEntity> &user);
        std::shared_ptr<odb::database> _database; // MySQL 连接
        std::shared_ptr<sw::redis::Redis> _redis; // Redis 连接
    };

}
