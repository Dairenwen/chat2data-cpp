#pragma once

#include "userService.pb.h"

#include <memory>

namespace chat2Data
{
    class UserBusiness;
}

namespace chat2data
{
    // 将 UserService 的 Protobuf 请求转交用户业务层，并按协议返回处理结果。
    class UserServiceImpl final : public chat2Data::UserService
    {
    public:
        /**
         * @brief 使用调用方已构造的用户业务对象。
         * @param business 用户业务对象，生命周期由服务和调用方共享。
         */
        explicit UserServiceImpl(std::shared_ptr<chat2Data::UserBusiness> business);

        /**
         * @brief 校验昵称格式并查询当前是否可用。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和待检查的昵称。
         * @param response 返回请求 ID 和校验结果。昵称可用时 error_code 为 0，已占用时返回对应错误码。
         * @param done 响应准备完成后执行的回调。
         */
        void ValidNickname(google::protobuf::RpcController *controller, const chat2Data::ValidNicknameRequest *request,
                           chat2Data::ValidNicknameResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 校验邮箱格式并查询当前是否可用。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和待检查的邮箱。
         * @param response 返回请求 ID 和校验结果。邮箱可用时 error_code 为 0，已占用时返回对应错误码。
         * @param done 响应准备完成后执行的回调。
         */
        void ValidEmail(google::protobuf::RpcController *controller, const chat2Data::ValidEmailRequest *request,
                        chat2Data::ValidEmailResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 注册用户，保存带盐的密码哈希，初始状态为离线。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、昵称、邮箱和密码。
         * @param response 返回请求 ID 和注册结果。成功后由客户端发起登录。
         * @param done 响应准备完成后执行的回调。
         */
        void UserRegister(google::protobuf::RpcController *controller, const chat2Data::UserRegisterRequest *request,
                          chat2Data::UserRegisterResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 使用已有会话登录，沿用原会话 ID 和有效期，并校正在线状态。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和已有会话 ID。
         * @param response 返回请求 ID 和登录结果。会话缺失和会话过期分别使用对应的错误码。
         * @param done 响应准备完成后执行的回调。
         */
        void SessionLogin(google::protobuf::RpcController *controller, const chat2Data::SessionLoginRequest *request,
                          chat2Data::SessionLoginResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 根据昵称或邮箱校验密码，成功后创建登录会话。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、账号和密码。nickname 字段填写用户昵称或注册邮箱。
         * @param response 返回请求 ID 和登录结果。成功时填写新会话的 session_id。
         * @param done 响应准备完成后执行的回调。
         */
        void PasswdLogin(google::protobuf::RpcController *controller, const chat2Data::PasswdLoginRequest *request,
                         chat2Data::PasswdLoginResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 向注册邮箱发送有效期为五分钟的验证码。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和与注册记录一致的邮箱地址。
         * @param response 返回请求 ID 和发送结果。通知服务确认成功后填写 code_id，验证码通过邮件送达。
         * @param done 响应准备完成后执行的回调。
         */
        void GetCode(google::protobuf::RpcController *controller, const chat2Data::GetCodeRequest *request,
                     chat2Data::GetCodeResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 原子校验并消费验证码，随后为匹配的用户创建登录会话。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、邮箱、验证码 ID 和保留前导零的六位验证码字符串。
         * @param response 返回请求 ID 和登录结果。成功时填写新会话的 session_id。
         * @param done 响应准备完成后执行的回调。
         */
        void VcodeLogin(google::protobuf::RpcController *controller, const chat2Data::VcodeLoginRequest *request,
                        chat2Data::VcodeLoginResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 撤销指定会话，并按该用户的其余有效会话校正在线状态。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和待撤销的会话 ID。
         * @param response 返回请求 ID 和退出结果。删除流程完成后返回成功。
         * @param done 响应准备完成后执行的回调。
         */
        void Logout(google::protobuf::RpcController *controller, const chat2Data::LogoutRequest *request,
                    chat2Data::LogoutResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 校验登录会话并查询所属用户的公开信息。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和用于确定用户身份的会话 ID。
         * @param response 返回请求 ID 和查询结果。成功时在 user_info 中填写用户 ID、昵称和邮箱。
         * @param done 响应准备完成后执行的回调。
         */
        void GetUserInfo(google::protobuf::RpcController *controller, const chat2Data::GetUserInfoRequest *request,
                         chat2Data::GetUserInfoResponse *response, google::protobuf::Closure *done) override;

        /**
         * @brief 为网关和其他内部服务校验会话的有效性。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和待校验的会话 ID。
         * @param response 返回请求 ID 和校验结果。有效时 valid 为 true 并填写 user_id。
         *                  会话缺失或过期时 valid 为 false，error_code 为 0。校验故障返回对应错误码。
         * @param done 响应准备完成后执行的回调。
         */
        void IsSessionValid(google::protobuf::RpcController *controller, const chat2Data::IsSessionValidRequest *request,
                            chat2Data::IsSessionValidResponse *response, google::protobuf::Closure *done) override;

    private:
        std::shared_ptr<chat2Data::UserBusiness> _business;
    };
}
