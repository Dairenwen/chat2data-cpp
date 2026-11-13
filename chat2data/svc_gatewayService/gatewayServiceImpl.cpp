#include "gatewayServiceImpl.hpp"

#include <BreezeLog.hpp>
#include <httplib.h>

#include <chrono>
#include <string>
#include <utility>

namespace GatewayService
{
    GatewayServiceImpl::GatewayServiceImpl(std::shared_ptr<SvcChannels> channels)
        : _channels(std::move(channels))
    {
        INF("初始化网关接口");
    }

    void GatewayServiceImpl::BindRoutes(httplib::Server &server)
    {
        INF("绑定网关路由");
        const auto not_implemented = [](const httplib::Request &request, httplib::Response &response)
        {
            DBG("接口尚未实现: {} {}", request.method, request.path);
            response.status = 501;
            response.set_content(R"({"error":"Not Implemented"})", "application/json; charset=utf-8");
        };

        const char *post_routes[] = {
            "/api/user/valid/nickname", "/api/user/valid/email", "/api/user/register",
            "/api/user/session/login", "/api/user/passwd/login", "/api/user/code",
            "/api/user/vcode/login", "/api/user/logout", "/api/file/upload/info",
            "/api/file/upload", "/api/file/preview", "/api/file/list", "/api/file/chat/map",
            "/api/file/sqlite/upload", "/api/db/connect", "/api/db/disconnect",
            "/api/db/table/data", "/api/db/connection/status", "/api/ai/models",
            "/api/ai/session/create", "/api/ai/chatSessionLists", "/api/ai/history",
            "/api/ai/delete", "/api/ai/sendStreamMessage"};

        for (const char *route : post_routes)
            server.Post(route, not_implemented); //

        const char *get_routes[] = {
            "/api/user/info", "/api/file/info", "/api/file/download", "/api/db/tables"};

        for (const char *route : get_routes)
            server.Get(route, not_implemented);

        server.Delete(R"(/api/file/[^/]+)", not_implemented);

        server.Get("/health", [](const httplib::Request &, httplib::Response &response)
                   {
            DBG("处理健康检测请求");
            const auto now = std::chrono::system_clock::now().time_since_epoch();// 获取当前时间戳
            const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();// 将时间戳转换为秒数
            response.set_content("{\"status\":\"healthy\",\"service\":\"GatewayService\",\"timestamp\":" + std::to_string(timestamp) + "}","application/json; charset=utf-8"); });
    }
}
