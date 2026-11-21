#include "gatewayServiceImpl.hpp"
#include "errorHandler.hpp"
#include "userService.pb.h"

#include <BreezeLog.hpp>
#include <BreezeRPC.hpp>
#include <BreezeUtil.hpp>
#include <gflags/gflags.h>
#include <httplib.h>

#include <chrono>
#include <string>
#include <utility>

DECLARE_string(user_service);

namespace GatewayService
{
    namespace
    {
        using chat2Data::ErrorCode;
        using chat2Data::UserService_Stub;

        // 从 HTTP 请求中解析 JSON 对象，若请求体为空或非对象则返回空对象
        Json::Value ReadBody(const httplib::Request &request)
        {
            const auto body = BreezeUtil::JSON::unserialize(request.body);
            return body && body->isObject() ? *body : Json::Value{};
        }

        // 从 JSON 对象中读取字符串字段，若字段不存在或非字符串则返回空字符串
        std::string ReadString(const Json::Value &body, const char *name)
        {
            const auto &field = body[name];
            return field.isString() ? field.asString() : std::string{};
        }

        // 将 RPC 调用结果封装为 HTTP 响应，若 result 为空则不返回 result 字段
        void Reply(httplib::Response &response, int status, const std::string &request_id,
                   int code, const std::string &message, const Json::Value &result = {})
        {
            Json::Value body(Json::objectValue);
            body["requestId"] = request_id;
            body["errorCode"] = code;
            body["errorMsg"] = message;
            if (!result.isNull())
                body["result"] = result;

            const auto json = BreezeUtil::JSON::serialize(body);
            response.status = json ? status : 500;
            if (json)
                response.set_content(*json, "application/json; charset=utf-8");
        }

        // 将错误码和错误信息封装为 HTTP 响应
        void ReplyError(httplib::Response &response, int status, const std::string &request_id, ErrorCode code)
        {
            Reply(response, status, request_id, static_cast<int>(code), chat2Data::error2String(code));
        }

        // 使用模版函数定义用户服务的 RPC 方法类型，便于后续调用
        template <typename Request, typename Response>
        using UserMethod = void (UserService_Stub::*)(google::protobuf::RpcController *, const Request *, Response *, google::protobuf::Closure *);

        // 每次 RPC 只发一次，避免重复注册、创建会话或发送验证码。
        template <typename Request, typename Response>
        bool CallUser(UserService_Stub &stub, UserMethod<Request, Response> method,
                      const Request &request, Response &result, httplib::Response &http_response)
        {
            brpc::Controller controller;
            controller.set_max_retry(0);                             // 禁止重试，避免重复注册、创建会话或发送验证码
            (stub.*method)(&controller, &request, &result, nullptr); // 发起 RPC 调用
            if (!controller.Failed() && result.request_id() == request.request_id())
                return true;

            WRN("调用用户 RPC 失败，请求类型 {}", request.GetDescriptor()->name());
            ReplyError(http_response, 502, request.request_id(), ErrorCode::GATEWAY_USER_RPC_FAILED);
            return false;
        }

        // 受保护的接口先校验会话，再用同一个信道发起业务调用。
        bool AuthorizeUser(UserService_Stub &stub, const std::string &request_id,
                           const std::string &session_id, httplib::Response &response)
        {
            chat2Data::IsSessionValidRequest request;
            request.set_request_id(request_id);
            request.set_session_id(session_id);
            chat2Data::IsSessionValidResponse auth;
            // 发起 RPC 调用，校验会话是否有效
            if (!CallUser(stub, &UserService_Stub::IsSessionValid, request, auth, response))
                return false;
            if (auth.error_code() != static_cast<int>(ErrorCode::SUCCESS))
            {
                Reply(response, 200, request_id, auth.error_code(), auth.error_msg());
                return false;
            }
            // 若会话无效，则返回 401 错误
            if (!auth.valid())
            {
                ReplyError(response, 401, request_id, ErrorCode::USER_SESSION_NOT_FOUND);
                return false;
            }
            return true;
        }

