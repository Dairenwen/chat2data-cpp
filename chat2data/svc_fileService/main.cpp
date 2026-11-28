#include "fileServer.hpp"
#include "fileServerBuilder.hpp"

#include "BreezeLog.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"

#include <chrono>
#include <csignal>
#include <exception>
#include <gflags/gflags.h>
#include <thread>
#include <utility>

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
DEFINE_int32(file_port, 9004, "文件 RPC 监听端口");
DEFINE_string(file_service, "fileService", "文件服务注册名称");
DEFINE_string(file_address, "", "注册的 RPC 地址，默认使用 dev-env-service 和监听端口");
DEFINE_string(excel_service, "excelService", "Excel 解析服务发现名称");
DEFINE_string(db_service, "dbService", "存储服务发现名称");
DEFINE_string(ai_service, "aiService", "AI 服务发现名称");
DEFINE_string(registry_address, "http://dev-etcd:2379", "etcd 注册端点");
DEFINE_string(fdfs_trackers, "dev-tracker:22122", "FastDFS Tracker 地址列表，使用逗号分隔");
DEFINE_int32(fdfs_connect_timeout, 30, "FastDFS 连接超时，单位秒");
DEFINE_int32(fdfs_network_timeout, 30, "FastDFS 网络读写超时，单位秒");

namespace
{
    volatile std::sig_atomic_t exit_requested = 0;

    // 信号处理只设置退出标志，主线程负责等待 RPC 和释放依赖。
    void HandleSignal(int)
    {
        exit_requested = 1;
    }
}

int main(int argc, char **argv)
{
    using chat2Data::Chat2DataException;
    using chat2Data::ErrorCode;

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    auto startup_error = ErrorCode::FILE_SERVER_START_FAILED;
    try
    {
        const auto level = spdlog::level::from_str(FLAGS_log_level);
        if (level == spdlog::level::off && FLAGS_log_level != "off")
            throw Chat2DataException(ErrorCode::FILE_SERVER_CONFIG_INVALID);
        BreezeLog::Logger::setLevel(level);

        chat2data::FileServerConfig config{};
        config.port = FLAGS_file_port;
        config.service_name = FLAGS_file_service;
        config.registry_address = FLAGS_registry_address;
        // etcd 发布调用方可访问的地址，默认使用开发容器名与监听端口。
        config.service_address = FLAGS_file_address.empty()
                                     ? "dev-env-service:" + std::to_string(config.port)
                                     : FLAGS_file_address;
        BreezeUtil::STR::split(FLAGS_fdfs_trackers, ",", config.fdfs.trackers);
        config.fdfs.connect_timeout = FLAGS_fdfs_connect_timeout;
        config.fdfs.network_timeout = FLAGS_fdfs_network_timeout;

        config.excel_service = FLAGS_excel_service;
        config.db_service = FLAGS_db_service;
        config.ai_service = FLAGS_ai_service;
        config.mysql.host = FLAGS_mysql_host;
        config.mysql.port = FLAGS_mysql_port;
        config.mysql.user = FLAGS_mysql_user;
        config.mysql.passwd = FLAGS_mysql_password;
        config.mysql.db = FLAGS_mysql_database;
        config.mysql.cset = FLAGS_mysql_charset;
        config.mysql.connection_pool_size = FLAGS_mysql_pool_size;
        config.redis.host = FLAGS_redis_host;
        config.redis.port = FLAGS_redis_port;
        config.redis.user = FLAGS_redis_user;
        config.redis.passwd = FLAGS_redis_password;
        config.redis.db = FLAGS_redis_db;
        config.redis.connection_pool_size = FLAGS_redis_pool_size;

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        auto server = chat2data::FileServerBuilder(std::move(config)).Build();
        INF("RPC 服务已启动，服务名 {}，监听端口 {}", FLAGS_file_service, FLAGS_file_port);
        // 信号处理设置退出标志，主线程负责停机和资源回收。
        while (!exit_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        INF("收到退出信号，正在停止 RPC 服务 {}", FLAGS_file_service);
        return 0;
    }
    catch (const Chat2DataException &error)
    {
        startup_error = error.Code();
    }
    catch (const std::exception &)
    {
        // 底层启动异常归入项目启动错误码。
    }
    ERR("文件服务错误码 {}，{}", static_cast<int>(startup_error), chat2Data::error2String(startup_error));
    return 1;
}
