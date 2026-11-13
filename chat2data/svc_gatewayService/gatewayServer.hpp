#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace httplib
{
    class Server;
}

namespace GatewayService
{
    class GatewayServiceImpl;
    class DiscoveryProcess;

    /**
     * @brief 在网关启动的子进程中运行服务发现。
     * @param etcd_address etcd 服务地址。
     * @param service_names 需要监听的服务名称列表。
     * @param parent_pid 网关主进程的进程 ID。
     * @return 启动失败时返回 1；正常监听期间持续运行。
     */
    int RunDiscoveryWorker(const std::string &etcd_address, const std::vector<std::string> &service_names, int parent_pid);

    class GatewayServer
    {
    public:
        /**
         * @brief 停止 HTTP 服务并释放服务发现等资源。
         */
        ~GatewayServer();

        /**
         * @brief 绑定监听地址并在后台线程中启动 HTTP 服务。
         * @return 服务已运行或启动成功时返回 true；绑定或监听失败时返回 false。
         */
        bool start();

        /**
         * @brief 停止 HTTP 服务并等待监听线程结束。
         * @note 服务发现组件在 GatewayServer 对象销毁时释放。
         */
        void stop();

        /**
         * @brief 查询 HTTP 服务的运行状态。
         * @return 服务正在运行时返回 true，否则返回 false。
         */
        bool isRunning() const;

    private:
        friend class GatewayServerBuilder;

        /**
         * @brief 使用已构建的组件初始化网关服务器。
         * @param address HTTP 服务监听地址。
         * @param port HTTP 服务监听端口。
         * @param server HTTP 服务器对象，所有权转移给网关。
         * @param service 已绑定路由的接口对象。
         * @param discovery 服务发现对象，所有权转移给网关。
         */
        GatewayServer(std::string address, int port, std::unique_ptr<httplib::Server> server, std::shared_ptr<GatewayServiceImpl> service, std::unique_ptr<DiscoveryProcess> discovery);

        std::string _address;                         // HTTP 服务监听地址
        int _port;                                    // HTTP 服务监听端口
        std::unique_ptr<httplib::Server> _server;     // HTTP 服务器对象
        std::shared_ptr<GatewayServiceImpl> _service; // 已绑定路由的接口对象
        std::unique_ptr<DiscoveryProcess> _discovery; // 服务发现对象
        std::atomic_bool _listener_finished{true};    // 监听线程是否已结束
        std::thread _server_thread;                   // HTTP 监听线程
    };

    class GatewayServerBuilder
    {
    public:
        /**
         * @brief 保存构建网关所需的配置。
         * @param address HTTP 服务监听地址。
         * @param port HTTP 服务监听端口。
         * @param etcd_address etcd 服务地址。
         * @param service_names 需要监听的服务名称列表。
         */
        GatewayServerBuilder(std::string address, int port, std::string etcd_address, std::vector<std::string> service_names);

        /**
         * @brief 构建网关组件并绑定 HTTP 路由。
         * @return 已构建的网关服务器及其唯一所有权。
         * @note 此方法启动服务发现管理线程；HTTP 服务需调用 start() 启动。
         */
        std::unique_ptr<GatewayServer> Build() const;

    private:
        std::string _address;                    // HTTP 服务监听地址
        int _port;                               // HTTP 服务监听端口
        std::string _etcd_address;               // etcd 服务地址
        std::vector<std::string> _service_names; // 需要监听的服务名称列表
    };
}
