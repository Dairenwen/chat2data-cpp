#include "Chat2ExcelServer.h"

#include <bite_scaffold/log.h>
#include <gflags/gflags.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

DEFINE_string(host, "0.0.0.0", "HTTP 服务器监听地址");
DEFINE_int32(port, 8080, "HTTP 服务器监听端口，范围为 1～65535");

namespace {

volatile std::sig_atomic_t stopRequested = 0;

/**
 * @brief 记录 Ctrl+C 或终止信号，让主线程执行正常停止流程。
 * @param signalNumber 接收到的信号编号。
 * @note 信号处理函数只修改 sig_atomic_t，不调用日志、锁或网络接口。
 */
void handleSignal(int signalNumber) {
    stopRequested = signalNumber;
}

} // namespace

/**
 * @brief 初始化日志、解析参数并启动 HTTP 服务器，等待停止后退出。
 * @param argc 命令行参数个数。
 * @param argv 支持 --host=地址 和 --port=端口。
 * @return 正常收到停止信号并关闭返回 EXIT_SUCCESS；启动或运行失败返回 EXIT_FAILURE。
 * @note 在入口捕获标准库异常并记录；后台 HTTP 异常由服务器类记录。
 */
int main(int argc, char* argv[]) {
    try {
        // 1. 在任何服务器操作之前初始化 bite_scaffold 的 INFO 级别日志。
        bitelog::log_settings logSettings;
        logSettings.level = spdlog::level::info;
        bitelog::bitelog_init(logSettings);

        gflags::SetUsageMessage("Chat2ExcelServer [--host=0.0.0.0] [--port=8080]");
        gflags::ParseCommandLineFlags(&argc, &argv, true);
        if (std::signal(SIGINT, handleSignal) == SIG_ERR || std::signal(SIGTERM, handleSignal) == SIG_ERR) {
            throw std::runtime_error("无法注册服务器停止信号处理函数");
        }

        // 2. 创建并初始化服务器，地址和端口由命令行参数提供。
        Chat2ExcelServer server(FLAGS_host, FLAGS_port);

        // 3. 启动失败时立即退出，避免主线程在没有服务的情况下空等。
        if (!server.start()) {
            return EXIT_FAILURE;
        }

        // 4. HTTP 请求由后台线程处理，主线程低频检查停止信号和服务状态。
        while (stopRequested == 0 && server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const bool stoppedBySignal = stopRequested != 0;
        if (!stoppedBySignal) {
            ERR("HTTP 服务意外结束，程序将退出");
        }
        server.stop();
        return stoppedBySignal ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        if (bitelog::g_logger) {
            ERR("服务器启动或运行失败：{}", error.what());
        } else {
            // 日志器自身初始化失败时仍需报告原因，此时不能访问空日志器。
            std::cerr << "日志初始化失败：" << error.what() << '\n';
        }
        return EXIT_FAILURE;
    }
}
