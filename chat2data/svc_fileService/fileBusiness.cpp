#include "fileBusiness.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"
#include "utils.hpp"

#include <brpc/protocol.h>
#include <sw/redis++/errors.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <utility>

namespace chat2Data
{
    namespace
    {
        constexpr std::uint64_t kRegistrationLifetimeMs = 24 * 60 * 60 * 1000; // 待上传登记的有效期，24小时
        constexpr std::string_view kSQLiteHeader("SQLite format 3\0", 16);     // SQLite 原件的文件头，含终止空字节

        // 依次校验文件是否包含合法扩展名、文件名是否为有效 UTF-8 且长度不超过 64、是否包含路径分隔符、是否含有控制字符，以及扩展名是否与指定值匹配
        void CheckFileName(const std::string &filename, const std::string &extension)
        {
            const auto dot = filename.rfind('.');
            if (dot == std::string::npos || dot == 0 || !Utils::IsValidUtf8Length(filename, 64) ||
                filename.find_first_of("/\\") != std::string::npos ||
                std::any_of(filename.begin(), filename.end(), [](unsigned char ch)
                            { return ch < 0x20 || ch == 0x7f; }) ||
                Utils::ToAsciiLower(std::string_view(filename).substr(dot + 1)) != extension)
                throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
        }

        // 校验文件归属是否匹配指定用户、状态是否为就绪态、传入扩展名时同步校验扩展名一致，同时验证 FastDFS 存储 ID 存在且格式完整
        void CheckReady(const FileInfoEntity &file, const std::string &user_id, const char *extension = nullptr)
        {
            if (file.userId != user_id || file.status != kFileReady ||
                (extension && file.fileExt != extension) ||
                !file.fdfsFileId || !Utils::IsCompleteFastDfsId(*file.fdfsFileId))
                throw Chat2DataException(ErrorCode::FILE_UNAVAILABLE);
        }

        // 先校验当前文件必须处于上传中状态，再校验若当前文件已绑定 FastDFS 存储 ID，则上传文件的存储 ID 必须与之完全一致
        void CheckUploadingSource(const FileInfoEntity &current, const FileInfoEntity &upload)
        {
            if (current.status != kFileUploading)
                throw Chat2DataException(ErrorCode::FILE_UNAVAILABLE);
            if (current.fdfsFileId && (!upload.fdfsFileId || *current.fdfsFileId != *upload.fdfsFileId))
                throw Chat2DataException(ErrorCode::FILE_BUSY);
        }

        // 自动生成安全唯一的文件 ID 与当前上传时间，组装并返回一个完整标准化的文件信息实体
        FileInfoEntity MakeFileRecord(const std::string &user_id, const std::string &filename, const char *extension, std::uint64_t size, const char *status)
        {
            FileInfoEntity file;
            file.fileId = Utils::GenerateSecureId();
            file.fileName = filename;
            file.fileExt = extension;
            file.fileSize = size;
            file.uploadTime = Utils::NowMs();
            file.userId = user_id;
            file.status = status;
            return file;
        }
    }

    FileBusiness::FileBusiness(FileData data, FileStorage storage, std::unique_ptr<FileDownstream> downstream)
        : _data(std::move(data)), _storage(std::move(storage)), _downstream(std::move(downstream))
    {
    }

    std::string FileBusiness::UploadFileInfo(const std::string &user_id, const FileInfo &info)
    {
        if (Utils::ToAsciiLower(info.file_ext()) != "xlsx" || info.file_size() <= 0 || info.file_size() > static_cast<std::int64_t>(kMaxFileSize))
            throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
        CheckFileName(info.file_name(), "xlsx");
        // 第一步只登记业务 ID 和上传要求。原件 ID 留空，等第二步保存原件后填写。
        auto file = MakeFileRecord(user_id, info.file_name(), "xlsx", info.file_size(), kFilePending);
        auto transaction = _data.Begin();
        _data.Save(file);
        transaction.commit(); // 先进行文件信息上传
        ClearCache(file.fileId);
        return file.fileId;
    }

    std::unique_ptr<FileInfoEntity> FileBusiness::FindOwned(const std::string &user_id, const std::string &file_id)
    {
        // 查找并锁定记录，调用方在事务内检查归属和状态
        auto file = _data.FindForUpdate(file_id);
        if (!file || file->userId != user_id || file->fileId != file_id)
            throw Chat2DataException(ErrorCode::FILE_UNAVAILABLE);
        return file;
    }

