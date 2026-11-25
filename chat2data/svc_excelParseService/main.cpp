#include "excelParserServer.hpp"

#include "BreezeLog.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"

#include <chrono>
#include <csignal>
#include <gflags/gflags.h>
#include <thread>
#include <utility>

DEFINE_int32(excel_port, 9003, "Excel 解析 RPC 监听端口");
DEFINE_string(excel_service, "excelService", "Excel 解析服务注册名称");
DEFINE_string(excel_address, "", "注册的 RPC 地址，默认使用 dev-env-service 和监听端口");
DEFINE_string(registry_address, "http://dev-etcd:2379", "etcd 注册端点");
DEFINE_string(fdfs_trackers, "dev-tracker:22122", "FastDFS Tracker 地址列表，使用逗号分隔");
DEFINE_int32(fdfs_connect_timeout, 30, "FastDFS 连接超时，单位秒");
DEFINE_int32(fdfs_network_timeout, 30, "FastDFS 网络读写超时，单位秒");
DEFINE_string(log_level, "info", "BreezeLog 日志级别");

namespace
{
    volatile std::sig_atomic_t exit_requested = 0;

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
    auto startup_error = ErrorCode::EXCEL_SERVER_START_FAILED;
    try
    {
        const auto level = spdlog::level::from_str(FLAGS_log_level);
        if (level == spdlog::level::off && FLAGS_log_level != "off")
            throw Chat2DataException(ErrorCode::EXCEL_SERVER_CONFIG_INVALID);
        BreezeLog::Logger::setLevel(level);

        chat2data::ExcelParserServerConfig config{};
        config.port = FLAGS_excel_port;
        config.service_name = FLAGS_excel_service;
        config.registry_address = FLAGS_registry_address;
        // etcd 发布调用方可访问的地址，默认使用开发容器名与监听端口。
        config.service_address = FLAGS_excel_address.empty()
                                     ? "dev-env-service:" + std::to_string(config.port)
                                     : FLAGS_excel_address;
        BreezeUtil::STR::split(FLAGS_fdfs_trackers, ",", config.trackers);
        config.connect_timeout = FLAGS_fdfs_connect_timeout;
        config.network_timeout = FLAGS_fdfs_network_timeout;

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        auto server = chat2data::ExcelParserServerBuilder(std::move(config)).Build();
        INF("RPC 服务已启动，服务名 {}，监听端口 {}", FLAGS_excel_service, FLAGS_excel_port);
        // 信号处理设置退出标志，主线程负责停机和资源回收。
        while (!exit_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        INF("收到退出信号，正在停止 RPC 服务 {}", FLAGS_excel_service);
        return 0;
    }
    catch (const Chat2DataException &error)
    {
        startup_error = error.Code();
    }
    catch (...)
    {
        // 底层启动异常归入项目启动错误码。
    }
    ERR("Excel 解析服务错误码 {}，{}", static_cast<int>(startup_error), chat2Data::error2String(startup_error));
    return 1;
}
