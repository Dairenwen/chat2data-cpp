#pragma once

#include <odb/core.hxx>

namespace chat2Data
{
    // 原生查询视图，读出 MySQL 文件操作锁的获取和释放结果。
#pragma db view
    struct FileOperationLock
    {
        int result;
    };
}