    void FileBusiness::ReclaimExpired(const FileInfoEntity &file, odb::transaction &transaction)
    {
        // 待上传状态的 uploadTime 是最初登记时间，超过 24 小时未上传原件的记录会被清理，避免占用文件 ID 并阻塞后续上传
        if (file.status == kFilePending && Utils::NowMs() >= file.uploadTime + kRegistrationLifetimeMs)
        {
            // 先删除 MySQL 记录，再清理 Redis 缓存
            _data.Delete(file);
            ClearCache(file.fileId);
            transaction.commit();
            throw Chat2DataException(ErrorCode::FILE_UPLOAD_EXPIRED);
        }
    }

    void FileBusiness::UploadFile(const std::string &request_id, const std::string &user_id, const std::string &file_id, const std::string &content)
    {
        // 文件操作锁跨事务持有，覆盖本次上传以及可能发生的旧资源清理。
        auto operation = _data.LockOperation(file_id);
        // 清理遗留资源后重新读取待上传记录，沿用同一文件 ID 和登记时间。
        for (;;)
        {
            // 先 SELECT ... FOR UPDATE，再检查归属、类型、状态和登记时限。
            auto transaction = operation.Begin();
            auto file = FindOwned(user_id, file_id);
            if (file->fileExt != "xlsx")
                throw Chat2DataException(ErrorCode::FILE_UNAVAILABLE);
            // 文件状态必须是待上传或上传中，已就绪的记录不允许再次上传
            if (file->status == kFileReady)
                throw Chat2DataException(ErrorCode::FILE_ALREADY_UPLOADED);
            if (file->status != kFilePending && file->status != kFileUploading)
                throw Chat2DataException(ErrorCode::FILE_UNAVAILABLE);
            // 待下一次上传时删除MySQL和redis的过期记录，采用的是懒回收策略
            ReclaimExpired(*file, transaction);
            // 附件与登记一致时才允许清理旧上传并开始新的尝试。
            if (content.empty() || file->fileSize != content.size())
                throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
            if (file->status == kFileUploading)
            {
                // 仍然是上传中则可能是上次上传失败或超时，先清理旧资源再尝试新的上传
                transaction.commit();
                // 这里将fastdfsid看作可能发起过导入，先给存储子服务确认
                CleanFailedUpload(*file, operation, static_cast<bool>(file->fdfsFileId));
                continue;
            }
            // 当前行锁覆盖校验和状态写入，完成提交后再访问 FastDFS。
            file->status = kFileUploading;
            _data.Update(*file);
            transaction.commit();
            ProcessUpload(request_id, *file, content, operation);
            return;
        }
    }

    std::string FileBusiness::UploadSQLiteFile(const std::string &user_id, const std::string &filename, const std::string &content)
    {
        CheckFileName(filename, "db");
        if (content.size() < 100 || content.size() > kMaxFileSize || std::string_view(content).substr(0, kSQLiteHeader.size()) != kSQLiteHeader)
            throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);

