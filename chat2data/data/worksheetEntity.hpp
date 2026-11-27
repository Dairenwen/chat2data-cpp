#pragma once

#include <cstdint>
#include <string>

#include <odb/core.hxx>

namespace chat2Data
{
        // 自增主键记录同一工作簿中各工作表映射的写入顺序。
#pragma db object table("tbl_worksheet") options("DEFAULT CHARACTER SET utf8mb4")
        class WorksheetEntity
        {
        public:
#pragma db id auto type("BIGINT UNSIGNED")
                std::uint64_t id = 0;

#pragma db column("fileId") index not_null type("VARCHAR(32)")
                std::string fileId; // 引用文件信息表的业务 ID，随文件记录级联删除。

#pragma db column("worksheetName") not_null type("VARCHAR(32)")
                std::string worksheetName; // 原始工作表名，用于精确定位预览。

#pragma db column("tableName") not_null type("VARCHAR(64)")
                std::string tableName;
        };
}
