#include "fileData.hpp"

#include "BreezeLog.hpp"
#include "BreezeUtil.hpp"
#include "errorHandler.hpp"
#include "fileInfoEntity-odb.hxx"
#include "fileOperationLock-odb.hxx"
#include "utils.hpp"
#include "worksheetEntity-odb.hxx"

#include <odb/database.hxx>
#include <odb/mysql/connection.hxx>
#include <sw/redis++/redis++.h>
#include <utility>

namespace chat2Data
{
    namespace
    {
        constexpr std::uint64_t kFileCacheTtlMs = 3 * 24 * 60 * 60 * 1000; // 文件缓存过期时间，3天
        constexpr std::uint64_t kWorksheetCacheTtlMs = 60 * 60 * 1000;     // 工作表缓存过期时间，1小时

        std::string FileKey(const std::string &id) { return "file:" + id; }
        std::string WorksheetKey(const std::string &id) { return "worksheet:" + id; }

        // 解析 Redis 缓存的 JSON 字符串，返回文件记录对象，缓存不命中返回 std::nullopt
        std::optional<FileInfoEntity> ParseFileCache(const std::string &file_id, const std::optional<std::string> &text)
        {
            if (!text)
                return std::nullopt;

            const auto data = Utils::ParseJsonObject(*text);
            if (data.isNull() || data["fileId"] != file_id || !data["fileName"].isString() ||
                !data["fileExt"].isString() || !data["fileSize"].isUInt64() ||
                !data["uploadTime"].isUInt64() || !data["fdfsFileId"].isString() ||
                !data["userId"].isString() || !data["status"].isString())
                throw Chat2DataException(ErrorCode::FILE_CACHE_FAILED);

            FileInfoEntity file;
            file.fileId = file_id;
            file.fileName = data["fileName"].asString();
            file.fileExt = data["fileExt"].asString();
            file.fileSize = data["fileSize"].asUInt64();
            file.uploadTime = data["uploadTime"].asUInt64();
            file.fdfsFileId = data["fdfsFileId"].asString();
            file.userId = data["userId"].asString();
            file.status = data["status"].asString();
            return file;
        }
    }

    FileOperation::FileOperation(odb::database &database, const std::string &file_id)
        : _database(database), _connection(database.connection()), _key("chat2data:file:" + file_id)
    {
        using Query = odb::query<FileOperationLock>; // 用查询对象来执行 SQL 查询，查询结果用视图表示
        auto transaction = Begin();
        int lock_result;
        try
        {
            // SELECT COALESCE(GET_LOCK(?, 0), -1)，? 绑定 chat2data:file:<fileId>。命名锁归当前连接持有：1 为获取成功，0 为立即返回的竞争结果，-1 为获取异常。
            const auto result = _database.query_value<FileOperationLock>(Query("SELECT COALESCE(GET_LOCK(") + Query::_val(_key) + ", 0), -1)");
            lock_result = result.result;
            // 提交只结束本次事务，命名锁仍由 _connection 持有。
            transaction.commit();
        }
        catch (...)
        {
            // 获取结果不明时关闭连接，由 MySQL 随连接关闭释放锁。
            static_cast<odb::mysql::connection &>(*_connection).mark_failed();
            throw;
        }
        if (lock_result == 0)
            throw Chat2DataException(ErrorCode::FILE_BUSY);
        if (lock_result != 1)
            throw Chat2DataException(ErrorCode::FILE_BUSINESS_FAILED);
    }

    FileOperation::~FileOperation()
    {
        using Query = odb::query<FileOperationLock>;
        try
        {
            auto transaction = Begin();
            // SELECT COALESCE(RELEASE_LOCK(?), 0)，使用取锁时的连接和锁名。
            _database.query_value<FileOperationLock>(Query("SELECT COALESCE(RELEASE_LOCK(") + Query::_val(_key) + "), 0)");
            transaction.commit();
        }
        catch (const std::exception &)
        {
            // 释放失败时关闭连接，避免带锁连接回到连接池。
            static_cast<odb::mysql::connection &>(*_connection).mark_failed();
            WRN("释放文件操作锁失败，锁名 {}", _key);
        }
    }