        auto file = MakeFileRecord(user_id, filename, "db", content.size(), kFileUploading);
        // 先取得操作锁，再登记新文件，使锁获取失败发生在创建记录之前。
        auto operation = _data.LockOperation(file.fileId);
        {
            auto transaction = operation.Begin();
            _data.Save(file); // 先保存文件信息
            transaction.commit();
        }
        // SQLite 复用原件保存和就绪提交流程，省去 Excel 的解析和导入步骤。
        ProcessUpload({}, file, content, operation);
        return file.fileId;
    }

    void FileBusiness::ProcessUpload(const std::string &request_id, FileInfoEntity &file, const std::string &content, const FileOperation &operation)
    {
        bool import_started = false; // 在发起导入请求前设置为true
        bool completion_started = false;
        try
        {
            // 先上传原件
            SaveOriginal(file, content, operation);

            // 如果为xlsx文件则解析工作簿并导入数据，否则直接完成上传。
            const auto sheets = file.fileExt == "xlsx" ? ImportExcel(request_id, file, import_started) : std::vector<WorksheetMapping>{};
            completion_started = true;
            CompleteUpload(file, sheets, operation);
        }
        catch (...)
        {
            // 完成事务的结果可能不明，先确认已提交状态再决定是否回收资源。
            if (completion_started)
            {
                auto transaction = operation.Begin();
                const auto current = FindOwned(file.userId, file.fileId);
                if (current->status == kFileReady)
                    throw;
                CheckUploadingSource(*current, file);
                transaction.commit();
            }
            CleanFailedUpload(file, operation, import_started);
            throw;
        }
        // 文件上传成功后清理缓存
        ClearCache(file.fileId);
    }

    void FileBusiness::SaveOriginal(FileInfoEntity &file, const std::string &content, const FileOperation &operation)
    {
        auto remote_id = _storage.upload(content); // 上传原件到 FastDFS，返回完整的组名和远端路径组成的 FastDFS ID
        if (!remote_id || remote_id->size() > 255 || !Utils::IsCompleteFastDfsId(*remote_id))
            throw Chat2DataException(ErrorCode::FILE_STORAGE_FAILED);

        // 先保留内存中的原件 ID，数据库写入失败时仍可清理本次上传。
        file.fdfsFileId = std::move(*remote_id);
        auto transaction = operation.Begin();
        const auto current = FindOwned(file.userId, file.fileId);
        CheckUploadingSource(*current, file);
        // 保留登记时刻，只修改实体中的原件定位字段，再由 ODB 按主键更新该行。
        current->fdfsFileId = file.fdfsFileId;
        _data.Update(*current); // 将fastdfs的文件id写入数据库
        transaction.commit();
    }

    std::vector<WorksheetMapping> FileBusiness::ImportExcel(const std::string &request_id, const FileInfoEntity &file, bool &import_started)
    {
        // 1.先解析工作簿，返回按工作簿顺序排列的工作表信息。
        const auto sheets = _downstream->ParseWorkbook(request_id, *file.fdfsFileId);
        if (sheets.empty())
            throw Chat2DataException(ErrorCode::FILE_PARSE_FAILED);
        // 2.再将工作表数据导入数据库，返回按工作簿顺序排列的工作表映射。
        auto mappings = _downstream->ImportExcelData(file, sheets, import_started);
        if (mappings.size() != sheets.size())
            throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
        // 3. 校验映射的工作表名与解析结果一致，且工作表名和表名均为有效 UTF-8 且长度不超过限制，表名不重复。
        std::unordered_set<std::string> tables;
        for (std::size_t i = 0; i < mappings.size(); ++i)
        {
            const auto &mapping = mappings[i];
            if (mapping.worksheet_name != sheets[i].name() || mapping.worksheet_name.empty() ||
                !Utils::IsValidUtf8Length(mapping.worksheet_name, 32) ||
                mapping.table_name.empty() ||
                !Utils::IsValidUtf8Length(mapping.table_name, 64) ||
                !tables.insert(mapping.table_name).second)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
        }
        INF("Excel 导入完成，请求 ID {}，文件 ID {}，工作表数量 {}", request_id, file.fileId, mappings.size());
        return mappings;
    }

    void FileBusiness::CompleteUpload(const FileInfoEntity &file, const std::vector<WorksheetMapping> &sheets, const FileOperation &operation)
    {
        auto transaction = operation.Begin();
        const auto current = FindOwned(file.userId, file.fileId);
        CheckUploadingSource(*current, file);
        if (!current->fdfsFileId)
            throw Chat2DataException(ErrorCode::FILE_STORAGE_FAILED);
        // 映射和就绪状态由同一事务提交，查询只会看到完整的工作表映射。
        _data.SaveWorksheets(file.fileId, sheets); // db文件无需保存工作表映射
        current->status = kFileReady;
        current->uploadTime = Utils::NowMs();
        _data.Update(*current);
        transaction.commit();
    }

    void FileBusiness::CleanFailedUpload(const FileInfoEntity &file, const FileOperation &operation, bool import_started)
    {
        // 若导入已经开始需要清理下游表，否则只清理原件
        if (import_started)
            _downstream->DropTableExcel(file);
        if (file.fdfsFileId && !_storage.remove(*file.fdfsFileId))
        {
            ERR("上传失败后的原件清理失败，文件 ID {}，FastDFS ID {}", file.fileId, *file.fdfsFileId);
            throw Chat2DataException(ErrorCode::FILE_STORAGE_FAILED);
        }

        // 下游表和原件清理完成后，若为xlsx文件则更新状态为待上传，供下一次上传尝试。若为db文件则直接删除记录。
        auto transaction = operation.Begin();
        const auto current = FindOwned(file.userId, file.fileId);
        CheckUploadingSource(*current, file);
        if (file.fileExt == "xlsx")
        {
            current->status = kFilePending;
            current->fdfsFileId.reset();
            _data.Update(*current);
        }
        else
        {
            _data.Delete(*current);
        }
        ClearCache(file.fileId); // 删除缓存
        transaction.commit();
    }

    FileInfoEntity FileBusiness::GetFileInfo(const std::string &user_id, const std::string &file_id)
    {
        // HGET 命中后直接校验缓存内的归属和状态，通过后返回。
        if (auto cached = _data.ReadFileCache(file_id))
        {
            CheckReady(*cached, user_id);
            return std::move(*cached);
        }
        // 如果缓存未命中则在文件行锁内查找记录，校验归属和状态后回填缓存。
        auto transaction = _data.Begin();
        const auto file = FindOwned(user_id, file_id);
        // 在查询文件信息的时候也会回收过期待上传的记录，避免占用文件 ID 并阻塞后续上传
        ReclaimExpired(*file, transaction);
        CheckReady(*file, user_id);
        // 回填在文件行锁内完成，与删除中状态的缓存清理保持顺序。
        _data.SaveFileCache(*file);
        transaction.commit();
        return std::move(*file);
    }

    FileBusiness::ExcelFile FileBusiness::GetExcelFile(const std::string &user_id, const std::string &file_id)
    {
        // 先尝试从缓存读取文件和工作表映射，缺项时再在文件行锁内查找记录并回填缓存。
        auto [cached_file, cached_worksheets] = _data.ReadExcelCache(file_id);
        if (cached_file)
        {
            CheckReady(*cached_file, user_id, "xlsx");
            if (cached_worksheets)
                return {std::move(*cached_file), _data.ParseWorksheetCache(*cached_worksheets)};
        }
        // 缓存缺失时在文件行锁内查找记录，校验归属和状态后回填缓存。
        auto transaction = _data.Begin();
        const auto file = FindOwned(user_id, file_id);
        ReclaimExpired(*file, transaction);
        CheckReady(*file, user_id, "xlsx");
        // 任一缓存缺失时先用文件行锁校验数据库记录。映射缺失时按 id 升序查表。两个键各自判断是否需要回填，回填完成后提交事务并释放行锁。
        auto sheets = cached_worksheets ? _data.ParseWorksheetCache(*cached_worksheets) : _data.FindWorksheets(file_id);
        if (!cached_file)
            _data.SaveFileCache(*file);
        if (!cached_worksheets)
            _data.SaveWorksheetCache(file_id, sheets);
        transaction.commit();
        return {std::move(*file), std::move(sheets)};
    }

    std::vector<FileInfoEntity> FileBusiness::GetFileList(const std::string &user_id)
    {
        return _data.ListReady(user_id); // 直接返回数据库查询结果，缓存不参与列表查询
    }

    std::pair<std::string, std::string> FileBusiness::DownloadFile(const std::string &user_id, const std::string &file_id)
    {
        auto file = GetFileInfo(user_id, file_id);
        std::string content;
        // FastDFS 原件的实际大小应与已就绪记录一致
        if (!_storage.download(*file.fdfsFileId, content) || content.size() != file.fileSize)
            throw Chat2DataException(ErrorCode::FILE_STORAGE_FAILED);
        return {std::move(file.fileName), std::move(content)};
    }

    std::string FileBusiness::GetSQLiteFile(const std::string &user_id, const std::string &file_id)
    {
        const auto file = GetFileInfo(user_id, file_id);
        CheckReady(file, user_id, "db");
        return *file.fdfsFileId; // 返回完整的组名和远端路径组成的 FastDFS ID
    }

    std::vector<WorksheetMapping> FileBusiness::GetWorksheetDBTables(const std::string &user_id, const std::string &file_id)
    {
        return GetExcelFile(user_id, file_id).second;
    }

    PreviewExcelResult FileBusiness::PreviewExcel(const std::string &user_id, const std::string &file_id, const std::string &sheet_name, int page, int page_size)
    {
        if (page < 0 || page_size < 0 || page_size > 100)
            throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
        page = page == 0 || sheet_name.empty() ? 1 : page; // 默认为第一页；
        page_size = page_size == 0 ? 50 : page_size;       // 默认每页50行
        const auto [file, mappings] = GetExcelFile(user_id, file_id);
        PreviewExcelResult result;
        result.set_file_id(file.fileId);
        result.set_file_name(file.fileName);
        result.set_file_size(file.fileSize);
        result.set_file_ext(file.fileExt);

        auto *excel = result.mutable_excel_data(); // 获取可变的 Excel 数据对象
        std::size_t response_size = result.ByteSizeLong();
        // 预览由存储查询入库后的实际行数，分页对原始工作表名称精确匹配。
        for (const auto &mapping : mappings)
        {
            // 遍历所有工作表映射，若指定了 sheet_name 则只预览匹配的工作表，否则预览全部工作表
            if (!sheet_name.empty() && mapping.worksheet_name != sheet_name)
                continue;
            auto sheet = _downstream->PreviewImportedWorksheet(file, mapping.table_name, page, page_size);
            const auto rows = sheet.total_rows();
            if (rows < 0)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            const auto pages = std::max<std::int64_t>(1, rows / page_size + (rows % page_size != 0));
            if (page > pages)
                throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);

            // 存储返回的分页数据与列顺序共同决定每行单元格的位置。
            const auto offset = static_cast<std::int64_t>(page - 1) * page_size;
            const auto expected_rows = std::min<std::int64_t>(rows - offset, page_size);
            if (sheet.data_size() != expected_rows || sheet.columns().empty())
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            for (const auto &row : sheet.data())
            {
                if (row.cells_size() != sheet.columns_size())
                    throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            }

            sheet.set_name(mapping.worksheet_name);
            sheet.set_current_page(page);
            sheet.set_page_size(page_size);
            sheet.set_total_pages(pages);
            sheet.set_col_count(sheet.columns_size());
            response_size += sheet.ByteSizeLong();
            if (response_size > brpc::FLAGS_max_body_size)
                throw Chat2DataException(ErrorCode::FILE_PREVIEW_TOO_LARGE);
            excel->add_sheets()->Swap(&sheet); // 追加工作表数据到预览结果
        }
        if (excel->sheets().empty())
            throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
        return result;
    }

    void FileBusiness::HandleFileChatSessionMap(const std::string &user_id, const std::string &file_id, const std::string &chat_session_id)
    {
        auto operation = _data.LockOperation(file_id);
        auto transaction = operation.Begin();
        const auto file = FindOwned(user_id, file_id);
        // 再来清理过期的待上传记录，避免占用文件 ID 并阻塞后续上传
        ReclaimExpired(*file, transaction);
        CheckReady(*file, user_id, "xlsx"); // 再来检查归属和状态，确保文件可用
        transaction.commit();

        // 先提交本地事务释放文件行锁。按下游约定，AI 写关联时在自己的事务内校验会话归属并锁定同一行。
        _downstream->UpdateSessionFile(*file, chat_session_id);
    }

    void FileBusiness::ClearCache(const std::string &file_id)
    {
        // 每次 DEL 同时删除文件和映射缓存。Redis 失败时再试一次，由调用方决定事务是否提交。
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            try
            {
                _data.ClearCache(file_id);
                return;
            }
            catch (const sw::redis::Error &)
            {
                WRN("清理文件缓存失败，文件 ID {}，尝试次数 {}", file_id, attempt + 1);
            }
        }
        throw Chat2DataException(ErrorCode::FILE_CACHE_FAILED);
    }

    void FileBusiness::DeleteFile(const std::string &user_id, const std::string &file_id)
    {
        // 1. 首次删除在行锁内写入删除中状态并清缓存，提交后进入资源清理。重试沿用删除中状态。
        auto operation = _data.LockOperation(file_id);
        auto transaction = operation.Begin();
        auto file = FindOwned(user_id, file_id);
        ReclaimExpired(*file, transaction);
        // 如果还在上传中则拒绝删除，避免清理正在上传的原件
        if (file->status == kFileUploading)
            throw Chat2DataException(ErrorCode::FILE_BUSY);
        if (file->status != kFileDeleting)
        {
            CheckReady(*file, user_id);
            // 持有文件行锁时清缓存，防止读请求回填已有就绪状态。
            file->status = kFileDeleting;
            _data.Update(*file);
            ClearCache(file_id);
        }
        transaction.commit();

        // 2. Excel 依次清理聊天会话和工作表，SQLite 先检查连接占用。通过后再删除 FastDFS 原件。
        if (file->fileExt == "xlsx")
        {
            _downstream->DeleteFileChatSessions(*file);
            _downstream->DropTableExcel(*file);
        }
        else if (_downstream->HasSQLiteFileConnections(*file))
        {
            // SQLite 仍被连接使用，恢复就绪后允许用户断开连接再重试。
            auto restore = operation.Begin();
            file = FindOwned(user_id, file_id);
            file->status = kFileReady;
            _data.Update(*file);
            ClearCache(file_id);
            restore.commit();
            throw Chat2DataException(ErrorCode::FILE_SQLITE_IN_USE);
        }
        // 删除 FastDFS 原件
        if (!_storage.remove(*file->fdfsFileId))
            throw Chat2DataException(ErrorCode::FILE_STORAGE_FAILED);

        // 3. DEL → 开启事务并 DELETE 文件记录 → 等待 500 毫秒 → 再次 DEL → 提交
        ClearCache(file_id);
        auto removal = operation.Begin();
        file = FindOwned(user_id, file_id);
        _data.Delete(*file);
        // 与用户服务一致，在事务提交前完成第二次清除，失败时回滚文件记录及映射
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ClearCache(file_id);
        removal.commit();
    }
}
