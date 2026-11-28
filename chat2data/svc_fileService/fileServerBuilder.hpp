#pragma once

#include "BreezeFDFS.hpp"
#include "BreezeODB.hpp"
#include "BreezeRedis.hpp"

#include <memory>
#include <string>

namespace chat2data
{
    class FileServer;

    // 文件服务监听、注册发现及各依赖客户端使用的启动参数。
    struct FileServerConfig
    {
        int port = 9004;
        std::string service_name;
        std::string service_address; // etcd 发布的地址，由调用方用于访问 RPC。
        std::string registry_address;
        std::string excel_service;
        std::string db_service;
        std::string ai_service;
        BreezeODB::mysql_settings mysql;
        Breezeredis::redis_settings redis;
        Breezefdfs::fdfs_settings fdfs;
    };

    class FileServerBuilder
    {
    public:
        explicit FileServerBuilder(FileServerConfig config);

        // 创建依赖，启动 RPC 监听和服务发现，再注册当前节点。
        std::unique_ptr<FileServer> Build() const;

    private:
        FileServerConfig _config;
    };
}
