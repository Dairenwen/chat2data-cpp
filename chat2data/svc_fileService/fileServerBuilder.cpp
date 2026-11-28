#include "BreezeRPC.hpp"

#include "fileServerBuilder.hpp"

#include "BreezeEtcd.hpp"
#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "fileServer.hpp"
#include "fileServiceImpl.hpp"
#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <netdb.h>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;
        using chat2Data::Utils;

        bool IsServiceName(const std::string &name)
        {
            return !name.empty() && name.find('/') == std::string::npos;
        }

        void ValidateConfig(const FileServerConfig &config)
        {
            if (!Utils::IsValidPort(config.port) || !Utils::IsValidHostPort(config.service_address) ||
                config.registry_address.empty() || !IsServiceName(config.service_name) ||
                !IsServiceName(config.excel_service) || !IsServiceName(config.db_service) ||
                !IsServiceName(config.ai_service) || config.fdfs.trackers.empty() ||
                config.fdfs.connect_timeout <= 0 || config.fdfs.network_timeout <= 0 ||
                config.mysql.host.empty() || config.mysql.user.empty() || config.mysql.db.empty() ||
                !Utils::IsValidPort(config.mysql.port) || config.mysql.connection_pool_size == 0 ||
                config.redis.host.empty() || !Utils::IsValidPort(config.redis.port) ||
                config.redis.db < 0 || config.redis.connection_pool_size == 0)
                throw Chat2DataException(ErrorCode::FILE_SERVER_CONFIG_INVALID);

            for (const auto &tracker : config.fdfs.trackers)
            {
                if (!Utils::IsValidHostPort(tracker))
                    throw Chat2DataException(ErrorCode::FILE_SERVER_CONFIG_INVALID);

                // SDK 初始化失败会结束进程，先检查 Tracker 的 IPv4 地址能否解析。
                const auto host = tracker.substr(0, tracker.rfind(':'));
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *resolved = nullptr;
                if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0)
                    throw Chat2DataException(ErrorCode::FILE_SERVER_CONFIG_INVALID);
                freeaddrinfo(resolved);
            }
        }
    }

    FileServerBuilder::FileServerBuilder(FileServerConfig config)
        : _config(std::move(config))
    {
    }

    std::unique_ptr<FileServer> FileServerBuilder::Build() const
    {
        ValidateConfig(_config);
        // 先通过有超时的请求检查 etcd，避免底层连接持续重试。
        etcd::Client registry(_config.registry_address);
        registry.set_grpc_timeout(std::chrono::seconds(2));
        if (!registry.head().get().is_ok())
            throw Chat2DataException(ErrorCode::FILE_ETCD_CONNECTION_FAILED);
        // 按照mysql配置连接数据库，按redis配置连接缓存，监视excel解析子服务、存储子服务以及AI子服务
        auto database = BreezeODB::DBFactory::mysql(_config.mysql);
        auto redis = Breezeredis::RedisFactory::create(_config.redis);
        auto channels = std::make_shared<SvcChannels>();
        const std::vector<std::string> watched{_config.excel_service, _config.db_service, _config.ai_service};
        for (const auto &name : watched)
            channels->setWatch(name);
        // 连接fastDFS并设置server的fdfs_client配置，设置server的启动参数
        auto server = std::unique_ptr<FileServer>(new FileServer(_config.fdfs));
        server->_service = std::make_unique<chat2Data::FileServiceImpl>(chat2Data::FileBusiness(chat2Data::FileData(std::move(database), std::move(redis)),
                                                                                                chat2Data::MakeFastDfsStorage(),
                                                                                                chat2Data::MakeFileDownstream(channels, _config.excel_service)));
        server->_listener = std::make_unique<brpc::Server>();
        brpc::ServerOptions options;
        options.has_builtin_services = false;
        // 上传和删除可跨 RPC 持有一条 MySQL 连接。限制在途请求数，避免工作线程阻塞在连接池上。
        options.max_concurrency = static_cast<int>(std::min(_config.mysql.connection_pool_size, static_cast<unsigned>(std::numeric_limits<int>::max())));
        if (server->_listener->AddService(server->_service.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0 || server->_listener->Start(_config.port, &options) != 0)
            throw Chat2DataException(ErrorCode::FILE_SERVER_START_FAILED);

        server->_watcher = std::make_unique<SvcWatcher>(
            _config.registry_address,
            [channels, watched](const std::string &name, const std::string &address)
            {
                if (!address.empty() && std::find(watched.begin(), watched.end(), name) != watched.end())
                {
                    channels->addNode(name, address);
                    INF("服务上线 {} {}", name, address);
                }
            },
            [channels, watched](const std::string &name, const std::string &address)
            {
                if (!address.empty() && std::find(watched.begin(), watched.end(), name) != watched.end())
                {
                    channels->delNode(name, address);
                    INF("服务下线 {} {}", name, address);
                }
            });
        if (!server->_watcher->watch())
            throw Chat2DataException(ErrorCode::FILE_SERVICE_DISCOVERY_FAILED);
        // 注册当前节点，注册失败会导致调用方无法访问 RPC
        server->_provider = std::make_unique<SvcProvider>(_config.registry_address, _config.service_name, _config.service_address);
        if (!server->_provider->registry())
            throw Chat2DataException(ErrorCode::FILE_SERVICE_REGISTRATION_FAILED);
        INF("RPC 服务已注册，服务名 {}，节点地址 {}", _config.service_name, _config.service_address);
        return server;
    }
}
