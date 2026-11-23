#include "notifyServer.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"

#include <chrono>
#include <csignal>
#include <gflags/gflags.h>
#include <thread>
#include <utility>

DEFINE_int32(notify_port, 9002, "通知 RPC 监听端口");
DEFINE_string(notify_service, "notifyService", "通知服务注册名称");
DEFINE_string(notify_address, "", "注册的 RPC 地址，默认使用 dev-env-service 和监听端口");
DEFINE_string(registry_address, "http://dev-etcd:2379", "etcd 注册端点");
DEFINE_string(log_level, "info", "BreezeLog 日志级别");
DEFINE_string(smtp_url, "smtps://smtp.qq.com:465", "SMTP 服务器地址");
DEFINE_string(smtp_username, "3484725023@qq.com", "SMTP 登录账号");
DEFINE_string(smtp_password, "lshcqpcfdmilchjj", "SMTP 客户端授权码");
DEFINE_string(smtp_from, "3484725023@qq.com", "发件邮箱");
DEFINE_string(smtp_login_options, "AUTH=LOGIN", "SMTP 认证选项");
DEFINE_string(mail_html_path, NOTIFY_DEFAULT_MAIL_TEMPLATE, "验证码邮件 HTML 模板路径");
DEFINE_uint32(mail_max_tasks, 32, "邮件队列最大任务数");
DEFINE_uint64(mail_max_payload_bytes, 512ULL * 1024 * 1024, "邮件队列最大负载字节数");
DEFINE_int64(mail_task_timeout_ms, 25000, "排队和 SMTP 传输的总预算，单位毫秒");

namespace
{
    volatile std::sig_atomic_t exit_requested = 0;

    // 信号处理只设置退出标志，主线程负责停机和释放资源。
    void HandleSignal(int)
    {
        exit_requested = 1;
    }
}

int main(int argc, char **argv)
{
    using chat2Data::Chat2DataException;
    using chat2Data::ErrorCode;

    gflags::ParseCommandLineFlags(&argc, &argv, true);
    auto startup_error = ErrorCode::NOTIFY_SERVER_START_FAILED;
    try
    {
        const auto level = spdlog::level::from_str(FLAGS_log_level);
        if (level == spdlog::level::off && FLAGS_log_level != "off")
            throw Chat2DataException(ErrorCode::NOTIFY_SERVER_CONFIG_INVALID);
        BreezeLog::Logger::setLevel(level);

        chat2data::NotifyServerConfig config{};
        config.port = FLAGS_notify_port;
        config.service_name = FLAGS_notify_service;
        config.registry_address = FLAGS_registry_address;
        // etcd 发布的是调用方可达的地址，默认使用开发容器名和监听端口。
        config.service_address = FLAGS_notify_address.empty()
                                     ? "dev-env-service:" + std::to_string(config.port)
                                     : FLAGS_notify_address;

        config.mail.username = FLAGS_smtp_username;
        config.mail.password = FLAGS_smtp_password;
        config.mail.url = FLAGS_smtp_url;
        config.mail.from = FLAGS_smtp_from;
        config.mail.login_options = FLAGS_smtp_login_options;
        config.mail.html_path = FLAGS_mail_html_path;

        // 两类邮件共用队列，任务预算覆盖排队和 SMTP 发送。
        config.queue.max_tasks = FLAGS_mail_max_tasks;
        config.queue.max_payload_bytes = FLAGS_mail_max_payload_bytes;
        config.queue.task_timeout = std::chrono::milliseconds(FLAGS_mail_task_timeout_ms);

        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);
        // Build 完成 RPC 监听和 etcd 注册后，服务进入运行状态。
        auto server = chat2data::NotifyServerBuilder(std::move(config)).Build();
        INF("通知 RPC 服务已启动，监听端口为 {}", FLAGS_notify_port);
        // 主线程保留服务的所有权，退出作用域时按顺序回收注册、RPC 和邮件资源。
        while (!exit_requested)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
    }
    catch (const Chat2DataException &error)
    {
        startup_error = error.Code();
    }
    catch (...)
    {
        // 底层异常统一使用启动失败错误码，日志只输出已定义的错误描述。
    }
    ERR("通知服务错误码 {}，{}", static_cast<int>(startup_error), chat2Data::error2String(startup_error));
    return 1;
}
