#pragma once

#include <arpa/inet.h>
#include <butil/base64.h>
#include <butil/fd_guard.h>
#include <cstdio>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace notify_test
{
    // 在回环地址接收 libcurl 实际发送的 SMTP 数据，每次接收一封邮件。
    class LocalSmtpReceiver
    {
    public:
        LocalSmtpReceiver() : _listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t size = sizeof(address);
            if (_listener < 0 || ::bind(_listener, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
                getsockname(_listener, reinterpret_cast<sockaddr *>(&address), &size) != 0 ||
                listen(_listener, 1) != 0)
                throw std::runtime_error("启动本地 SMTP 接收端失败");
            _port = ntohs(address.sin_port);
        }

        std::string Url() const
        {
            return "smtp://127.0.0.1:" + std::to_string(_port);
        }

        std::string Receive(bool accept_mail)
        {
            // 给连接和读写设置等待上限，以便投递流程异常时测试能够退出。
            pollfd waiting{_listener, POLLIN, 0};
            if (poll(&waiting, 1, 4000) <= 0)
                throw std::runtime_error("等待 SMTP 连接超时");

            butil::fd_guard connection(accept(_listener, nullptr, nullptr));
            const timeval timeout{3, 0};
            if (connection < 0 || setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
                setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
                throw std::runtime_error("无法接收 SMTP 连接");

            std::unique_ptr<std::FILE, decltype(&std::fclose)> input(fdopen(connection, "r"), &std::fclose);
            if (!input)
                throw std::runtime_error("无法创建 SMTP 输入流");
            const int fd = connection.release();
            Reply(fd, "220 localhost SMTP test\r\n");

            char buffer[1024];
            std::string message;
            bool reading_data = false;
            while (std::fgets(buffer, sizeof(buffer), input.get()))
            {
                const std::string_view line(buffer);
                if (reading_data)
                {
                    // 正文传输结束后再返回接收结果，覆盖 SMTP 对整封邮件的确认阶段。
                    if (line == ".\r\n")
                    {
                        Reply(fd, accept_mail ? "250 accepted\r\n" : "550 rejected\r\n");
                        reading_data = false;
                    }
                    else
                    {
                        message += line;
                    }
                }
                else if (line == "DATA\r\n")
                {
                    Reply(fd, "354 end with a dot\r\n");
                    reading_data = true;
                }
                else if (line == "QUIT\r\n")
                {
                    Reply(fd, "221 closing\r\n");
                    break;
                }
                else
                {
                    Reply(fd, "250 OK\r\n");
                }
            }

            return message;
        }

    private:
        static void Reply(int fd, std::string_view text)
        {
            while (!text.empty())
            {
                const auto sent = send(fd, text.data(), text.size(), MSG_NOSIGNAL);
                if (sent <= 0)
                    throw std::runtime_error("无法返回 SMTP 响应");
                text.remove_prefix(sent);
            }
        }

        butil::fd_guard _listener;
        int _port = 0;
    };

    // 从 SMTP 收到的原始邮件解码 MIME 分段，按字节验证实际投递内容。
    inline std::string DecodePart(const std::string &wire, std::string_view header)
    {
        const auto part = wire.find(header);
        const auto headers_end = wire.find("\r\n\r\n", part);
        if (headers_end == std::string::npos)
            throw std::runtime_error("缺少预期邮件分段");

        const auto body_start = headers_end + 4;
        const auto body_end = wire.find("\r\n--", body_start);
        std::string encoded;
        for (const char ch : std::string_view(wire).substr(body_start, body_end - body_start))
        {
            if (ch != '\r' && ch != '\n')
                encoded.push_back(ch);
        }
        std::string decoded;
        if (!butil::Base64Decode(encoded, &decoded))
            throw std::runtime_error("邮件分段的 Base64 内容无效");
        return decoded;
    }
}
