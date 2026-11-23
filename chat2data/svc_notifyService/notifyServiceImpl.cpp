#include "notifyServiceImpl.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "notifyBusiness.hpp"

#include <brpc/closure_guard.h>
#include <bthread/bthread.h>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::ErrorCode;
        using Clock = std::chrono::steady_clock;

        ErrorCode WaitForSend(std::future<bool> result, std::chrono::milliseconds timeout)
        {
            // 使用单调时钟计算提交后的等待时间，避免系统时钟调整影响超时判断。
            const auto started = Clock::now();
            while (result.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
            {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started) >= timeout)
                    return ErrorCode::NOTIFY_SEND_TIMEOUT;

                // 等待 SMTP 时让出 bthread 执行权，以便处理其他 RPC。
                bthread_usleep(1000);
            }
            // 客户端以 false 统一报告队列拒绝、任务过期和 SMTP 发送失败。
            return result.get() ? ErrorCode::SUCCESS : ErrorCode::NOTIFY_SEND_FAILED;
        }

        // 两个通知接口共用响应完成、结果等待和错误映射流程。
        template <typename Request, typename Response, typename Submit>
        void HandleNotifyRpc(const Request &request, Response &response, google::protobuf::Closure *done,
                             const char *method, std::chrono::milliseconds timeout, Submit submit)
        {
            // 发送结果或业务错误写入响应后，由 ClosureGuard 完成 RPC。
            brpc::ClosureGuard guard(done);
            // 请求 ID 用于关联调用结果，字段校验与邮件构造交由业务层负责。
            auto code = ErrorCode::NOTIFY_INVALID_INPUT;
            if (!request.request_id().empty())
            {
                INF("RPC {} 开始处理，请求 ID {}", method, request.request_id());
                try
                {
                    code = WaitForSend(submit(), timeout);
                }
                catch (const chat2Data::Chat2DataException &error)
                {
                    // 保留业务层定义的参数错误码。
                    code = error.Code();
                }
                catch (...)
                {
                    // 调用依赖或读取 future 时抛出的异常统一映射为业务失败。
                    code = ErrorCode::NOTIFY_BUSINESS_FAILED;
                }
            }

            response.set_request_id(request.request_id());
            response.set_error_code(static_cast<int>(code));
            response.set_error_msg(code == ErrorCode::SUCCESS ? "" : chat2Data::error2String(code));
            // 通过方法、请求 ID 和错误码定位问题，邮件内容仅用于投递。
            if (code != ErrorCode::SUCCESS)
                WRN("RPC {} 失败，请求 ID {}，错误码 {}", method, request.request_id(), static_cast<int>(code));
            else
                INF("RPC {} 成功，请求 ID {}", method, request.request_id());
        }
    }

    NotifyServiceImpl::NotifyServiceImpl(std::unique_ptr<NotifyBusiness> business,
                                         std::chrono::milliseconds task_timeout)
        : _business(std::move(business)), _task_timeout(task_timeout)
    {
    }

    NotifyServiceImpl::~NotifyServiceImpl() = default;

    void NotifyServiceImpl::SendVerifyCode(google::protobuf::RpcController *, const chat2Data::SendVerifyCodeRequest *request,
                                           chat2Data::SendVerifyCodeResponse *response, google::protobuf::Closure *done)
    {
        HandleNotifyRpc(*request, *response, done, "SendVerifyCode", _task_timeout, [&]
                        { return _business->SendVerifyCode(*request); });
    }

    void NotifyServiceImpl::SendEmail(google::protobuf::RpcController *, const chat2Data::SendEmailRequest *request,
                                      chat2Data::SendEmailResponse *response, google::protobuf::Closure *done)
    {
        HandleNotifyRpc(*request, *response, done, "SendEmail", _task_timeout, [&]
                        { return _business->SendEmail(*request); });
    }
}
