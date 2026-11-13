#pragma once

#include <memory>

class SvcChannels;

namespace httplib
{
    class Server;
}

namespace GatewayService
{
    class GatewayServiceImpl
    {
    public:
        /**
         * @brief 创建网关接口对象并保存服务信道管理对象。
         * @param channels 用于获取各服务 RPC 信道的共享对象。
         */
        explicit GatewayServiceImpl(std::shared_ptr<SvcChannels> channels);

        /**
         * @brief 将健康检测和业务占位接口绑定到 HTTP 服务器。
         * @param server 接收路由绑定的 HTTP 服务器。
         * @note 健康检测返回服务状态，其余业务接口返回 HTTP 501。
         */
        void BindRoutes(httplib::Server &server);

    private:
        std::shared_ptr<SvcChannels> _channels;
    };
}
