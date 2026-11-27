#pragma once

#include <cstdint>
#include <string>

#include <odb/core.hxx>

namespace chat2Data
{
// 用户信息表的 ODB 映射。
#pragma db object table("tbl_user") options("DEFAULT CHARACTER SET utf8mb4")
        class UserEntity
        {
        public:
#pragma db id auto type("BIGINT UNSIGNED")
                std::uint64_t id = 0;

#pragma db column("userId") unique type("VARCHAR(32)")
                std::string userId;

#pragma db unique type("VARCHAR(32)")
                std::string nickname;

#pragma db unique type("VARCHAR(255)")
                std::string email;

#pragma db type("TEXT")
                std::string password; // 带盐的密码哈希

#pragma db type("INT")
                int status = 0; // 用户状态，默认为0
        };
}
