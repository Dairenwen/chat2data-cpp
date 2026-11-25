#pragma once

#include <memory>
#include <string>
#include <vector>

class SvcProvider;

namespace brpc
{
    class Server;
}

namespace Breezefdfs
{
    struct fdfs_settings;
}

namespace chat2data
{
    class ExcelParserServiceImpl;

    // 汇集 RPC 监听、etcd 注册和 FastDFS 下载所需的配置
    struct ExcelParserServerConfig
    {
        int port;                          // RPC 监听端口。
        std::string registry_address;      // etcd 注册端点。
        std::string service_name;          // etcd 注册名称。
        std::string service_address;       // 调用方可访问的 host:port。
        std::vector<std::string> trackers; // FastDFS Tracker 地址列表。
        int connect_timeout;               // FastDFS 连接超时，单位为秒。
        int network_timeout;               // FastDFS 网络读写超时，单位为秒。
    };

    // 管理 brpc 监听器、etcd 租约和进程内 FastDFS 客户端。
    class ExcelParserServer
    {
    public:
        /**
         * @brief 停止 etcd 续租和 RPC 监听，等待在途请求后释放 FastDFS 客户端。
         */
        ~ExcelParserServer();

    private:
        friend class ExcelParserServerBuilder;

        // 在启动 RPC 监听前初始化进程内共享的 FastDFS 客户端。
        explicit ExcelParserServer(const Breezefdfs::fdfs_settings &settings);

        std::unique_ptr<ExcelParserServiceImpl> _service;
        std::unique_ptr<brpc::Server> _listener;
        std::unique_ptr<SvcProvider> _provider;
    };

    class ExcelParserServerBuilder
    {
    public:
        /**
         * @brief 保存 Excel 解析服务的启动配置。
         * @param config RPC 监听、etcd 注册和 FastDFS 配置。
         */
        explicit ExcelParserServerBuilder(ExcelParserServerConfig config);

        /**
         * @brief 校验配置及 etcd 连通性，启动 RPC 监听后注册节点。
         * @return 已监听并完成 etcd 注册的 Excel 解析服务。
         * @throws chat2Data::Chat2DataException 配置、etcd 探测、监听或注册失败。
         */
        std::unique_ptr<ExcelParserServer> Build() const;

    private:
        ExcelParserServerConfig _config;
    };
}
