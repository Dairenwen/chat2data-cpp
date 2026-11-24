#include "BreezeRPC.hpp"

#include "userServerBuilder.hpp"

#include "BreezeEtcd.hpp"
#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "userBusiness.hpp"
#include "userServer.hpp"
#include "userServiceImpl.hpp"
#include "utils.hpp"

#include <chrono>
#include <utility>

namespace chat2data
{
    using chat2Data::Chat2DataException;
    using chat2Data::ErrorCode;

    namespace
    {
        void CheckEtcd(const std::string &address)
        {
            // BreezeKit 在连接失败时持续重试。启动前先用有超时的请求检查端点。
            etcd::Client client(address);
            client.set_grpc_timeout(std::chrono::seconds(2)); // 设置超时
            if (!client.head().get().is_ok())
                throw Chat2DataException(ErrorCode::USER_ETCD_CONNECTION_FAILED);
        }
    }

    void UserServerBuilder::SetMysqlConfig(MysqlConfig config)
    {
        _mysql = std::move(config);
    }

    void UserServerBuilder::SetRedisConfig(RedisConfig config)
    {
        _redis = std::move(config);
    }

    void UserServerBuilder::SetPort(int port)
    {
        _port = port;
    }

    void UserServerBuilder::SetRegistryAddress(std::string address)
    {
        _registry_address = std::move(address);
    }

    void UserServerBuilder::SetServiceName(std::string name)
    {
        _service_name = std::move(name);
    }

    void UserServerBuilder::SetServiceAddress(std::string address)
    {
        _service_address = std::move(address);
    }

    void UserServerBuilder::SetEtcdAddress(std::string address)
    {
        _etcd_address = std::move(address);
    }

    void UserServerBuilder::SetWatchedService(std::string name)
    {
        _watched_service = std::move(name);
    }

    void UserServerBuilder::SetNotifyRpcTimeout(int timeout_ms)
    {
        _notify_rpc_timeout_ms = timeout_ms;
    }

    std::unique_ptr<UserServer> UserServerBuilder::Build() const
    {
        const auto notify_rpc_timeout_ms = _notify_rpc_timeout_ms.value_or(chat2Data::kDefaultNotifyRpcTimeoutMs);
        if (!chat2Data::Utils::IsValidPort(_port) || notify_rpc_timeout_ms <= 0)
            throw Chat2DataException(ErrorCode::USER_SERVER_CONFIG_INVALID);

        if (_registry_address.empty() || _etcd_address.empty() || _service_address.empty() ||
            _service_name.empty() || _watched_service.empty() ||
            _service_name.find('/') != std::string::npos || _watched_service.find('/') != std::string::npos)
            throw Chat2DataException(ErrorCode::USER_SERVER_CONFIG_INVALID);

        CheckEtcd(_registry_address);           // 检查 etcd 端点是否可用
        if (_etcd_address != _registry_address) // 如果发现和注册使用不同的 etcd 地址，则也检查发现端点
            CheckEtcd(_etcd_address);

        const auto database = BreezeODB::DBFactory::mysql(_mysql);
        const auto redis = Breezeredis::RedisFactory::create(_redis);
        auto channels = std::make_shared<SvcChannels>();
        channels->setWatch(_watched_service);

        // 发现事件和验证码业务共用配置中的通知服务名作为通道键
        auto watcher = std::make_unique<SvcWatcher>(
            _etcd_address,
            [channels, name = _watched_service](const std::string &service, const std::string &address)
            {
                if (service == name && !address.empty())
                {
                    channels->addNode(service, address);
                    INF("服务上线 {} {}", service, address);
                }
            },
            [channels, name = _watched_service](const std::string &service, const std::string &address)
            {
                if (service == name && !address.empty())
                {
                    channels->delNode(service, address);
                    INF("服务下线 {} {}", service, address);
                }
            });
        auto provider = std::make_unique<SvcProvider>(_registry_address, _service_name, _service_address);
        auto sessions = std::make_shared<chat2Data::SessionData>(database, redis);
        auto business = std::make_shared<chat2Data::UserBusiness>(
            std::make_shared<chat2Data::SessionManager>(sessions),
            std::make_shared<chat2Data::UserData>(database, redis),
            std::make_shared<chat2Data::VerifyCodeData>(redis), channels, _watched_service, notify_rpc_timeout_ms);
        auto service = std::make_unique<UserServiceImpl>(std::move(business));
        // 必须在堆上构造 UserServer 对象
        auto server = std::unique_ptr<UserServer>(
            new UserServer(_port, std::move(service), std::move(watcher), std::move(provider)));

        // 先确认监听成功，再注册本节点，避免发布无法连接的地址。
        if (!server->start())
            throw Chat2DataException(ErrorCode::USER_SERVER_START_FAILED);
        INF("启动服务发现，监听服务 {}", _watched_service);
        if (!server->_watcher->watch())
            throw Chat2DataException(ErrorCode::USER_SERVICE_DISCOVERY_FAILED);
        if (!server->_provider->registry())
            throw Chat2DataException(ErrorCode::USER_SERVICE_REGISTRATION_FAILED);
        INF("RPC 服务已注册，服务名 {}，节点地址 {}", _service_name, _service_address);
        return server;
    }
}
