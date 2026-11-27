#pragma once

#include <cstdint>
#include <string>

#include <odb/core.hxx>
#include <odb/nullable.hxx>

namespace chat2Data
{
        // 文件记录保存原件定位信息和上传、删除的处理状态。
#pragma db object table("tbl_fileInfo") options("DEFAULT CHARACTER SET utf8mb4")
        class FileInfoEntity
        {
        public:
#pragma db id auto type("BIGINT UNSIGNED")
                std::uint64_t id = 0;

#pragma db column("fileId") unique not_null type("VARCHAR(32)")
                std::string fileId;

#pragma db column("fileName") not_null type("VARCHAR(64)")
                std::string fileName;

#pragma db column("fileExt") not_null type("VARCHAR(8)")
                std::string fileExt;

#pragma db column("fileSize") not_null type("BIGINT UNSIGNED")
                std::uint64_t fileSize = 0;

#pragma db column("uploadTime") not_null type("BIGINT UNSIGNED")
                std::uint64_t uploadTime = 0; // 就绪前记录登记时刻，就绪后记录完成时刻。

#pragma db column("fdfsFileId") null type("VARCHAR(255)")
                odb::nullable<std::string> fdfsFileId; // 完整 FastDFS 文件 ID，在原件保存后填写。

#pragma db column("userId") not_null type("VARCHAR(32)")
                std::string userId;

#pragma db not_null type("VARCHAR(16)")
                std::string status; // 待上传、上传中、已就绪或删除中。

#pragma db index("file_owner_status_time") members(userId, status, uploadTime)
        };
}
