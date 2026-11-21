#include "userServer.hpp"

#include "BreezeEtcd.hpp"
#include "BreezeRPC.hpp"
#include "userServiceImpl.hpp"

#include <utility>

namespace chat2data
{
    UserServer::UserServer(int port, std::unique_ptr<UserServiceImpl> service,
                           std::unique_ptr<SvcWatcher> watcher, std::unique_ptr<SvcProvider> provider)
        : _port(port), _service(std::move(service)), _watcher(std::move(watcher)), _provider(std::move(provider))
    {
    }

    UserServer::~UserServer()
    {
        stop();
    }

    bool UserServer::start()
    {
        if (_server)
            return true;

        // 工厂接管堆上构造的 RPC 服务对象，交出 brpc::Server 的所有权给 BreezeKit
        _server = RpcServerFactory::create(_port, _service.release());
        return static_cast<bool>(_server);
    }

    void UserServer::stop()
    {
        // 先结束注册和发现，避免在停止监听后仍有回调触发。
        _provider.reset();
        if (_server)
        {
            _server->Stop(0);
            _server->Join();
            _server.reset();
        }
        _watcher.reset();
    }
}