        /**
         * @brief 转发 UserService RPC 请求，封装 stub 调用、结果序列化与 HTTP 返回
         * @tparam Request RPC 请求消息类型（protobuf 消息）
         * @tparam Response RPC 响应消息类型（protobuf 消息）
         * @param channels 服务通道管理器，用于获取 UserService 连接通道
         * @param response httplib HTTP 响应对象，用于填充返回给前端的 JSON
         * @param request 入参，protobuf 封装的 RPC 请求体
         * @param has_fields 是否校验 request 存在有效字段（可选业务校验标记）
         * @param method UserService_Stub 的成员函数指针，指向具体 RPC 方法
         * @param make_result 回调函数，将 RPC Response 转为 Json::Value；为空时使用默认组装逻辑
         * @param session_id 会话ID，可选，用于链路追踪/登录态传递，可为 nullptr
         */
        template <typename Request, typename Response>
        void ForwardUser(SvcChannels &channels, httplib::Response &response,
                         const Request &request, bool has_fields,
                         UserMethod<Request, Response> method,
                         Json::Value (*make_result)(const Response &) = nullptr,
                         const std::string *session_id = nullptr)
        {
            const auto &request_id = request.request_id();
            // 若请求体缺少必要字段，则返回 400 错误
            if (request_id.empty() || !has_fields)
            {
                ReplyError(response, 400, request_id, ErrorCode::USER_INVALID_INPUT);
                return;
            }

            // 获取用户服务的信道，若不存在则返回 503 错误
            const auto channel = channels.getNode(FLAGS_user_service);
            if (!channel)
            {
                ReplyError(response, 503, request_id, ErrorCode::GATEWAY_USER_RPC_FAILED);
                return;
            }
            // 创建用户服务存根，若需要会话验证则先调用 AuthorizeUser，再调用指定的 RPC 方法
            UserService_Stub stub(channel.get());
            if (session_id && !AuthorizeUser(stub, request_id, *session_id, response))
                return;
            // 发起 RPC 调用，若失败则返回 502 错误
            Response result;
            if (!CallUser(stub, method, request, result, response))
                return;
            // 将 RPC 调用结果封装为 JSON 对象，若调用成功且提供了结果转换函数，则调用该函数生成结果对象
            Json::Value data;
            if (result.error_code() == static_cast<int>(ErrorCode::SUCCESS) && make_result)
                data = make_result(result);
            Reply(response, 200, request_id, result.error_code(), result.error_msg(), data);
        }

        // 将会话信息封装为 JSON 对象
        template <typename Response>
        Json::Value SessionResult(const Response &response)
        {
            Json::Value data(Json::objectValue);
            data["sessionId"] = response.session_id();
            return data;
        }

        // 将验证码信息封装为 JSON 对象
        Json::Value CodeResult(const chat2Data::GetCodeResponse &response)
        {
            Json::Value data(Json::objectValue);
            data["codeId"] = response.code_id();
            return data;
        }

        // 将用户信息封装为 JSON 对象
        Json::Value UserInfoResult(const chat2Data::GetUserInfoResponse &response)
        {
            const auto &info = response.user_info(); // 先读取用户信息
            Json::Value data(Json::objectValue);
            auto &user = data["userInfo"];
            user["userId"] = info.user_id();
            user["nickname"] = info.nickname();
            user["email"] = info.email();
            return data;
        }
    }

    GatewayServiceImpl::GatewayServiceImpl(std::shared_ptr<SvcChannels> channels)
        : _channels(std::move(channels))
    {
        INF("初始化网关接口");
    }

