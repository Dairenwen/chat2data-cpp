#pragma once

#include "notifyService.pb.h"

#include <chrono>
#include <memory>

namespace chat2data
{
    class NotifyBusiness;

    // 将通知 RPC 转交业务层，在邮件任务完成或等待超时后返回结果。
    class NotifyServiceImpl final : public chat2Data::NotifyService
    {
    public:
        /**
         * @brief 接管通知业务对象并设置发送结果的等待预算。
         * @param business 负责校验和提交邮件任务的通知业务对象。
         * @param task_timeout 与邮件队列使用的任务时间预算一致。
         */
        NotifyServiceImpl(std::unique_ptr<NotifyBusiness> business, std::chrono::milliseconds task_timeout);

        /**
         * @brief 释放通知业务对象及其持有的邮件客户端。
         */
        ~NotifyServiceImpl() override;

        /**
         * @brief 向指定邮箱发送验证码并返回投递结果。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、收件邮箱和六位数字验证码。
         * @param response 返回原始请求 ID、错误码和错误描述。SMTP 接收邮件后错误码为 0。
         * @param done 响应准备完成后由 ClosureGuard 执行的回调。
         */
        void SendVerifyCode(google::protobuf::RpcController *controller, const chat2Data::SendVerifyCodeRequest *request,
                            chat2Data::SendVerifyCodeResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 发送 HTML 邮件和可选附件，并返回投递结果。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、收件邮箱、主题、HTML 正文和可选附件。
         * @param response 返回原始请求 ID、错误码和错误描述。SMTP 接收邮件后错误码为 0。
         * @param done 响应准备完成后由 ClosureGuard 执行的回调。
         */
        void SendEmail(google::protobuf::RpcController *controller, const chat2Data::SendEmailRequest *request,
                       chat2Data::SendEmailResponse *response, google::protobuf::Closure *done) override;

    private:
        std::unique_ptr<NotifyBusiness> _business;
        const std::chrono::milliseconds _task_timeout;
    };
}
