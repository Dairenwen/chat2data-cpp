#include "userBusiness.hpp"
#include "userServer.hpp"
#include "userServerBuilder.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "utils.hpp"

#include <chrono>
#include <csignal>
#include <exception>
#include <gflags/gflags.h>
#include <string>
#include <thread>

DEFINE_string(log_level, "info", "BreezeLog 日志级别");
DEFINE_string(mysql_host, "dev-mysql", "MySQL 服务地址");
DEFINE_uint32(mysql_port, 3306, "MySQL 服务端口");
DEFINE_string(mysql_user, "root", "MySQL 用户名");
DEFINE_string(mysql_password, "123456", "MySQL 密码");
DEFINE_string(mysql_database, "chat2data", "MySQL 数据库名");
DEFINE_string(mysql_charset, "utf8mb4", "MySQL 连接字符集");
DEFINE_uint32(mysql_pool_size, 3, "MySQL 连接池大小");
DEFINE_string(redis_host, "dev-redis", "Redis 服务地址");
DEFINE_int32(redis_port, 6379, "Redis 服务端口");
DEFINE_string(redis_user, "default", "Redis 用户名");
DEFINE_string(redis_password, "123456", "Redis 密码");
DEFINE_int32(redis_db, 0, "Redis 数据库编号");
DEFINE_uint32(redis_pool_size, 3, "Redis 连接池大小");
DEFINE_int32(user_port, 9001, "用户 RPC 监听端口");
DEFINE_string(user_service, "userService", "用户服务注册名称");
DEFINE_string(user_address, "", "注册的 RPC 地址，默认使用 dev-env-service 和监听端口");
DEFINE_string(etcd_address, "http://dev-etcd:2379", "服务发现使用的 etcd 地址");
DEFINE_string(registry_address, "", "服务注册使用的 etcd 地址，默认沿用 etcd_address");
DEFINE_string(watch_service, "notifyService", "监听的通知服务注册名称");
DEFINE_int32(notify_rpc_timeout_ms, chat2Data::kDefaultNotifyRpcTimeoutMs, "通知 RPC 超时，单位毫秒");

namespace
{
    volatile std::sig_atomic_t exit_requested = 0;

    // 信号处理只设置退出标志，主线程负责清理注册和 RPC 资源。
    void HandleSignal(int)
    {
        exit_requested = 1;
    }
}

int main(int argc, char **argv)
{
    using chat2Data::Chat2DataException;
    using chat2Data::ErrorCode;
    using chat2Data::Utils;

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    ErrorCode startup_error;
    try
    {
        const auto level = spdlog::level::from_str(FLAGS_log_level);
        if (level == spdlog::level::off && FLAGS_log_level != "off")
            throw Chat2DataException(ErrorCode::USER_SERVER_CONFIG_INVALID);
        BreezeLog::Logger::setLevel(level);

        if (!Utils::IsValidPort(FLAGS_mysql_port) || FLAGS_mysql_pool_size == 0 ||
            !Utils::IsValidPort(FLAGS_redis_port) || FLAGS_redis_pool_size == 0 || FLAGS_redis_db < 0)
            throw Chat2DataException(ErrorCode::USER_SERVER_CONFIG_INVALID);

        chat2data::MysqlConfig mysql;
        mysql.host = FLAGS_mysql_host;
        mysql.port = FLAGS_mysql_port;
        mysql.user = FLAGS_mysql_user;
        mysql.passwd = FLAGS_mysql_password;
        mysql.db = FLAGS_mysql_database;
        mysql.cset = FLAGS_mysql_charset;
        mysql.connection_pool_size = FLAGS_mysql_pool_size;

        chat2data::RedisConfig redis;
        redis.host = FLAGS_redis_host;
        redis.port = FLAGS_redis_port;
        redis.user = FLAGS_redis_user;
        redis.passwd = FLAGS_redis_password;
        redis.db = FLAGS_redis_db;
        redis.connection_pool_size = FLAGS_redis_pool_size;

        // 收到 SIGINT 或 SIGTERM 信号时设置退出标志，主线程检查退出标志后清理资源并退出。
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        chat2data::UserServerBuilder builder;
        builder.SetMysqlConfig(mysql);
        builder.SetRedisConfig(redis);
        builder.SetPort(FLAGS_user_port);
        builder.SetRegistryAddress(FLAGS_registry_address.empty() ? FLAGS_etcd_address : FLAGS_registry_address);
        builder.SetServiceName(FLAGS_user_service);
        builder.SetServiceAddress(FLAGS_user_address.empty() ? "dev-env-service:" + std::to_string(FLAGS_user_port) : FLAGS_user_address);
        builder.SetEtcdAddress(FLAGS_etcd_address);
        builder.SetWatchedService(FLAGS_watch_service);
        builder.SetNotifyRpcTimeout(FLAGS_notify_rpc_timeout_ms);

        // 构建器启动监听和服务发现，并在监听成功后发布注册节点。
        auto server = builder.Build();
        INF("RPC 服务已启动，服务名 {}，监听端口 {}", FLAGS_user_service, FLAGS_user_port);
        // 主线程每隔 100 毫秒检查退出标志，收到信号后退出循环
        while (!exit_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        INF("收到退出信号，正在停止 RPC 服务 {}", FLAGS_user_service);
        return 0;
    }
    catch (const Chat2DataException &error)
    {
        startup_error = error.Code();
    }
    catch (const std::exception &)
    {
        // 底层异常统一使用启动失败错误码，日志只输出项目定义的错误描述。
        startup_error = ErrorCode::USER_SERVER_START_FAILED;
    }
    ERR("用户服务错误码 {}，{}", static_cast<int>(startup_error), chat2Data::error2String(startup_error));
    return 1;
}