    void GatewayServiceImpl::BindRoutes(httplib::Server &server)
    {
        INF("绑定网关路由");
        // 用户接口
        server.Post("/api/user/valid/nickname", [this](const httplib::Request &request, httplib::Response &response)
                    { ValidateNickname(request, response); });
        server.Post("/api/user/valid/email", [this](const httplib::Request &request, httplib::Response &response)
                    { ValidateEmail(request, response); });
        server.Post("/api/user/register", [this](const httplib::Request &request, httplib::Response &response)
                    { RegisterUser(request, response); });
        server.Post("/api/user/session/login", [this](const httplib::Request &request, httplib::Response &response)
                    { SessionLogin(request, response); });
        server.Post("/api/user/passwd/login", [this](const httplib::Request &request, httplib::Response &response)
                    { PasswordLogin(request, response); });
        server.Post("/api/user/code", [this](const httplib::Request &request, httplib::Response &response)
                    { GetVerificationCode(request, response); });
        server.Post("/api/user/vcode/login", [this](const httplib::Request &request, httplib::Response &response)
                    { VerificationCodeLogin(request, response); });
        server.Post("/api/user/logout", [this](const httplib::Request &request, httplib::Response &response)
                    { Logout(request, response); });
        server.Get("/api/user/info", [this](const httplib::Request &request, httplib::Response &response)
                   { GetUserInfo(request, response); });

        // 文件接口
        server.Post("/api/file/upload/info", [this](const httplib::Request &request, httplib::Response &response)
                    { UploadFileInfo(request, response); });
        server.Get("/api/file/info", [this](const httplib::Request &request, httplib::Response &response)
                   { GetFileInfo(request, response); });
        server.Post("/api/file/upload", [this](const httplib::Request &request, httplib::Response &response)
                    { UploadFile(request, response); });
        server.Get("/api/file/download", [this](const httplib::Request &request, httplib::Response &response)
                   { DownloadFile(request, response); });
        server.Delete(R"(/api/file/[^/]+)", [this](const httplib::Request &request, httplib::Response &response)
                      { DeleteFile(request, response); });
        server.Post("/api/file/preview", [this](const httplib::Request &request, httplib::Response &response)
                    { PreviewExcel(request, response); });
        server.Post("/api/file/list", [this](const httplib::Request &request, httplib::Response &response)
                    { GetFileList(request, response); });
        server.Post("/api/file/chat/map", [this](const httplib::Request &request, httplib::Response &response)
                    { AssociateFileWithSession(request, response); });
        server.Post("/api/file/sqlite/upload", [this](const httplib::Request &request, httplib::Response &response)
                    { UploadSQLiteFile(request, response); });

        // 数据库接口
        server.Post("/api/db/connect", [this](const httplib::Request &request, httplib::Response &response)
                    { ConnectDatabase(request, response); });
        server.Get("/api/db/tables", [this](const httplib::Request &request, httplib::Response &response)
                   { ListDatabaseTables(request, response); });
        server.Post("/api/db/disconnect", [this](const httplib::Request &request, httplib::Response &response)
                    { DisconnectDatabase(request, response); });
        server.Post("/api/db/table/data", [this](const httplib::Request &request, httplib::Response &response)
                    { GetTableData(request, response); });
        server.Post("/api/db/connection/status", [this](const httplib::Request &request, httplib::Response &response)
                    { GetConnectionStatus(request, response); });

        // AI 接口
        server.Post("/api/ai/models", [this](const httplib::Request &request, httplib::Response &response)
                    { GetModels(request, response); });
        server.Post("/api/ai/session/create", [this](const httplib::Request &request, httplib::Response &response)
                    { CreateChatSession(request, response); });
        server.Post("/api/ai/chatSessionLists", [this](const httplib::Request &request, httplib::Response &response)
                    { GetChatSessions(request, response); });
        server.Post("/api/ai/history", [this](const httplib::Request &request, httplib::Response &response)
                    { GetChatHistory(request, response); });
        server.Post("/api/ai/delete", [this](const httplib::Request &request, httplib::Response &response)
                    { DeleteChatSession(request, response); });
        server.Post("/api/ai/sendStreamMessage", [this](const httplib::Request &request, httplib::Response &response)
                    { SendStreamMessage(request, response); });

        server.Get("/health", [](const httplib::Request &, httplib::Response &response)
                   {
                        DBG("处理健康检测请求");
                        const auto now = std::chrono::system_clock::now().time_since_epoch();// 获取当前时间戳
                        const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();// 将时间戳转换为秒数
                        response.set_content("{\"status\":\"healthy\",\"service\":\"GatewayService\",\"timestamp\":" + std::to_string(timestamp) + "}","application/json; charset=utf-8"); });
    }

    // 下面的实现流程大致都一样，读取 HTTP 请求体->解析 JSON 字段->构造 RPC 请求
    // ->调用 ForwardUser 转发给用户服务->最后返回结果
    void GatewayServiceImpl::ValidateNickname(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::ValidNicknameRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_nickname(ReadString(body, "nickname"));
        ForwardUser(*_channels, response, rpc, !rpc.nickname().empty(), &UserService_Stub::ValidNickname);
    }

