#include "userServiceImpl.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "userBusiness.hpp"

#include <brpc/closure_guard.h>
#include <utility>

namespace chat2data
{
    using google::protobuf::Closure;
    using google::protobuf::RpcController;

    namespace
    {
        using chat2Data::ErrorCode;

        // 这里使用模板方法模式封装 RPC 处理流程，避免重复的异常捕获和响应字段设置
        template <typename Request, typename Response, typename Operation>
        void HandleUserRpc(const Request *request, Response *response, Closure *done,
                           const char *method, Operation operation)
        {
            // 正常返回和异常处理路径均通过 ClosureGuard 完成 RPC 响应。
            brpc::ClosureGuard guard(done);
            response->Clear(); // 避免重复使用时残留上次调用的业务字段

            // 请求 ID 用于关联请求与响应。缺少 ID 时直接返回参数错误。
            auto code = ErrorCode::USER_INVALID_INPUT;
            if (!request->request_id().empty())
            {
                try
                {
                    code = operation();
                }
                catch (const chat2Data::Chat2DataException &error)
                {
                    // 将已有业务错误转换为协议约定的错误码
                    code = error.Code();
                }
                catch (...)
                {
                    // 依赖异常统一返回业务失败，日志记录对应方法的请求类型。
                    ERR("处理用户 RPC 请求失败。请求类型为 {}", request->GetDescriptor()->name());
                    code = ErrorCode::USER_BUSINESS_FAILED;
                }
            }

            // 错误响应只填写公共字段，成功响应保留接口生成的业务字段。
            if (code != ErrorCode::SUCCESS)
            {
                response->Clear();
                response->set_error_msg(chat2Data::error2String(code));
                WRN("RPC {} 失败，请求 ID {}，错误码 {}", method, request->request_id(), static_cast<int>(code));
            }
            else
                INF("RPC {} 成功，请求 ID {}", method, request->request_id());
            response->set_request_id(request->request_id());
            response->set_error_code(static_cast<int>(code));
        }
    }

    UserServiceImpl::UserServiceImpl(std::shared_ptr<chat2Data::UserBusiness> business)
        : _business(std::move(business))
    {
    }

    void UserServiceImpl::ValidNickname(RpcController *, const chat2Data::ValidNicknameRequest *request,
                                        chat2Data::ValidNicknameResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "ValidNickname", [&]
                      {
            // 校验必填昵称，格式规则由业务层统一处理。
            if (request->nickname().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 按协议使用错误码表示昵称已被占用。
            return _business->ValidNickname(request->nickname()) ? ErrorCode::SUCCESS : ErrorCode::USER_NICKNAME_TAKEN; });
    }

    void UserServiceImpl::ValidEmail(RpcController *, const chat2Data::ValidEmailRequest *request,
                                     chat2Data::ValidEmailResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "ValidEmail", [&]
                      {
            // 校验必填邮箱，格式规则由业务层统一处理。
            if (request->email().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 按协议使用错误码表示邮箱已被占用。
            return _business->ValidEmail(request->email()) ? ErrorCode::SUCCESS : ErrorCode::USER_EMAIL_TAKEN; });
    }

    void UserServiceImpl::UserRegister(RpcController *, const chat2Data::UserRegisterRequest *request,
                                       chat2Data::UserRegisterResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "UserRegister", [&]
                      {
            // 校验注册所需字段，密码规则和唯一性由业务层处理。
            if (request->nickname().empty() || request->email().empty() || request->password().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 注册完成后按协议返回公共结果字段。
            _business->Register(request->nickname(), request->email(), request->password());
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::SessionLogin(RpcController *, const chat2Data::SessionLoginRequest *request,
                                       chat2Data::SessionLoginResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "SessionLogin", [&]
                      {
            // 会话 ID 用于定位已有会话。
            if (request->session_id().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 业务层校验会话并校正状态。成功时返回公共结果字段。
            _business->SessionLogin(request->session_id());
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::PasswdLogin(RpcController *, const chat2Data::PasswdLoginRequest *request,
                                      chat2Data::PasswdLoginResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "PasswdLogin", [&]
                      {
            // nickname 字段承载昵称或邮箱，登录还需提供密码。
            if (request->nickname().empty() || request->password().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 业务层验证凭证后返回新会话 ID，将其写入响应。
            response->set_session_id(_business->PasswdLogin(request->nickname(), request->password()));
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::GetCode(RpcController *, const chat2Data::GetCodeRequest *request,
                                  chat2Data::GetCodeResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "GetCode", [&]
                      {
            // 邮箱用于定位验证码的接收地址。
            if (request->email().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 复用业务层发信流程。成功响应的业务字段为验证码 ID。
            response->set_code_id(_business->GetCode(request->request_id(), request->email()));
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::VcodeLogin(RpcController *, const chat2Data::VcodeLoginRequest *request,
                                     chat2Data::VcodeLoginResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "VcodeLogin", [&]
                      {
            // 按六位数字字符串校验验证码，保留前导零。
            const auto &code = request->verify_code();
            if (request->email().empty() || request->code_id().empty() ||
                code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos)
                return ErrorCode::USER_INVALID_INPUT;

            // 由业务层原子消费验证码，成功后返回新会话 ID。
            response->set_session_id(_business->VcodeLogin(request->email(), request->code_id(), code));
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::Logout(RpcController *, const chat2Data::LogoutRequest *request,
                                 chat2Data::LogoutResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "Logout", [&]
                      {
            // 会话 ID 用于撤销当前设备的会话。
            if (request->session_id().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 业务层完成会话撤销后返回公共结果字段。
            _business->Logout(request->session_id());
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::GetUserInfo(RpcController *, const chat2Data::GetUserInfoRequest *request,
                                      chat2Data::GetUserInfoResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "GetUserInfo", [&]
                      {
            // 会话 ID 用于确认请求对应的用户。
            if (request->session_id().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 业务层完成会话校验后，将公开字段填入嵌套消息。
            const auto user = _business->GetUserInfo(request->session_id());
            auto *info = response->mutable_user_info();
            info->set_user_id(user.userId);
            info->set_nickname(user.nickname);
            info->set_email(user.email);
            return ErrorCode::SUCCESS; });
    }

    void UserServiceImpl::IsSessionValid(RpcController *, const chat2Data::IsSessionValidRequest *request,
                                         chat2Data::IsSessionValidResponse *response, Closure *done)
    {
        HandleUserRpc(request, response, done, "IsSessionValid", [&]
                      {
            // 校验请求携带用于内部鉴权的会话 ID。
            if (request->session_id().empty())
                return ErrorCode::USER_INVALID_INPUT;

            // 校验完成后填写 valid，仅有效会话携带用户 ID。
            const auto user_id = _business->IsSessionValid(request->session_id());
            response->set_valid(user_id.has_value());
            if (user_id)
                response->set_user_id(*user_id);
            return ErrorCode::SUCCESS; });
    }
}