    odb::transaction FileOperation::Begin() const { return odb::transaction(_connection->begin()); }

    FileData::FileData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis)
        : _database(std::move(database)), _redis(std::move(redis))
    {
    }

    FileOperation FileData::LockOperation(const std::string &file_id) { return FileOperation(*_database, file_id); }

    // 开启 MySQL 事务，commit() 提交，未提交的事务离开作用域时由 ODB 回滚。
    odb::transaction FileData::Begin() const { return odb::transaction(_database->begin()); }

    // INSERT INTO tbl_fileInfo (id, fileId, fileName, fileExt, fileSize, uploadTime, fdfsFileId, userId, status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    void FileData::Save(FileInfoEntity &file) { _database->persist(file); }

    // update 按主键 id 更新实体的全部非主键字段，业务状态校验由调用方在文件行锁内完成。
    // UPDATE tbl_fileInfo SET fileId=?, fileName=?, fileExt=?, fileSize=?, uploadTime=?, fdfsFileId=?, userId=?, status=? WHERE id=?;
    void FileData::Update(const FileInfoEntity &file) { _database->update(file); }

    // DELETE FROM tbl_fileInfo WHERE id=?;tbl_worksheet 的外键引用文件的业务 fileId，MySQL 随文件记录一并删除对应映射。
    void FileData::Delete(const FileInfoEntity &file) { _database->erase(file); }

    std::unique_ptr<FileInfoEntity> FileData::FindForUpdate(const std::string &file_id)
    {
        // SELECT id, fileId, fileName, fileExt, fileSize, uploadTime, fdfsFileId, userId, status FROM tbl_fileInfo WHERE fileId=? FOR UPDATE;
        // 按业务 ID 找记录，取得的文件行锁保持到当前事务结束。
        using Query = odb::query<FileInfoEntity>;
        return std::unique_ptr<FileInfoEntity>(_database->query_one<FileInfoEntity>((Query::fileId == file_id) + "FOR UPDATE"));
    }

    std::vector<FileInfoEntity> FileData::ListReady(const std::string &user_id)
    {
        using Query = odb::query<FileInfoEntity>;
        auto transaction = Begin();
        std::vector<FileInfoEntity> files;
        // SELECT id, fileId, fileName, fileExt, fileSize, uploadTime, fdfsFileId, userId, status FROM tbl_fileInfo WHERE userId=? AND status=? ORDER BY uploadTime DESC, fileId ASC;
        // status 参数绑定 ready。
        const auto query = (Query::userId == user_id && Query::status == kFileReady) + "ORDER BY `uploadTime` DESC, `fileId` ASC";
        for (const auto &file : _database->query<FileInfoEntity>(query))
        {
            // 文件归属按原始字节匹配，和缓存校验保持一致。
            if (file.userId == user_id)
                files.push_back(file);
        }
        transaction.commit();
        return files;
    }

    void FileData::SaveWorksheets(const std::string &file_id, const std::vector<WorksheetMapping> &worksheets)
    {
        // 每张工作表执行一次 INSERT INTO tbl_worksheet (id, fileId, worksheetName, tableName) VALUES (?, ?, ?, ?);
        for (const auto &sheet : worksheets)
        {
            WorksheetEntity row{0, file_id, sheet.worksheet_name, sheet.table_name};
            _database->persist(row);
        }
    }

    std::vector<WorksheetMapping> FileData::FindWorksheets(const std::string &file_id)
    {
        using Query = odb::query<WorksheetEntity>;
        std::vector<WorksheetMapping> sheets;
        // SELECT id, fileId, worksheetName, tableName FROM tbl_worksheet WHERE fileId=? ORDER BY id;
        for (const auto &row : _database->query<WorksheetEntity>((Query::fileId == file_id) + "ORDER BY `id`"))
            sheets.push_back({row.worksheetName, row.tableName});
        return sheets;
    }

    std::optional<FileInfoEntity> FileData::ReadFileCache(const std::string &file_id)
    {
        // HGET file:<fileId> data，返回文件元数据 JSON，空回复表示缓存未命中。
        return ParseFileCache(file_id, _redis->hget(FileKey(file_id), "data"));
    }

    std::pair<std::optional<FileInfoEntity>, std::optional<std::string>> FileData::ReadExcelCache(const std::string &file_id)
    {
        // 将两次 HGET 批量发送，在同一个 Redis 事务中取得元数据和映射。
        // MULTI → HGET file:<fileId> data → HGET worksheet:<fileId> data → EXEC。EXEC 第一项为文件元数据，第二项为有序的工作表映射。各项返回 JSON 字符串或空回复。
        auto transaction = _redis->transaction(true, false); // 开启 Redis 事务，不抛出异常。
        transaction.hget(FileKey(file_id), "data").hget(WorksheetKey(file_id), "data");
        auto replies = transaction.exec();
        return {ParseFileCache(file_id, replies.get<sw::redis::OptionalString>(0)), replies.get<sw::redis::OptionalString>(1)};
    }

    std::vector<WorksheetMapping> FileData::ParseWorksheetCache(const std::string &text)
    {
        const auto value = BreezeUtil::JSON::unserialize(text);
        if (!value || !value->isArray() || value->empty())
            throw Chat2DataException(ErrorCode::FILE_CACHE_FAILED);
        std::vector<WorksheetMapping> sheets;
        for (const auto &row : *value)
        {
            if (!row.isObject() || !row["worksheetName"].isString() || !row["tableName"].isString())
                throw Chat2DataException(ErrorCode::FILE_CACHE_FAILED);
            sheets.push_back({row["worksheetName"].asString(), row["tableName"].asString()});
        }
        return sheets;
    }

    void FileData::SaveFileCache(const FileInfoEntity &file)
    {
        Json::Value value(Json::objectValue);
        value["fileId"] = file.fileId;
        value["fileName"] = file.fileName;
        value["fileExt"] = file.fileExt;
        value["fileSize"] = Json::UInt64(file.fileSize);
        value["uploadTime"] = Json::UInt64(file.uploadTime);
        value["fdfsFileId"] = *file.fdfsFileId;
        value["userId"] = file.userId;
        value["status"] = file.status;
        // Utils::WriteCache 执行 MULTI → HSET file:<fileId> data <JSON> → PEXPIRE file:<fileId> 259200000 → EXEC。
        Utils::WriteCache(*_redis, FileKey(file.fileId), value, kFileCacheTtlMs);
    }

    void FileData::SaveWorksheetCache(const std::string &file_id, const std::vector<WorksheetMapping> &worksheets)
    {
        Json::Value value(Json::arrayValue);
        for (const auto &sheet : worksheets)
        {
            Json::Value row(Json::objectValue);
            row["worksheetName"] = sheet.worksheet_name;
            row["tableName"] = sheet.table_name;
            value.append(std::move(row));
        }
        // Utils::WriteCache 执行 MULTI → HSET worksheet:<fileId> data <JSON数组> → PEXPIRE worksheet:<fileId> 3600000 → EXEC。
        Utils::WriteCache(*_redis, WorksheetKey(file_id), value, kWorksheetCacheTtlMs);
    }

    void FileData::ClearCache(const std::string &file_id)
    {
        // DEL file:<fileId> worksheet:<fileId>，用一次命令移除同一文件的两个键。
        const std::string keys[]{FileKey(file_id), WorksheetKey(file_id)};
        _redis->del(std::begin(keys), std::end(keys));
    }
}
