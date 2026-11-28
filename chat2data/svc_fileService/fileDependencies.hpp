#pragma once

#include "excelParseService.pb.h"
#include "fileData.hpp"
#include "fileService.pb.h"

#include <functional>
#include <string_view>

class SvcChannels;

namespace chat2Data
{
    // FastDFS 原件操作由服务进程提供，测试可以注入同样的上传、下载和删除接口。
    struct FileStorage
    {
        // 接收原件字节，成功时返回组名和远端路径组成的完整 FastDFS ID。
        std::function<std::optional<std::string>(const std::string &)> upload;
        // 按完整 FastDFS ID 下载原件，把二进制内容写入第二个参数。
        std::function<bool(const std::string &, std::string &)> download;
        // 原件删除成功或 Storage 确认原件已不存在时返回 true。
        std::function<bool(const std::string &)> remove;
    };

    class FileDownstream
    {
    public:
        virtual ~FileDownstream() = default;

        /**
         * @brief 从解析子服务取得按工作簿顺序排列的数据。
         * @param fdfs_id Excel 原件的完整 FastDFS 文件 ID。
         * @note 先用 GetWorksheets 取得名称，再按该顺序调用 ParseExcel。
         */
        virtual std::vector<WorksheetData> ParseWorkbook(const std::string &request_id, const std::string &fdfs_id) = 0;

        // 存储接口约定按工作簿顺序入库，并逐项返回工作表原名与实际表名。
        // 实际接入写 RPC 时关闭 brpc 自动重试，由文件业务处理失败后的资源清理。
        // 发出导入请求前设置 import_started，超时后调用方据此保留清理上下文。
        virtual std::vector<WorksheetMapping> ImportExcelData(const FileInfoEntity &file, const std::vector<WorksheetData> &sheets, bool &import_started) = 0;

        // 成功表示旧导入已结束且其物理表已清理，表已不存在也算成功。旧导入仍在执行时报告 FILE_BUSY，调用方保留资源定位信息。
        virtual void DropTableExcel(const FileInfoEntity &file) = 0;

        /**
         * @brief 通过实际表名读取指定数据页。
         * @return 按导入顺序排列的列名、数据和总行数，总行数只统计数据行。
         */
        virtual FilePreviewSheet PreviewImportedWorksheet(const FileInfoEntity &file, const std::string &table_name, int page, int page_size) = 0;

        /**
         * @brief 检查所有存储节点上正在建立或已经建立的 SQLite 连接。
         */
        virtual bool HasSQLiteFileConnections(const FileInfoEntity &file) = 0;

        /**
         * @brief 由 AI 子服务在校验会话归属后保存文件关联。
         */
        virtual void UpdateSessionFile(const FileInfoEntity &file, const std::string &chat_session_id) = 0;

        /**
         * @brief 由 AI 子服务清理引用该文件的会话及其消息。
         */
        virtual void DeleteFileChatSessions(const FileInfoEntity &file) = 0;
    };

    FileStorage MakeFastDfsStorage();
    // 解析服务使用真实 RPC，尚未落地的存储和 AI 接口报告依赖错误
    std::unique_ptr<FileDownstream> MakeFileDownstream(std::shared_ptr<SvcChannels> channels, std::string excel_service);
}