    void GatewayServiceImpl::ValidateEmail(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::ValidEmailRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_email(ReadString(body, "email"));
        ForwardUser(*_channels, response, rpc, !rpc.email().empty(), &UserService_Stub::ValidEmail);
    }

    void GatewayServiceImpl::RegisterUser(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::UserRegisterRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_nickname(ReadString(body, "nickname"));
        rpc.set_email(ReadString(body, "email"));
        rpc.set_password(ReadString(body, "password"));
        const bool has_fields = !rpc.nickname().empty() && !rpc.email().empty() && !rpc.password().empty();
        ForwardUser(*_channels, response, rpc, has_fields, &UserService_Stub::UserRegister);
    }

    void GatewayServiceImpl::SessionLogin(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::SessionLoginRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_session_id(ReadString(body, "sessionId"));
        ForwardUser(*_channels, response, rpc, !rpc.session_id().empty(), &UserService_Stub::SessionLogin);
    }

    void GatewayServiceImpl::PasswordLogin(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::PasswdLoginRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_nickname(ReadString(body, "nickname"));
        rpc.set_password(ReadString(body, "password"));
        const bool has_fields = !rpc.nickname().empty() && !rpc.password().empty();
        // 注意这里 PasswordLogin 需要返回 sessionId
        ForwardUser(*_channels, response, rpc, has_fields, &UserService_Stub::PasswdLogin, SessionResult<chat2Data::PasswdLoginResponse>);
    }

    void GatewayServiceImpl::GetVerificationCode(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::GetCodeRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_email(ReadString(body, "email"));
        // GetVerificationCode 需要返回验证码
        ForwardUser(*_channels, response, rpc, !rpc.email().empty(), &UserService_Stub::GetCode, CodeResult);
    }

    void GatewayServiceImpl::VerificationCodeLogin(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::VcodeLoginRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_email(ReadString(body, "email"));
        rpc.set_code_id(ReadString(body, "codeId"));
        rpc.set_verify_code(ReadString(body, "verifyCode"));
        const bool has_fields = !rpc.email().empty() && !rpc.code_id().empty() && !rpc.verify_code().empty();
        ForwardUser(*_channels, response, rpc, has_fields, &UserService_Stub::VcodeLogin, SessionResult<chat2Data::VcodeLoginResponse>);
    }

    void GatewayServiceImpl::Logout(const httplib::Request &request, httplib::Response &response)
    {
        const auto body = ReadBody(request);
        chat2Data::LogoutRequest rpc;
        rpc.set_request_id(ReadString(body, "requestId"));
        rpc.set_session_id(ReadString(body, "sessionId"));
        // Logout 需要校验会话是否有效，因此传入 session_id
        ForwardUser<chat2Data::LogoutRequest, chat2Data::LogoutResponse>(
            *_channels, response, rpc, !rpc.session_id().empty(), &UserService_Stub::Logout, nullptr, &rpc.session_id());
    }

    void GatewayServiceImpl::GetUserInfo(const httplib::Request &request, httplib::Response &response)
    {
        chat2Data::GetUserInfoRequest rpc;
        rpc.set_request_id(request.get_param_value("requestId"));
        rpc.set_session_id(request.get_param_value("sessionId"));
        // GetUserInfo 需要校验会话是否有效，因此传入 session_id
        ForwardUser(*_channels, response, rpc, !rpc.session_id().empty(), &UserService_Stub::GetUserInfo, UserInfoResult, &rpc.session_id());
    }

    void GatewayServiceImpl::UploadFileInfo(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetFileInfo(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::UploadFile(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::DownloadFile(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::DeleteFile(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::PreviewExcel(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetFileList(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::AssociateFileWithSession(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::UploadSQLiteFile(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::ConnectDatabase(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::ListDatabaseTables(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::DisconnectDatabase(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetTableData(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetConnectionStatus(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetModels(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::CreateChatSession(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetChatSessions(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::GetChatHistory(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::DeleteChatSession(const httplib::Request &, httplib::Response &)
    {
    }

    void GatewayServiceImpl::SendStreamMessage(const httplib::Request &, httplib::Response &)
    {
    }
}
