#pragma once

#include "BreezeCode.hpp"

#include <memory>
#include <string>

class SvcProvider;

namespace brpc
{
    class Server;
}

namespace chat2data
{
    class NotifyServiceImpl;

    // 汇集通知服务监听、etcd 注册和 SMTP 投递所需的配置。
    struct NotifyServerConfig
    {
        int port = 0;                       // 监听端口
        std::string registry_address;       // etcd 注册端点
        std::string service_name;           // 服务注册名称
        std::string service_address;        // 注册的 RPC 地址
        Breezecode::mail_settings mail;     // SMTP 服务器设置
        Breezecode::MailQueueOptions queue; // 邮件队列设置
    };

    // 管理通知服务实现、brpc 监听器和 etcd 租约的生命周期。
    class NotifyServer
    {
    public:
        /**
         * @brief 停止 etcd 续租和 RPC 监听，等待在途请求完成后释放业务资源。
         */
        ~NotifyServer();

    private:
        friend class NotifyServerBuilder;

        /**
         * @brief 接管待挂载到 brpc 监听器的通知 RPC 实现。
         * @param service 通知 RPC 实现对象，由当前服务器持有。
         */
        explicit NotifyServer(std::unique_ptr<NotifyServiceImpl> service);

        std::unique_ptr<NotifyServiceImpl> _service;
        std::unique_ptr<brpc::Server> _listener;
        std::unique_ptr<SvcProvider> _provider;
    };

    // 根据配置构造服务，并在监听成功后向 etcd 注册节点。
    class NotifyServerBuilder
    {
    public:
        /**
         * @brief 保存通知服务的启动配置。
         * @param config 包含监听端口、注册地址、SMTP 和队列参数。
         * @note 验证码邮件的固定附件路径在构造时清空。
         */
        explicit NotifyServerBuilder(NotifyServerConfig config);

        /**
         * @brief 校验启动配置和 etcd 连通性，启动 RPC 监听后注册节点。
         * @return 已监听并完成 etcd 注册的通知服务。
         * @throws chat2Data::Chat2DataException 配置、模板检查、etcd 探测、监听或注册失败。
         */
        std::unique_ptr<NotifyServer> Build() const;

    private:
        NotifyServerConfig _config;
    };
}
