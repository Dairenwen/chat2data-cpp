#pragma once

#include "fileDependencies.hpp"

#include <cstddef>

namespace chat2Data
{
    inline constexpr std::size_t kMaxFileSize = 10 * 1024 * 1024; // 文件子服务的单次附件上限，10 MiB。
    // 文件业务负责归属校验和跨依赖处理，RPC 层负责请求附件及响应字段。
    class FileBusiness
    {
    public:
        /**
         * @brief 保存文件数据访问对象、原件操作和下游调用接口。
         */
        FileBusiness(FileData data, FileStorage storage, std::unique_ptr<FileDownstream> downstream);

        /**
         * @brief 校验 Excel 文件元数据并登记待上传记录。
         * @param user_id 由调用方鉴权得到的用户 ID。
         * @return 第二步上传使用的业务文件 ID。
         * @note 先提交待上传记录，再使对应缓存失效。
         */
        std::string UploadFileInfo(const std::string &user_id, const FileInfo &info);
        /**
         * @brief 保存 Excel 原件，解析工作簿并导入工作表。
         * @param request_id 用于关联 Excel 解析请求和日志。
         * @param file_id 第一步登记返回的文件 ID。
         * @param content 上传的原始文件字节。
         * @note 命名锁覆盖整个上传，文件行锁保护每次状态变更。导入完成后一次提交工作表映射和就绪状态。
         *       清理成功后可在原期限内使用同一文件 ID 重试。
         */
        void UploadFile(const std::string &request_id, const std::string &user_id, const std::string &file_id, const std::string &content);
        /**
         * @brief 检查 SQLite 快照的文件名、大小和文件头，再保存原件。
         * @param filename 含 .db 扩展名的原始文件名。
         * @return 原件和元数据均已就绪的业务文件 ID。
         */
        std::string UploadSQLiteFile(const std::string &user_id, const std::string &filename, const std::string &content);

        /**
         * @brief 获取当前用户的已就绪文件。读取优先使用 Redis，未命中则在文件行锁内回填。
         */
        FileInfoEntity GetFileInfo(const std::string &user_id, const std::string &file_id);
        /**
         * @brief 从 MySQL 取回当前用户的已就绪文件，按完成时间倒序排列。
         */
        std::vector<FileInfoEntity> GetFileList(const std::string &user_id);
        /**
         * @brief 校验文件归属和状态，再从 FastDFS 下载原件。
         * @return 原始文件名和二进制文件内容。
         */
        std::pair<std::string, std::string> DownloadFile(const std::string &user_id, const std::string &file_id);
        /**
         * @brief 向已登记连接的存储子服务提供就绪 SQLite 文件的完整 FastDFS ID。
         */
        std::string GetSQLiteFile(const std::string &user_id, const std::string &file_id);
        /**
         * @brief 查询 Excel 文件的工作表映射，保留工作簿顺序。
         */
        std::vector<WorksheetMapping> GetWorksheetDBTables(const std::string &user_id, const std::string &file_id);
        /**
         * @brief 读取已入库的工作表数据，组合分页信息。
         * @param sheet_name 工作表原名。为空时按工作簿顺序返回各表第一页。
         * @param page 从 1 开始的页码，0 取默认值 1。
         * @param page_size 每页数据行数，0 取默认值 50。
         */
        PreviewExcelResult PreviewExcel(const std::string &user_id, const std::string &file_id, const std::string &sheet_name, int page, int page_size);

        /**
         * @brief 将已就绪 Excel 文件交给 AI 子服务关联聊天会话。
         * @param chat_session_id AI 子服务管理的聊天会话 ID。
         */
        void HandleFileChatSessionMap(const std::string &user_id, const std::string &file_id, const std::string &chat_session_id);
        /**
         * @brief 清理下游资源、FastDFS 原件和文件数据库记录。
         * @note 先提交删除中状态，再清理下游资源。最后在删除记录前和提交删除事务前各清一次缓存。清理失败后保留删除中状态和资源标识，调用方沿用原文件 ID 重试。
         */
        void DeleteFile(const std::string &user_id, const std::string &file_id);

    private:
        using ExcelFile = std::pair<FileInfoEntity, std::vector<WorksheetMapping>>;

        // 查找并锁定记录，调用方在事务内校验状态后使用。
        std::unique_ptr<FileInfoEntity> FindOwned(const std::string &user_id, const std::string &file_id);
        // 在当前文件行锁内回收过期待上传记录，提交后报告登记已过期。
        void ReclaimExpired(const FileInfoEntity &file, odb::transaction &transaction);
        // 合并读取文件和工作表映射缓存，缺项在校验数据库记录后回填。
        ExcelFile GetExcelFile(const std::string &user_id, const std::string &file_id);
        // 处理上传的原件，先上传原件再提交就绪状态。
        void ProcessUpload(const std::string &request_id, FileInfoEntity &file, const std::string &content, const FileOperation &operation);
        // 保存原件并记录定位信息，供解析、导入和失败清理使用。
        void SaveOriginal(FileInfoEntity &file, const std::string &content, const FileOperation &operation);
        // 解析工作簿并导入数据，返回按工作簿顺序排列的工作表映射。
        std::vector<WorksheetMapping> ImportExcel(const std::string &request_id, const FileInfoEntity &file, bool &import_started);
        // 完成上传后提交工作表映射和就绪状态，供后续下载和预览使用。
        void CompleteUpload(const FileInfoEntity &file, const std::vector<WorksheetMapping> &sheets, const FileOperation &operation);
        // 清理失败的上传资源，供调用方沿用原文件 ID 重试。
        void CleanFailedUpload(const FileInfoEntity &file, const FileOperation &operation, bool import_started);
        // 清理缓存
        void ClearCache(const std::string &file_id);

        FileData _data;
        FileStorage _storage;
        std::unique_ptr<FileDownstream> _downstream;
    };
}
