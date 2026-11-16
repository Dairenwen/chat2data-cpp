#pragma once

#include <cstdint>
#include <string>

#include <odb/core.hxx>

namespace chat2Data
{
// 登录会话表的 ODB 映射。
#pragma db object table("tbl_session") options("DEFAULT CHARACTER SET utf8mb4")
    class SessionEntity
    {
    public:
#pragma db id auto type("BIGINT UNSIGNED")
        std::uint64_t id = 0;

#pragma db column("sessionId") unique not_null type("VARCHAR(32)")
        std::string sessionId;

#pragma db column("userId") not_null type("VARCHAR(32)")
        std::string userId;

#pragma db column("expiresAt") not_null type("BIGINT UNSIGNED")
        std::uint64_t expiresAt = 0; // Unix 毫秒
    };
}
