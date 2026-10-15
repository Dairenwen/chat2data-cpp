#include "Chat2ExcelServer.h"

#include <bite_scaffold/log.h>
#include <httplib.h>

#include <chrono>
#include <exception>
#include <stdexcept>

Chat2ExcelServer::Chat2ExcelServer(const std::string& host, int port)
    : host(host), port(port)
{
    if (host.empty()) {
        throw std::invalid_argument("服务器监听地址不能为空");
    }
    if (port < 1 || port > 65535) {
        throw std::invalid_argument("服务器端口必须在 1～65535 之间");
    }
}

Chat2ExcelServer::~Chat2ExcelServer() {
    // 保留后台线程所有权，确保其访问的 this 和 httpServer 存活到线程退出。
    if (serverThread.joinable()) {
        stop();
    }
}

bool Chat2ExcelServer::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    if (running.load() && httpServer && httpServer->is_running()) {
        if (bitelog::g_logger) {
            WRN("服务器已在运行，地址：{}，端口：{}", host, port);
        }
        return false;
    }

    // 异常退出后的线程可能已经结束但仍可 join，重启前先回收旧线程。
    if (serverThread.joinable()) {
        serverThread.join();
    }
    // 每次启动使用新实例，使停止后重启不会重复绑定路由或继承旧监听状态。
    httpServer = std::make_unique<httplib::Server>();
    httpServer->set_socket_options([](socket_t socket) {
        httplib::default_socket_options(socket);
        // 当前 httplib 默认启用 SO_REUSEPORT；关闭后才能明确发现另一实例占用端口。
#ifdef SO_REUSEPORT
        const int reusePort = 0;
        setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reusePort), sizeof(reusePort));
#endif
        // 保留 SO_REUSEADDR，允许关闭后重新绑定仍有 TIME_WAIT 连接的地址。
        const int reuseAddress = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));
    });
    setRoutes();
    running.store(true);
    threadFinished.store(false);

    try {
        serverThread = std::thread([this]() {
            try {
                if (!httpServer->listen(host, port) && bitelog::g_logger) {
                    ERR("HTTP 监听失败，请检查地址或端口占用，地址：{}，端口：{}", host, port);
                }
            } catch (const std::exception& error) {
                if (bitelog::g_logger) {
                    ERR("HTTP 后台线程异常：{}", error.what());
                }
            } catch (...) {
                if (bitelog::g_logger) {
                    ERR("HTTP 后台线程发生未知异常");
                }
            }
            running.store(false);
            threadFinished.store(true);
        });
    } catch (...) {
        running.store(false);
        threadFinished.store(true);
        throw;
    }

    // listen() 可能因为地址无效或端口冲突立即返回；不能仅凭线程创建成功
    // 就声称服务器已经运行。完成标记同时避免后台抛出异常时一直等待。
    while (!httpServer->is_running() && !threadFinished.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!running.load() || !httpServer->is_running()) {
        serverThread.join();
        running.store(false);
        return false;
    }

    if (bitelog::g_logger) {
        INF("Chat2ExcelServer 启动成功，地址：{}，端口：{}，健康检查：/health", host, port);
    }
    return true;
}

void Chat2ExcelServer::stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const bool wasRunning = running.load() && httpServer && httpServer->is_running();
    if (!wasRunning && bitelog::g_logger) {
        WRN("服务器未运行，无需再次停止，地址：{}，端口：{}", host, port);
    }

    if (httpServer && httpServer->is_running()) {
        httpServer->stop();
    }
    // httplib 会等待其请求工作线程退出；join 后才可以释放服务器实例。
    if (serverThread.joinable()) {
        serverThread.join();
    }
    running.store(false);
    if (wasRunning && bitelog::g_logger) {
        INF("Chat2ExcelServer 已停止，地址：{}，端口：{}", host, port);
    }
}

bool Chat2ExcelServer::isRunning() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return running.load() && httpServer && httpServer->is_running();
}

void Chat2ExcelServer::setRoutes() {
    httpServer->Get("/health", [](const httplib::Request&, httplib::Response& response) {
        // 请求能进入此处理函数说明 HTTP 服务仍在处理请求。
        // 处理函数不获取生命周期锁，保证停止时可等待请求正常结束。
        response.status = 200;
        response.set_content(R"({"status": "healthy"})", "application/json; charset=utf-8");
    });
}
