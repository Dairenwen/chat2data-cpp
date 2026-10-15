#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

/**
 * @brief 基于 cpp-httplib 的 Chat2Excel HTTP 服务器。
 *
 * 使用独立后台线程接收请求，提供 /health 健康检查。start() 等待实际监听成功后返回，主线程可继续执行；stop() 和析构函数负责回收后台线程。
 * start()/stop() 的生命周期操作通过互斥量串行执行，应由控制线程调用，不应在 HTTP 请求处理函数内部调用 stop()。
 * 日志使用 bite_scaffold；调用方应在创建服务器前初始化 bitelog 日志器。
 */
class Chat2ExcelServer {
public:
    /**
     * @brief 初始化监听地址和端口，暂不启动监听。
     * @param host 监听地址，默认 0.0.0.0，不能为空。
     * @param port 监听端口，默认 8080，有效范围为 1～65535。
     * @throws std::invalid_argument 地址为空或端口不在有效范围内。
     */
    explicit Chat2ExcelServer(const std::string& host = "0.0.0.0", int port = 8080);

    /** @brief 对象销毁前停止服务器，并等待后台线程退出。 */
    ~Chat2ExcelServer();

    Chat2ExcelServer(const Chat2ExcelServer&) = delete;
    Chat2ExcelServer& operator=(const Chat2ExcelServer&) = delete;
    Chat2ExcelServer(Chat2ExcelServer&&) = delete;
    Chat2ExcelServer& operator=(Chat2ExcelServer&&) = delete;

    /**
     * @brief 设置路由并在独立线程中启动服务器。
     * @return 成功进入监听返回 true；已在运行、绑定失败或监听启动失败返回 false。
     *         已运行时记录提示，不重复创建线程或注册路由。
     * @throws std::exception 分配服务器、设置路由或创建线程失败时传播标准库异常。
     *         后台监听异常会记录日志并使运行状态变为 false。
     */
    bool start();

    /**
     * @brief 停止接受请求，等待正在处理的请求及后台线程退出。
     * @note 未运行时记录提示；已结束但尚未回收的线程也会被回收。
     * @throws std::system_error 互斥量操作或线程回收失败时传播线程库异常。
     */
    void stop();

    /**
     * @brief 获取实际运行状态。
     * @return 服务器已成功启动且 cpp-httplib 仍在监听时返回 true，否则返回 false。
     * @throws std::system_error 获取生命周期互斥量失败时传播标准库异常。
     */
    bool isRunning() const;

private:
    /**
     * @brief 在当前 HTTP 服务器实例上绑定所有路由。
     * @note 仅在 start() 持有生命周期锁、创建 HTTP 服务器之后调用。
     * @throws std::exception 路由注册中的分配或正则表达式构造失败。
     */
    void setRoutes();

    const std::string host;
    const int port;
    std::unique_ptr<httplib::Server> httpServer;
    std::thread serverThread;
    mutable std::mutex lifecycleMutex;
    std::atomic<bool> running{false};
    std::atomic<bool> threadFinished{true};
};
