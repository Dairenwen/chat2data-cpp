#include "gatewayServer.hpp"
#include <BreezeLog.hpp>
#include <gflags/gflags.h>
#include <chrono>
#include <csignal>
#include <exception>
#include <thread>
#include <vector>

DEFINE_string(log_level, "info", "BreezeLog 日志级别");
DEFINE_string(gateway_address, "0.0.0.0", "网关监听地址");
DEFINE_int32(gateway_port, 9000, "网关监听端口");
DEFINE_string(etcd_address, "http://dev-etcd:2379", "etcd 地址");
DEFINE_string(user_service, "userService", "用户服务名称");
DEFINE_string(file_service, "fileService", "文件服务名称");
DEFINE_string(db_service, "dbService", "数据库服务名称");
DEFINE_string(ai_service, "aiService", "AI 服务名称");

namespace
{
    volatile std::sig_atomic_t exit_requested = 0; // 退出标志，由信号处理函数设置，主线程检查

    // 信号处理函数只设置标志，停止与日志操作由主线程完成。
    void HandleSignal(int)
    {
        exit_requested = 1;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 4 && std::string(argv[1]) == "--gateway-discovery-worker")
    {
        return GatewayService::RunDiscoveryWorker(argv[2], {argv + 4, argv + argc}, std::stoi(argv[3]));
    }
    gflags::ParseCommandLineFlags(&argc, &argv, true);           // 解析命令行参数
    const auto level = spdlog::level::from_str(FLAGS_log_level); // 设置日志级别
    if (level == spdlog::level::off && FLAGS_log_level != "off")
    {
        ERR("无效的日志级别: {}", FLAGS_log_level);
        return 1;
    }
    BreezeLog::Logger::setLevel(level);
    if (FLAGS_gateway_port < 1 || FLAGS_gateway_port > 65535)
    {
        ERR("无效的网关端口: {}", FLAGS_gateway_port);
        return 1;
    }
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    try
    {
        GatewayService::GatewayServerBuilder builder(
            FLAGS_gateway_address, FLAGS_gateway_port, FLAGS_etcd_address,
            {FLAGS_user_service, FLAGS_file_service, FLAGS_db_service, FLAGS_ai_service});
        auto server = builder.Build();

        if (!server->start()) // 启动失败，直接退出
            return 1;

        while (!exit_requested && server->isRunning()) // 每隔 100 毫秒检查退出标志和服务器状态，避免阻塞信号处理
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const bool interrupted = exit_requested != 0;
        INF(interrupted ? "收到退出信号" : "网关监听已结束");
        server->stop();
        return interrupted ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        ERR("网关启动失败: {}", error.what());
        return 1;
    }
}
