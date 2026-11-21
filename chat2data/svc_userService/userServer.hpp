#pragma once

#include <memory>

class SvcProvider;
class SvcWatcher;

namespace brpc
{
    class Server;
}

namespace chat2data
{
    class UserServerBuilder;
    class UserServiceImpl;

    // 管理用户 RPC 监听器以及服务注册和发现对象的生命周期。
    class UserServer
    {
    public:
        ~UserServer();

        /**
         * @brief 通过 BreezeKit 启动用户 RPC 监听。
         * @return 监听已启动时为 true，绑定端口或挂载服务失败时为 false。
         */
        bool start();

        /**
         * @brief 停止接收请求并等待正在执行的 RPC 完成。
         */
        void stop();

    private:
        friend class UserServerBuilder;

        UserServer(int port, std::unique_ptr<UserServiceImpl> service, std::unique_ptr<SvcWatcher> watcher, std::unique_ptr<SvcProvider> provider);

        int _port;
        std::unique_ptr<UserServiceImpl> _service; // 用户服务实现对象
        std::unique_ptr<SvcWatcher> _watcher;      // 服务发现对象
        std::unique_ptr<SvcProvider> _provider;    // 服务注册对象
        std::shared_ptr<brpc::Server> _server;     // BreezeKit RPC 服务器对象
    };
}
