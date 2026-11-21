#pragma once

#include "BreezeODB.hpp"
#include "BreezeRedis.hpp"
#include "errorHandler.hpp"
#include "userData.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <odb/connection.hxx>
#include <odb/transaction.hxx>
#include <sw/redis++/redis.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

DECLARE_string(mysql_host);
DECLARE_uint32(mysql_port);
DECLARE_string(mysql_user);
DECLARE_string(mysql_password);
DECLARE_string(mysql_database);
DECLARE_string(redis_host);
DECLARE_int32(redis_port);
DECLARE_string(redis_user);
DECLARE_string(redis_password);

namespace user_data_test
{
    // 返回 Unix 毫秒，用于构造测试会话和验证码的时间字段。
    inline std::uint64_t NowMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }

    // 三类数据访问测试共享连接与隔离数据的准备和清理流程
    class DataTestFixture : public ::testing::Test
    {
    protected:
        // 每个测试创建独立连接、MySQL 临时表及 Redis 键前缀。
        void SetUp() override
        {
            database = OpenMysql(FLAGS_mysql_database);
            OpenRedis();
            ASSERT_NO_FATAL_FAILURE(CreateUserTables(true));

            // 进程 ID 和递增序号组合，避免不同测试使用相同的 Redis 键。
            static std::atomic<unsigned> sequence{0};
            prefix = "t" + std::to_string(getpid()) + "-" + std::to_string(sequence++);
        }

        static std::shared_ptr<odb::database> OpenMysql(const std::string &database_name)
        {
            // 使用 gflags 指定连接信息，默认地址为开发容器内的服务名。
            BreezeODB::mysql_settings mysql;
            mysql.host = FLAGS_mysql_host;
            mysql.port = FLAGS_mysql_port;
            mysql.user = FLAGS_mysql_user;
            mysql.passwd = FLAGS_mysql_password;
            mysql.db = database_name;
            mysql.cset = "utf8mb4";
            mysql.connection_pool_size = 1;
            return BreezeODB::DBFactory::mysql(mysql);
        }

        void OpenRedis()
        {
            Breezeredis::redis_settings redisSettings;
            redisSettings.host = FLAGS_redis_host;
            redisSettings.port = FLAGS_redis_port;
            redisSettings.user = FLAGS_redis_user;
            redisSettings.passwd = FLAGS_redis_password;
            redis = Breezeredis::RedisFactory::create(redisSettings);
        }

        // 进程内测试使用临时表，跨进程测试在专用数据库中创建普通表。
        void CreateUserTables(bool temporary)
        {
            std::ifstream schemaFile(USER_DATA_SCHEMA);
            ASSERT_TRUE(schemaFile.good());
            std::string sql((std::istreambuf_iterator<char>(schemaFile)), std::istreambuf_iterator<char>());
            std::size_t pos = 0;
            if (temporary)
            {
                while ((pos = sql.find("CREATE TABLE", pos)) != std::string::npos)
                {
                    sql.replace(pos, 12, "CREATE TEMPORARY TABLE");
                    pos += 22;
                }
            }
            odb::transaction transaction(database->begin());
            pos = 0;
            while (true)
            {
                const auto end = sql.find(';', pos);
                if (end == std::string::npos)
                    break;
                const auto statement = sql.substr(pos, end - pos);
                if (statement.find_first_not_of(" \t\r\n") != std::string::npos)
                    transaction.connection().execute(statement);
                pos = end + 1;
            }
            transaction.commit();
        }

        // 只删除当前测试登记过的 Redis 键；临时表随数据库连接关闭而清理。
        void TearDown() override
        {
            if (redis)
                for (const auto &key : keys)
                    redis->del(key);
        }

        // 登记待清理的 Redis 键，并返回业务 ID 供测试使用。
        std::string Track(const std::string &kind, const std::string &id)
        {
            keys.push_back(kind + ":" + id);
            return id;
        }

        std::shared_ptr<odb::database> database;
        std::shared_ptr<sw::redis::Redis> redis;
        std::string prefix;
        std::vector<std::string> keys;
    };

}
