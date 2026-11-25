#include "excelParserServer.hpp"

#include "BreezeEtcd.hpp"
#include "BreezeFDFS.hpp"
#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "excelParserServiceImpl.hpp"
#include "utils.hpp"

#include <brpc/server.h>
#include <chrono>
#include <netdb.h>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;
        using chat2Data::Utils;

        // 检查服务器配置参数，避免启动后失败
        void ValidateConfig(const ExcelParserServerConfig &config)
        {
            // etcd 用服务名构造键路径，注册端口可以与本地监听端口不同。
            if (!Utils::IsValidPort(config.port) || config.registry_address.empty() ||
                config.service_name.empty() || config.service_name.find('/') != std::string::npos ||
                !Utils::IsValidHostPort(config.service_address) || config.trackers.empty() ||
                config.connect_timeout <= 0 || config.network_timeout <= 0)
                throw Chat2DataException(ErrorCode::EXCEL_SERVER_CONFIG_INVALID);

            for (const auto &tracker : config.trackers)
            {
                if (!Utils::IsValidHostPort(tracker))
                    throw Chat2DataException(ErrorCode::EXCEL_SERVER_CONFIG_INVALID);

                // FastDFS SDK 初始化失败会终止进程，先用与 SDK 一致的 IPv4 解析检查 Tracker。
                const auto host = tracker.substr(0, tracker.rfind(':'));
                addrinfo hints{};
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *resolved = nullptr;
                if (getaddrinfo(host.c_str(), nullptr, &hints, &resolved) != 0)
                    throw Chat2DataException(ErrorCode::EXCEL_SERVER_CONFIG_INVALID);
                freeaddrinfo(resolved);
            }
        }
    }

    ExcelParserServer::ExcelParserServer(const Breezefdfs::fdfs_settings &settings)
    {
        // 初始化 FastDFS 全局客户端，供 RPC 处理共享下载
        Breezefdfs::FDFSClient::init(settings);
    }

    ExcelParserServer::~ExcelParserServer()
    {
        // FastDFS 全局客户端由请求共享，等待在途 RPC 完成后才能销毁
        _provider.reset();
        if (_listener)
        {
            _listener->Stop(0);
            _listener->Join();
            _listener.reset();
        }
        _service.reset();
        Breezefdfs::FDFSClient::destroy();
    }

    ExcelParserServerBuilder::ExcelParserServerBuilder(ExcelParserServerConfig config)
        : _config(std::move(config))
    {
    }

    std::unique_ptr<ExcelParserServer> ExcelParserServerBuilder::Build() const
    {
        ValidateConfig(_config);
        // 先用带超时的请求探测 etcd，避免阻塞启动
        etcd::Client registry(_config.registry_address);
        registry.set_grpc_timeout(std::chrono::seconds(2)); // 设置 2 秒超时，避免阻塞启动
        if (!registry.head().get().is_ok())
            throw Chat2DataException(ErrorCode::EXCEL_ETCD_CONNECTION_FAILED);
        // 初始化 FastDFS 客户端
        Breezefdfs::fdfs_settings settings;
        settings.trackers = _config.trackers;               // 设置 Tracker 地址列表
        settings.connect_timeout = _config.connect_timeout; // 设置连接超时，单位为秒
        settings.network_timeout = _config.network_timeout; // 设置网络读写超时，单位为秒
        auto server = std::unique_ptr<ExcelParserServer>(new ExcelParserServer(settings));
        server->_service = std::make_unique<ExcelParserServiceImpl>(ExcelParserBusiness(Breezefdfs::FDFSClient::download_to_file));
        server->_listener = std::make_unique<brpc::Server>();

        brpc::ServerOptions options;
        options.has_builtin_services = false;
        // 服务器对象持有 RPC 实现，brpc 在监听期间借用该实例。
        if (server->_listener->AddService(server->_service.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0 ||
            server->_listener->Start(_config.port, &options) != 0)
            throw Chat2DataException(ErrorCode::EXCEL_SERVER_START_FAILED);

        // 注册时监听器已经就绪。注册失败时，服务器析构会停止监听并释放客户端。
        server->_provider = std::make_unique<SvcProvider>(_config.registry_address, _config.service_name, _config.service_address);
        if (!server->_provider->registry())
            throw Chat2DataException(ErrorCode::EXCEL_SERVICE_REGISTRATION_FAILED);

        INF("RPC 服务已注册，服务名 {}，节点地址 {}", _config.service_name, _config.service_address);
        return server;
    }
}
