#include "userBusiness.hpp"

#include "BreezeRPC.hpp"
#include "errorHandler.hpp"
#include "notifyService.pb.h"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <odb/exceptions.hxx>
#include <odb/mysql/exceptions.hxx>
#include <utility>

namespace chat2Data
{
    namespace
    {
        // 计算 UTF-8 字符串的字符数，续字节不计入
        std::size_t CharacterCount(const std::string &text)
        {
            // UTF-8 续字节属于同一个字符。
            return std::count_if(text.begin(), text.end(), [](unsigned char byte)
                                 { return (byte & 0xc0) != 0x80; });
        }

        // 昵称、邮箱和密码的基本格式校验，业务层在此基础上判断唯一性
        void ValidateNickname(const std::string &nickname)
        {
            const auto length = CharacterCount(nickname);
            if (length == 0 || length > 32 || nickname.find_first_not_of(' ') == std::string::npos ||
                std::any_of(nickname.begin(), nickname.end(), [](unsigned char ch)
                            { return std::iscntrl(ch); }))
                throw Chat2DataException(ErrorCode::USER_INVALID_INPUT);
        }

        // 邮箱格式校验，长度、@位置、空白字符和控制字符
        void ValidateEmail(const std::string &email)
        {
            const auto at = email.find('@');
            if (email.size() > 255 || at == std::string::npos || at == 0 || at + 1 == email.size() ||
                email.find('@', at + 1) != std::string::npos ||
                std::any_of(email.begin(), email.end(), [](unsigned char ch)
                            { return std::isspace(ch) || std::iscntrl(ch); }))
                throw Chat2DataException(ErrorCode::USER_INVALID_INPUT);
        }

        // 密码格式校验，长度和字符类别，要求至少两类字符：数字、小写字母、大写字母、符号
        void ValidatePassword(const std::string &password)
        {
            if (CharacterCount(password) < 8)
                throw Chat2DataException(ErrorCode::USER_INVALID_INPUT);

            bool digit = false;
            bool lower = false;
            bool upper = false;
            bool symbol = false;
            for (const unsigned char ch : password)
            {
                digit |= std::isdigit(ch) != 0;
                lower |= std::islower(ch) != 0;
                upper |= std::isupper(ch) != 0;
                symbol |= std::ispunct(ch) != 0;
            }
            if (digit + lower + upper + symbol < 2)
                throw Chat2DataException(ErrorCode::USER_INVALID_INPUT);
        }
    }

    UserBusiness::UserBusiness(std::shared_ptr<SessionManager> sessions, std::shared_ptr<UserData> users,
                               std::shared_ptr<VerifyCodeData> codes, std::shared_ptr<SvcChannels> channels)
        : _sessions(std::move(sessions)), _users(std::move(users)),
          _codes(std::move(codes)), _channels(std::move(channels))
    {
    }

    bool UserBusiness::ValidNickname(const std::string &nickname)
    {
        ValidateNickname(nickname);
        return !_users->NicknameExists(nickname);
    }

    bool UserBusiness::ValidEmail(const std::string &email)
    {
        ValidateEmail(email);
        return !_users->EmailExists(email);
    }

    void UserBusiness::Register(const std::string &nickname, const std::string &email, const std::string &password)
    {
        ValidateNickname(nickname);
        ValidateEmail(email);
        ValidatePassword(password);
        if (_users->NicknameExists(nickname))
            throw Chat2DataException(ErrorCode::USER_NICKNAME_TAKEN);
        if (_users->EmailExists(email))
            throw Chat2DataException(ErrorCode::USER_EMAIL_TAKEN);

        UserEntity user;
        user.userId = Utils::GenerateUUID(); // userID采用UUID生成
        user.nickname = nickname;
        user.email = email;
        user.password = Utils::HashPassword(password); // 保存带盐的密码哈希，不保存明文密码
        try
        {
            // Save中会开启事务并提交，若违反唯一约束会抛出异常
            _users->Save(user);
        }
        catch (const odb::object_already_persistent &)
        {
            throw Chat2DataException(ErrorCode::USER_REGISTER_CONFLICT);
        }
        catch (const odb::mysql::database_exception &error)
        {
            // MySQL 错误码 1062 表示违反唯一约束，可能是昵称或邮箱已存在
            constexpr unsigned int kDuplicateKey = 1062;
            if (error.error() == kDuplicateKey)
                throw Chat2DataException(ErrorCode::USER_REGISTER_CONFLICT);
            throw;
        }
        // 注册成功后使缓存失效，避免缓存中存在旧数据
        _users->DeleteCache(user.userId);
    }

    // 使用昵称或邮箱登录，优先按昵称查询
    std::string UserBusiness::PasswdLogin(const std::string &account, const std::string &password)
    {
        // 先按昵称查询，若不存在再按邮箱查询
        auto user = _users->FindByNickname(account);
        if (!user)
            user = _users->FindByEmail(account);
        if (!user || !Utils::VerifyPassword(password, user->passwordHash))
            throw Chat2DataException(ErrorCode::USER_INVALID_LOGIN);
        // 登录成功后创建会话并校正在线状态
        return CreateLoginSession(user->userId);
    }

