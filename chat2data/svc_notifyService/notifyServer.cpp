#include "notifyServer.hpp"

#include "BreezeEtcd.hpp"
#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "notifyBusiness.hpp"
#include "notifyServiceImpl.hpp"

#include <brpc/server.h>
#include <charconv>
#include <fstream>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;

        bool IsValidPort(int port)
        {
            return port > 0 && port <= 65535;
        }

        bool IsValidServiceAddress(const std::string &address)
        {
            // 注册地址的末尾端口供调用方连接，可配置为外部映射端口。
            const auto colon = address.rfind(':');
            if (colon == std::string::npos || colon == 0 ||
                address.find_first_of("/ \t\r\n") != std::string::npos || address.find('\0') != std::string::npos)
                return false;

            int port = 0;
            const auto end = address.data() + address.size();
            const auto result = std::from_chars(address.data() + colon + 1, end, port);
            return result.ec == std::errc{} && result.ptr == end && IsValidPort(port);
        }

        void ValidateConfig(const NotifyServerConfig &config)
        {
            // 服务名会作为 etcd 键的一级路径使用。
            const auto &name = config.service_name;
            if (!IsValidPort(config.port) || config.registry_address.empty() || name.empty() ||
                name.find('/') != std::string::npos || !IsValidServiceAddress(config.service_address) ||
                config.queue.max_tasks == 0 || config.queue.max_payload_bytes == 0 ||
                config.queue.task_timeout <= std::chrono::milliseconds::zero())
                throw Chat2DataException(ErrorCode::NOTIFY_SERVER_CONFIG_INVALID);

            const auto &mail = config.mail;
            if ((mail.url.compare(0, 8, "smtps://") != 0 && mail.url.compare(0, 7, "smtp://") != 0) ||
                mail.from.empty() || mail.from.find_first_of("\r\n") != std::string::npos ||
                mail.from.find('\0') != std::string::npos)
                throw Chat2DataException(ErrorCode::NOTIFY_SERVER_CONFIG_INVALID);

            // 启动前检查模板文件可读且有内容。
            std::ifstream html(mail.html_path);
            if (html.peek() == std::char_traits<char>::eof())
                throw Chat2DataException(ErrorCode::NOTIFY_TEMPLATE_UNREADABLE);
        }
    }

    NotifyServer::NotifyServer(std::unique_ptr<NotifyServiceImpl> service)
        : _service(std::move(service))
    {
    }

    NotifyServer::~NotifyServer()
    {
        // 先停止续租，再等待在途 RPC。服务实现和邮件队列随后按成员顺序释放。
        _provider.reset();
        if (_listener)
        {
            _listener->Stop(0);
            _listener->Join();
        }
    }

    NotifyServerBuilder::NotifyServerBuilder(NotifyServerConfig config)
        : _config(std::move(config))
    {
        // 验证码邮件使用 HTML 模板，普通邮件的附件随 RPC 请求传入。
        _config.mail.attachment_path.clear();
    }

    std::unique_ptr<NotifyServer> NotifyServerBuilder::Build() const
    {
        ValidateConfig(_config);
        // 在 SvcProvider 开始连接重试前，用带超时的请求探测 etcd。
        etcd::Client registry(_config.registry_address);
        registry.set_grpc_timeout(std::chrono::seconds(2));
        if (!registry.head().get().is_ok())
            throw Chat2DataException(ErrorCode::NOTIFY_ETCD_CONNECTION_FAILED);

        // 两个 RPC 共用一个发送队列，业务对象与队列随服务实现一并释放。
        auto mail = std::make_unique<Breezecode::MailClient>(_config.mail, _config.queue);
        auto business = std::make_unique<NotifyBusiness>(std::move(mail));
        auto service = std::make_unique<NotifyServiceImpl>(std::move(business), _config.queue.task_timeout);
        auto server = std::unique_ptr<NotifyServer>(new NotifyServer(std::move(service)));

        // 监听成功后发布节点，保证发现服务的调用方可以连接。
        auto listener = std::make_unique<brpc::Server>();
        brpc::ServerOptions options;
        // 内部服务仅暴露通知 RPC，SMTP 凭据留在进程配置中。
        options.has_builtin_services = false;
        if (listener->AddService(server->_service.get(), brpc::SERVER_DOESNT_OWN_SERVICE) != 0 ||
            listener->Start(_config.port, &options) != 0)
            throw Chat2DataException(ErrorCode::NOTIFY_SERVER_START_FAILED);

        // 先将监听器交给 NotifyServer。注册失败时，析构过程会停止监听并清理邮件资源。
        server->_listener = std::move(listener);
        server->_provider = std::make_unique<SvcProvider>(
            _config.registry_address, _config.service_name, _config.service_address);
        if (!server->_provider->registry())
            throw Chat2DataException(ErrorCode::NOTIFY_SERVICE_REGISTRATION_FAILED);
        INF("RPC 服务已注册，服务名 {}，节点地址 {}", _config.service_name, _config.service_address);
        return server;
    }
}
