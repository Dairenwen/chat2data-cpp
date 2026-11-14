#pragma once

#include <memory>

class SvcChannels;

namespace httplib
{
    class Server;
    struct Request;
    struct Response;
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
         * @note 健康检测返回服务状态，其余业务接口暂时只保留独立处理函数。
         * @note 路由回调引用当前对象，处理请求期间对象需保持有效。
         */
        void BindRoutes(httplib::Server &server);

    private:
        /**
         * @brief 校验用户昵称请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void ValidateNickname(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 校验用户邮箱请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void ValidateEmail(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 注册用户请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void RegisterUser(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 会话登录请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void SessionLogin(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 密码登录请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void PasswordLogin(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取验证码请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetVerificationCode(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 验证码登录请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void VerificationCodeLogin(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 退出登录请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void Logout(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取用户信息请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetUserInfo(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 上传文件信息请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void UploadFileInfo(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取文件信息请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetFileInfo(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 上传文件请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void UploadFile(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 下载文件请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void DownloadFile(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 删除文件请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void DeleteFile(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 预览 Excel 文件请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void PreviewExcel(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取文件列表请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetFileList(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 关联文件和聊天会话请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void AssociateFileWithSession(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 上传 SQLite 文件请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void UploadSQLiteFile(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 连接数据库请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void ConnectDatabase(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取数据库表列表请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void ListDatabaseTables(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 断开数据库连接请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void DisconnectDatabase(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取数据库表数据请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetTableData(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取连接的临时表列表请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetConnectionStatus(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取模型列表请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetModels(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 创建聊天会话请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void CreateChatSession(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取聊天会话列表请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetChatSessions(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 获取聊天历史消息请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void GetChatHistory(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 删除聊天会话请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void DeleteChatSession(const httplib::Request &request, httplib::Response &response);

        /**
         * @brief 发送流式聊天消息请求。
         * @param request HTTP 请求。
         * @param response HTTP 响应。
         */
        void SendStreamMessage(const httplib::Request &request, httplib::Response &response);

        std::shared_ptr<SvcChannels> _channels;
    };
}