    // 使用邮箱和验证码登录，验证码在五分钟内有效
    std::string UserBusiness::GetCode(const std::string &requestId, const std::string &email)
    {
        ValidateEmail(email);
        const auto user = _users->FindByEmail(email);
        if (!user || user->email != email)
            throw Chat2DataException(ErrorCode::USER_NOT_FOUND);

        const auto channel = _channels->getNode("notifyService");
        if (!channel)
            throw Chat2DataException(ErrorCode::USER_NOTIFICATION_FAILED);

        const CachedVerifyCode code{Utils::GenerateSecureId(), Utils::GenerateVerificationCode(),
                                    user->email, Utils::NowMs()};
        if (!_codes->Save(code))
            throw Chat2DataException(ErrorCode::USER_CODE_MISSING);

        SendVerifyCodeRequest request;
        request.set_request_id(requestId);
        request.set_email(code.email);
        request.set_verify_code(code.verifyCode);
        SendVerifyCodeResponse response;
        brpc::Controller controller;
        // 发信结果不确定时，自动重试可能导致同一封验证码邮件重复投递。
        controller.set_max_retry(0);
        NotifyService_Stub(channel.get()).SendVerifyCode(&controller, &request, &response, nullptr);
        if (controller.Failed() || response.error_code() != 0 || response.request_id() != requestId)
        {
            _codes->Delete(code.codeId);
            throw Chat2DataException(ErrorCode::USER_NOTIFICATION_FAILED);
        }
        return code.codeId;
    }

    // 使用邮箱、验证码 ID 和验证码登录，验证码在五分钟内有效
    std::string UserBusiness::VcodeLogin(const std::string &email, const std::string &codeId, const std::string &code)
    {
        ValidateEmail(email);
        const auto result = _codes->Consume(codeId, email, code);
        if (result == VerifyCodeResult::Mismatch)
            throw Chat2DataException(ErrorCode::USER_CODE_MISMATCH);
        if (result == VerifyCodeResult::MissingOrExpired)
            throw Chat2DataException(ErrorCode::USER_CODE_MISSING);

        const auto user = _users->FindByEmail(email);
        // 邮箱投递地址必须与注册地址完全一致，避免数据库排序规则折叠不同地址。
        if (!user || user->email != email)
            throw Chat2DataException(ErrorCode::USER_NOT_FOUND);
        return CreateLoginSession(user->userId);
    }

    // 使用会话 ID 登录，若会话不存在或已过期则抛出异常
    void UserBusiness::SessionLogin(const std::string &sessionId)
    {
        RefreshOnlineStatus(RequireSessionOwner(sessionId));
    }

    void UserBusiness::Logout(const std::string &sessionId)
    {
        // 先保留归属，删除语句执行后在同一事务中校正在线状态。
        const auto userId = _sessions->FindOwner(sessionId);
        _sessions->Delete(sessionId, [&](odb::transaction &transaction)
                          {
            if (userId && _users->UpdateStatus(*userId, &transaction))
                _users->DeleteCache(*userId); });
    }

    PublicUserInfo UserBusiness::GetUserInfo(const std::string &sessionId)
    {
        const auto userId = RequireSessionOwner(sessionId);
        auto user = _users->FindCache(userId);
        if (!user)
            user = _users->FindById(userId, true);
        if (!user)
            throw Chat2DataException(ErrorCode::USER_NOT_FOUND);
        return {user->userId, user->nickname, user->email};
    }

    std::optional<std::string> UserBusiness::IsSessionValid(const std::string &sessionId)
    {
        const auto result = CheckSession(sessionId);
        if (result.session)
            return result.session->userId;
        return std::nullopt;
    }

    SessionResult UserBusiness::CheckSession(const std::string &sessionId)
    {
        auto result = _sessions->Find(sessionId);
        // 如果会话已过期，则按归属用户校正在线状态，避免缓存中存在旧数据
        if (result.status == SessionStatus::Expired)
        {
            const auto userId = _sessions->FindOwner(sessionId); // 找到归属用户
            if (userId && _users->UpdateStatus(*userId))
                _users->DeleteCache(*userId); // 更新状态后删除缓存
        }
        return result;
    }

    // 要求会话存在且未过期，返回归属用户 ID；否则抛出异常
    std::string UserBusiness::RequireSessionOwner(const std::string &sessionId)
    {
        const auto result = CheckSession(sessionId);
        if (!result.session)
            throw Chat2DataException(result.status == SessionStatus::Expired ? ErrorCode::USER_SESSION_EXPIRED : ErrorCode::USER_SESSION_NOT_FOUND);
        return result.session->userId;
    }

    std::string UserBusiness::CreateLoginSession(const std::string &userId)
    {
        const auto sessionId = _sessions->Create(userId);
        try
        {
            // 用户登陆成功后刷新在线状态，若失败则撤销会话并抛出异常
            RefreshOnlineStatus(userId);
        }
        catch (...)
        {
            // 状态校正失败时撤销尚未交付的会话，再按剩余会话校正状态。
            _sessions->DiscardUnissued(sessionId);
            _users->UpdateStatus(userId);
            throw;
        }
        return sessionId;
    }

    void UserBusiness::RefreshOnlineStatus(const std::string &userId)
    {
        if (!_users->UpdateStatus(userId))
            throw Chat2DataException(ErrorCode::USER_NOT_FOUND);
        // 更新状态并删除缓存，避免缓存中存在旧数据
        _users->DeleteCache(userId);
    }
}
