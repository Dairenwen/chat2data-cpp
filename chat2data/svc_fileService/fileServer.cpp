#include "fileServer.hpp"

#include "BreezeEtcd.hpp"
#include "fileServiceImpl.hpp"

#include <brpc/server.h>

namespace chat2data
{
    FileServer::FileServer(const Breezefdfs::fdfs_settings &settings)
    {
        Breezefdfs::FDFSClient::init(settings);
    }

    FileServer::~FileServer()
    {
        // 先停止租约续期，再停止监听并等待业务请求退出。
        _provider.reset();
        if (_listener)
        {
            _listener->Stop(0);
            _listener->Join();
            _listener.reset();
        }
        _watcher.reset();
        _service.reset();
        Breezefdfs::FDFSClient::destroy();
    }
}
