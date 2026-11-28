#pragma once

#include "BreezeFDFS.hpp"

#include <memory>

class SvcProvider;
class SvcWatcher;

namespace brpc
{
    class Server;
}

namespace chat2Data
{
    class FileServiceImpl;
}

namespace chat2data
{
    class FileServerBuilder;

    // 文件服务等待在途 RPC 结束后释放 FastDFS 全局客户端。
    class FileServer
    {
    public:
        // 停止租约续期和监听，等待在途请求完成后释放客户端。
        ~FileServer();

    private:
        friend class FileServerBuilder;
        explicit FileServer(const Breezefdfs::fdfs_settings &settings);

        std::unique_ptr<chat2Data::FileServiceImpl> _service; // 文件服务实现，持有文件业务对象
        std::unique_ptr<brpc::Server> _listener;              // brpc 监听器，持有文件服务实现
        std::unique_ptr<SvcWatcher> _watcher;                 // 服务发现监听器，持有服务通道
        std::unique_ptr<SvcProvider> _provider;               // 服务注册器，持有服务发现客户端
    };
}
