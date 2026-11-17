#include "userData.hpp"

#include "errorHandler.hpp"
#include "utils.hpp"
#include "userEntity-odb.hxx"
#include <json/json.h>
#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <sw/redis++/redis.h>
#include <algorithm>
#include <memory>
#include <utility>

namespace chat2Data
{
    namespace
    {
        // 用户缓存有效期，单位为毫秒。
        constexpr std::uint64_t kUserTtlMs = 3600ULL * 1000;

        // 为用户缓存拼接固定前缀。
        std::string UserKey(const std::string &id) { return "user:" + id; }

        // 数据库实体转为缓存业务字段，不携带自增主键。
        CachedUser ToCache(const UserEntity &user)
        {
            return {user.userId, user.nickname, user.password, user.email, user.status};
        }

        // ODB 根据实体类型和查询条件生成 SELECT；事务提交后返回单条记录或空值。使用模版可以不用为每个实体类型写重复的查询函数。
        template <typename Entity>
        std::optional<Entity> QueryOne(odb::database &database, const odb::query<Entity> &query)
        {
            odb::transaction transaction(database.begin());                    // 用database.begin()开启事务
            std::unique_ptr<Entity> result(database.query_one<Entity>(query)); // 执行查询并获取结果
            auto value = result ? std::optional<Entity>(*result) : std::nullopt;
            transaction.commit(); // 提交事务
            return value;
        }

    }

    // 保存调用方传入的数据库和 Redis 客户端。
    UserData::UserData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis)
        : _database(std::move(database)), _redis(std::move(redis)) {}

    // 写入 MySQL；缓存失效由业务层在提交成功后执行。
    void UserData::Save(UserEntity &user)
    {
        odb::transaction transaction(_database->begin());
        // ODB 生成 INSERT INTO tbl_user (...) VALUES (...)，并将自增主键写回 user.id。
        _database->persist(user);
        transaction.commit();
    }

    // 将数据库查询结果转换为业务字段。
    std::optional<CachedUser> UserData::FromDatabase(const std::optional<UserEntity> &user)
    {
        if (!user)
            return std::nullopt;
        return ToCache(*user);
    }

    // 按用户 ID 查询 MySQL。
    std::optional<CachedUser> UserData::FindById(const std::string &userId)
    {
        // 对应查询条件：SELECT ... FROM tbl_user WHERE userId = ?。
        return FromDatabase(QueryOne<UserEntity>(*_database, odb::query<UserEntity>::userId == userId));
    }

    // 昵称没有独立缓存索引，以数据库查询结果为准。
    std::optional<CachedUser> UserData::FindByNickname(const std::string &nickname)
    {
        // 对应查询条件：SELECT ... FROM tbl_user WHERE nickname = ?。
        return FromDatabase(QueryOne<UserEntity>(*_database, odb::query<UserEntity>::nickname == nickname));
    }

    // 邮箱没有独立缓存索引，以数据库查询结果为准。
    std::optional<CachedUser> UserData::FindByEmail(const std::string &email)
    {
        // 对应查询条件：SELECT ... FROM tbl_user WHERE email = ?。
        return FromDatabase(QueryOne<UserEntity>(*_database, odb::query<UserEntity>::email == email));
    }

    // 昵称存在性从数据库查询，最终唯一性仍由数据库约束保证。
    bool UserData::NicknameExists(const std::string &nickname)
    {
        // SELECT ... FROM tbl_user WHERE nickname = ?；有记录即表示昵称存在。
        return QueryOne<UserEntity>(*_database, odb::query<UserEntity>::nickname == nickname).has_value();
    }

    // 邮箱存在性从数据库查询，最终唯一性仍由数据库约束保证。
    bool UserData::EmailExists(const std::string &email)
    {
        // SELECT ... FROM tbl_user WHERE email = ?；有记录即表示邮箱存在。
        return QueryOne<UserEntity>(*_database, odb::query<UserEntity>::email == email).has_value();
    }

    // 将用户业务字段序列化到 Redis，并设置一小时 TTL。
    void UserData::SaveCache(const CachedUser &user)
    {
        Json::Value value(Json::objectValue);
        value["userId"] = user.userId;
        value["nickname"] = user.nickname;
        value["passwordHash"] = user.passwordHash;
        value["email"] = user.email;
        value["status"] = user.status;
        Utils::WriteCache(*_redis, UserKey(user.userId), value, kUserTtlMs);
    }

    // 读取并检查缓存字段，避免把格式错误或不同用户的数据当作有效缓存。
    std::optional<CachedUser> UserData::FindCache(const std::string &userId)
    {
        const auto text = Utils::ReadCache(*_redis, UserKey(userId));
        if (!text)
            return std::nullopt;
        const auto value = Utils::ParseCacheJson(*text);
        if (!value["userId"].isString() || value["userId"].asString() != userId ||
            !value["nickname"].isString() || !value["passwordHash"].isString() ||
            !value["email"].isString() || !value["status"].isInt())
            throw Chat2DataException(ErrorCode::USER_CACHE_INVALID);
        return CachedUser{value["userId"].asString(), value["nickname"].asString(), value["passwordHash"].asString(), value["email"].asString(), value["status"].asInt()};
    }

    // 执行 DEL user:{userId}，删除该用户的缓存键。
    void UserData::DeleteCache(const std::string &userId) { _redis->del(UserKey(userId)); }

}
