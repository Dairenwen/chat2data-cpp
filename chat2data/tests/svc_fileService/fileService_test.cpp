#include "BreezeRPC.hpp"

#include "BreezeFDFS.hpp"
#include "BreezeODB.hpp"
#include "BreezeRedis.hpp"
#include "errorHandler.hpp"
#include "excelParserServiceImpl.hpp"
#include "fileServiceImpl.hpp"
#include "utils.hpp"

#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <odb/mysql/exceptions.hxx>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>

DECLARE_string(mysql_host);
DECLARE_uint32(mysql_port);
DECLARE_string(mysql_user);
DECLARE_string(mysql_password);
DECLARE_string(redis_host);
DECLARE_int32(redis_port);
DECLARE_string(redis_user);
DECLARE_string(redis_password);

namespace
{
    using namespace chat2Data;

    constexpr char kOwner[] = "file-test-owner";
    constexpr char kWorkbook[] = "Excel test bytes";

    // 按文件记录模拟入库的工作表和会话关联，预览从同一份导入数据读取。
    class DownstreamStub final : public FileDownstream
    {
    public:
        explicit DownstreamStub(std::unique_ptr<FileDownstream> parser = nullptr)
            : _parser(std::move(parser))
        {
        }

        std::vector<WorksheetData> ParseWorkbook(const std::string &request_id, const std::string &fdfs_id) override
        {
            if (_parser)
                return _parser->ParseWorkbook(request_id, fdfs_id);

            std::vector<WorksheetData> sheets(2);
            sheets[0].set_name("汇总");
            sheets[1].set_name("明细");
            for (auto &sheet : sheets)
            {
                auto *column = sheet.add_columns();
                column->set_name("金额");
                column->set_type("BIGINT");
                sheet.set_total_cols(1);
            }
            for (int value = 1; value <= 3; ++value)
            {
                auto *cell = sheets[0].add_rows()->add_cells();
                cell->set_value(std::to_string(value));
                cell->set_type("Integer");
            }
            for (auto &sheet : sheets)
                sheet.set_total_rows(sheet.rows_size() + 1);
            return sheets;
        }

        std::vector<WorksheetMapping> ImportExcelData(const FileInfoEntity &file,
                                                      const std::vector<WorksheetData> &sheets,
                                                      bool &import_started) override
        {
            EXPECT_EQ(file.status, kFileUploading);
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_FALSE(file.fileId.empty());
            EXPECT_TRUE(file.fdfsFileId);
            import_started = true;
            if (before_import)
                before_import(file);

            std::vector<WorksheetMapping> mappings;
            for (std::size_t i = 0; i < sheets.size(); ++i)
                mappings.push_back({sheets[i].name(), "excel_" + std::to_string(file.id) + "_" + std::to_string(i + 1)});

            std::lock_guard<std::mutex> guard(_mutex);
            EXPECT_EQ(_imports.count(file.fileId), 0U);
            _imports.emplace(file.fileId, ImportedFile{*file.fdfsFileId, sheets, mappings});
            // 模拟下游已有写入，但调用方收到导入失败。
            if (fail_import)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            return mappings;
        }

        void DropTableExcel(const FileInfoEntity &file) override
        {
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_TRUE(file.fdfsFileId);
            if (drop_error != ErrorCode::SUCCESS)
                throw Chat2DataException(drop_error);

            std::lock_guard<std::mutex> guard(_mutex);
            const auto found = _imports.find(file.fileId);
            if (found != _imports.end())
            {
                EXPECT_EQ(found->second.fdfs_id, *file.fdfsFileId);
                _imports.erase(found);
            }
        }

        FilePreviewSheet PreviewImportedWorksheet(const FileInfoEntity &file, const std::string &table,
                                                  int page, int page_size) override
        {
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_EQ(file.status, kFileReady);
            std::lock_guard<std::mutex> guard(_mutex);
            const auto &import = _imports.at(file.fileId);
            EXPECT_EQ(import.fdfs_id, *file.fdfsFileId);
            const auto mapping = std::find_if(import.mappings.begin(), import.mappings.end(),
                                              [&](const WorksheetMapping &sheet) { return sheet.table_name == table; });
            if (mapping == import.mappings.end())
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);

            const auto &source = import.sheets[mapping - import.mappings.begin()];
            FilePreviewSheet sheet;
            for (const auto &column : source.columns())
                sheet.add_columns(column.name());
            sheet.set_total_rows(source.rows_size());
            const auto start = static_cast<std::int64_t>(page - 1) * page_size;
            const auto end = std::min<std::int64_t>(start + page_size, source.rows_size());
            for (auto i = start; i < end; ++i)
            {
                auto *row = sheet.add_data();
                for (const auto &cell : source.rows(static_cast<int>(i)).cells())
                    row->add_cells(cell.value());
            }
            return sheet;
        }

        bool HasSQLiteFileConnections(const FileInfoEntity &file) override
        {
            EXPECT_EQ(file.status, kFileDeleting);
            EXPECT_EQ(file.fileExt, "db");
            return sqlite_in_use;
        }

        void UpdateSessionFile(const FileInfoEntity &file, const std::string &session) override
        {
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_EQ(file.status, kFileReady);
            std::lock_guard<std::mutex> guard(_mutex);
            _sessions[file.fileId].insert(session);
        }

        void DeleteFileChatSessions(const FileInfoEntity &file) override
        {
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_EQ(file.status, kFileDeleting);
            std::lock_guard<std::mutex> guard(_mutex);
            _sessions.erase(file.fileId);
        }

        std::size_t SessionCount(const std::string &file_id)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            const auto found = _sessions.find(file_id);
            return found == _sessions.end() ? 0 : found->second.size();
        }

        bool HasImportedFile(const std::string &file_id)
        {
            std::lock_guard<std::mutex> guard(_mutex);
            return _imports.count(file_id) != 0;
        }

        // 测试在发请求前设置故障和同步点，所有在途请求结束后再修改。
        std::function<void(const FileInfoEntity &)> before_import;
        bool fail_import = false;
        ErrorCode drop_error = ErrorCode::SUCCESS;
        bool sqlite_in_use = false;

    private:
        struct ImportedFile
        {
            std::string fdfs_id;
            std::vector<WorksheetData> sheets;
            std::vector<WorksheetMapping> mappings;
        };

        std::unique_ptr<FileDownstream> _parser;
        std::mutex _mutex;
        std::map<std::string, ImportedFile> _imports;
        std::map<std::string, std::set<std::string>> _sessions;
    };

    template <typename Request>
    void SetIdentity(Request &request, const std::string &user = kOwner)
    {
        request.set_request_id("file-test");
        request.set_session_id("file-test-session");
        request.set_user_id(user);
    }

    template <typename Run>
    void ExpectError(ErrorCode code, Run run)
    {
        try
        {
            run();
            ADD_FAILURE() << "应返回业务错误 " << static_cast<int>(code);
        }
        catch (const Chat2DataException &error)
        {
            EXPECT_EQ(error.Code(), code);
        }
    }

    class FileServiceTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // 外键要求使用普通表，每个测试建立独立数据库并只清理本次创建的数据。
            schema = "file_test_" + Utils::GenerateSecureId();
            BreezeODB::mysql_settings mysql;
            mysql.host = FLAGS_mysql_host;
            mysql.port = FLAGS_mysql_port;
            mysql.user = FLAGS_mysql_user;
            mysql.passwd = FLAGS_mysql_password;
            mysql.cset = "utf8mb4";
            admin = BreezeODB::DBFactory::mysql(mysql);
            Execute(*admin, "CREATE DATABASE `" + schema + "` DEFAULT CHARACTER SET utf8mb4");
            created = true;
            mysql.db = schema;
            database = BreezeODB::DBFactory::mysql(mysql);
            std::ifstream sql(FILE_SCHEMA);
            ASSERT_TRUE(sql.good());
            // 只执行以分号结束的建表语句，ODB 生成的末尾注释由此跳过。
            for (std::string statement; std::getline(sql, statement, ';') && !sql.eof();)
                if (statement.find_first_not_of(" \t\r\n") != std::string::npos)
                    Execute(*database, statement);

            redis_settings.host = FLAGS_redis_host;
            redis_settings.port = FLAGS_redis_port;
            redis_settings.user = FLAGS_redis_user;
            redis_settings.passwd = FLAGS_redis_password;
            redis = Breezeredis::RedisFactory::create(redis_settings);

            auto dependency = std::make_unique<DownstreamStub>();
            downstream = dependency.get();
            business = std::make_unique<FileBusiness>(
                FileData(database, redis), MakeStorage(), std::move(dependency));
        }

        void TearDown() override
        {
            business.reset();
            if (redis)
            {
                for (const auto &id : file_ids)
                    FileData(database, redis).ClearCache(id);
                if (!acl_user.empty())
                    redis->command<long long>("ACL", "DELUSER", acl_user);
            }
            database.reset();
            if (created)
                Execute(*admin, "DROP DATABASE `" + schema + "`");
        }

        static void Execute(odb::database &db, const std::string &sql)
        {
            odb::transaction transaction(db.begin());
            db.execute(sql);
            transaction.commit();
        }

        FileStorage MakeStorage()
        {
            return {
                [this](const std::string &bytes) -> std::optional<std::string>
                {
                    auto id = "group1/M00/" + Utils::GenerateSecureId();
                    std::lock_guard<std::mutex> guard(originals_mutex);
                    originals.emplace(id, bytes);
                    return id;
                },
                [this](const std::string &id, std::string &bytes)
                {
                    std::lock_guard<std::mutex> guard(originals_mutex);
                    const auto found = originals.find(id);
                    if (found == originals.end())
                        return false;
                    bytes = found->second;
                    return true;
                },
                [this](const std::string &id)
                {
                    std::lock_guard<std::mutex> guard(originals_mutex);
                    originals.erase(id);
                    return true;
                }};
        }

        static FileInfo WorkbookInfo(const std::string &filename)
        {
            FileInfo info;
            info.set_file_name(filename);
            info.set_file_ext("xlsx");
            info.set_file_size(sizeof(kWorkbook) - 1);
            return info;
        }

        std::string RegisterExcel()
        {
            auto id = business->UploadFileInfo(kOwner, WorkbookInfo("年度报表.xlsx"));
            file_ids.push_back(id);
            return id;
        }

        std::unique_ptr<FileInfoEntity> ReadRecord(const std::string &id)
        {
            FileData data(database, redis);
            auto transaction = data.Begin();
            return data.FindForUpdate(id);
        }

        std::string schema;
        std::string acl_user;
        bool created = false;
        Breezeredis::redis_settings redis_settings;
        std::shared_ptr<odb::database> admin;
        std::shared_ptr<odb::database> database;
        std::shared_ptr<sw::redis::Redis> redis;
        std::vector<std::string> file_ids;
        std::mutex originals_mutex;
        std::map<std::string, std::string> originals;
        DownstreamStub *downstream = nullptr;
        std::unique_ptr<FileBusiness> business;
    };

    // 已入库的工作表按原顺序展示，删除时同时清理会话、映射和原件。
    TEST_F(FileServiceTest, UploadPreviewAssociateAndDeleteExcel)
    {
        const auto id = RegisterExcel();
        EXPECT_TRUE(business->GetFileList(kOwner).empty());

        business->UploadFile("upload", kOwner, id, kWorkbook);
        ExpectError(ErrorCode::FILE_ALREADY_UPLOADED, [&]
                    { business->UploadFile("upload", kOwner, id, kWorkbook); });
        ASSERT_EQ(business->GetFileList(kOwner).size(), 1U);
        EXPECT_EQ(business->DownloadFile(kOwner, id), std::make_pair(std::string("年度报表.xlsx"), std::string(kWorkbook)));

        const auto first = business->PreviewExcel(kOwner, id, "", 0, 0);
        ASSERT_EQ(first.excel_data().sheets_size(), 2);
        EXPECT_EQ(first.excel_data().sheets(0).name(), "汇总");
        EXPECT_EQ(first.excel_data().sheets(1).name(), "明细");
        EXPECT_EQ(first.excel_data().sheets(1).total_pages(), 1);
        const auto second = business->PreviewExcel(kOwner, id, "汇总", 2, 2);
        EXPECT_EQ(second.excel_data().sheets(0).data(0).cells(0), "3");
        ExpectError(ErrorCode::FILE_INVALID_INPUT, [&]
                    { business->PreviewExcel(kOwner, id, "汇总", 3, 2); });

        // 缓存命中时仍校验文件归属和状态。
        const auto mappings = business->GetWorksheetDBTables(kOwner, id);
        ASSERT_EQ(mappings.size(), 2U);
        const auto file_row = ReadRecord(id);
        ASSERT_TRUE(file_row);
        const auto table_prefix = "excel_" + std::to_string(file_row->id) + "_";
        EXPECT_EQ(mappings[0].table_name, table_prefix + "1");
        EXPECT_EQ(mappings[1].table_name, table_prefix + "2");
        EXPECT_GT(redis->pttl("file:" + id), 0);
        EXPECT_GT(redis->pttl("worksheet:" + id), 0);
        ExpectError(ErrorCode::FILE_UNAVAILABLE, [&]
                    { business->GetWorksheetDBTables("other-user", id); });
        auto file = business->GetFileInfo(kOwner, id);
        file.status = kFileDeleting;
        FileData(database, redis).SaveFileCache(file);
        ExpectError(ErrorCode::FILE_UNAVAILABLE, [&]
                    { business->GetFileInfo(kOwner, id); });
        ExpectError(ErrorCode::FILE_UNAVAILABLE, [&]
                    { business->GetWorksheetDBTables(kOwner, id); });

        business->HandleFileChatSessionMap(kOwner, id, "chat-1");
        business->HandleFileChatSessionMap(kOwner, id, "chat-2");
        business->HandleFileChatSessionMap(kOwner, id, "chat-1");
        EXPECT_EQ(downstream->SessionCount(id), 2U);
        business->DeleteFile(kOwner, id);
        EXPECT_EQ(downstream->SessionCount(id), 0U);
        EXPECT_FALSE(downstream->HasImportedFile(id));
        EXPECT_TRUE(originals.empty());
        EXPECT_FALSE(ReadRecord(id));
        EXPECT_EQ(redis->exists("file:" + id), 0);
        EXPECT_EQ(redis->exists("worksheet:" + id), 0);
        FileData data(database, redis);
        auto transaction = data.Begin();
        EXPECT_TRUE(data.FindWorksheets(id).empty());
    }

    // 导入或清理失败时保留原登记时刻，清理成功后继续使用同一文件 ID。
    TEST_F(FileServiceTest, RetryExcelWithOriginalIdAfterImportFailure)
    {
        const auto id = RegisterExcel();
        const auto registered_at = ReadRecord(id)->uploadTime;
        downstream->fail_import = true;
        downstream->drop_error = ErrorCode::FILE_BUSY;
        ExpectError(ErrorCode::FILE_BUSY, [&]
                    { business->UploadFile("first", kOwner, id, kWorkbook); });
        EXPECT_TRUE(downstream->HasImportedFile(id));
        const auto failed = ReadRecord(id);
        ASSERT_TRUE(failed);
        EXPECT_EQ(failed->status, kFileUploading);
        ASSERT_TRUE(failed->fdfsFileId);
        EXPECT_EQ(failed->uploadTime, registered_at);
        ASSERT_EQ(originals.size(), 1U);
        EXPECT_TRUE(business->GetFileList(kOwner).empty());

        downstream->drop_error = ErrorCode::SUCCESS;
        // 附件大小有误时继续保留上一次的原件和上传中状态。
        ExpectError(ErrorCode::FILE_INVALID_INPUT, [&]
                    { business->UploadFile("wrong-size", kOwner, id, "short"); });
        const auto unchanged = ReadRecord(id);
        ASSERT_TRUE(unchanged);
        EXPECT_EQ(unchanged->status, kFileUploading);
        ASSERT_TRUE(unchanged->fdfsFileId);
        EXPECT_EQ(*unchanged->fdfsFileId, *failed->fdfsFileId);
        EXPECT_EQ(originals.size(), 1U);

        ExpectError(ErrorCode::FILE_DEPENDENCY_FAILED, [&]
                    { business->UploadFile("second", kOwner, id, kWorkbook); });
        const auto pending = ReadRecord(id);
        ASSERT_TRUE(pending);
        EXPECT_EQ(pending->status, kFilePending);
        EXPECT_EQ(pending->uploadTime, registered_at);
        EXPECT_FALSE(pending->fdfsFileId);
        EXPECT_FALSE(downstream->HasImportedFile(id));
        EXPECT_TRUE(originals.empty());

        downstream->fail_import = false;
        business->UploadFile("third", kOwner, id, kWorkbook);
        EXPECT_EQ(business->GetFileInfo(kOwner, id).fileId, id);
    }

    // RPC 使用附件传输 SQLite 字节，文件占用检查由存储替身提供。
    TEST_F(FileServiceTest, UploadAndDownloadSQLiteUsingRpcAttachments)
    {
        auto dependency = std::make_unique<DownstreamStub>();
        auto *connections = dependency.get();
        FileServiceImpl service(FileBusiness(FileData(database, redis), MakeStorage(), std::move(dependency)));
        UploadSQLiteFileRequest upload;
        SetIdentity(upload);
        upload.set_filename("sales.db");
        std::string bytes(4096, '\0');
        bytes.replace(0, 15, "SQLite format 3");
        brpc::Controller upload_controller;
        upload_controller.request_attachment().append(bytes);
        UploadSQLiteFileResponse uploaded;
        service.UploadSQLiteFile(&upload_controller, &upload, &uploaded, nullptr);
        ASSERT_EQ(uploaded.error_code(), 0);
        EXPECT_EQ(uploaded.request_id(), upload.request_id());
        const auto id = uploaded.result().file_id();
        file_ids.push_back(id);

        GetSQLiteFileRequest location;
        SetIdentity(location);
        location.set_file_id(id);
        GetSQLiteFileResponse located;
        service.GetSQLiteFile(nullptr, &location, &located, nullptr);
        ASSERT_EQ(located.error_code(), 0);
        EXPECT_EQ(originals.at(located.result().fdfs_file_id()), bytes);
        // SQLite 保存的是整个原件，文件记录下没有工作表映射。
        {
            FileData data(database, redis);
            auto transaction = data.Begin();
            EXPECT_TRUE(data.FindWorksheets(id).empty());
        }

        DownloadFileRequest download;
        SetIdentity(download);
        download.set_file_id(id);
        brpc::Controller download_controller;
        DownloadFileResponse downloaded;
        service.DownloadFile(&download_controller, &download, &downloaded, nullptr);
        EXPECT_EQ(downloaded.error_code(), 0);
        EXPECT_EQ(downloaded.file_name(), "sales.db");
        EXPECT_EQ(download_controller.response_attachment().to_string(), bytes);

        // 原件内容短于就绪记录时，RPC 报告存储错误并返回空附件。
        auto &original = originals.at(located.result().fdfs_file_id());
        original.pop_back();
        brpc::Controller failed_download;
        service.DownloadFile(&failed_download, &download, &downloaded, nullptr);
        EXPECT_EQ(downloaded.error_code(), static_cast<int>(ErrorCode::FILE_STORAGE_FAILED));
        EXPECT_TRUE(downloaded.file_name().empty());
        EXPECT_TRUE(failed_download.response_attachment().empty());

        DeleteFileRequest deletion;
        SetIdentity(deletion);
        deletion.set_file_id(id);
        DeleteFileResponse deleted;
        connections->sqlite_in_use = true;
        service.DeleteFile(nullptr, &deletion, &deleted, nullptr);
        EXPECT_EQ(deleted.error_code(), static_cast<int>(ErrorCode::FILE_SQLITE_IN_USE));
        EXPECT_EQ(ReadRecord(id)->status, kFileReady);
        connections->sqlite_in_use = false;
        service.DeleteFile(nullptr, &deletion, &deleted, nullptr);
        EXPECT_EQ(deleted.error_code(), 0);
        EXPECT_TRUE(originals.empty());
    }

    // 下游清理失败后只保留可重试的删除中记录，查询缓存应已失效。
    TEST_F(FileServiceTest, DeleteRetriesAfterDownstreamCleanupFailure)
    {
        const auto id = RegisterExcel();
        business->UploadFile("upload", kOwner, id, kWorkbook);
        ASSERT_EQ(business->GetWorksheetDBTables(kOwner, id).size(), 2U);

        downstream->drop_error = ErrorCode::FILE_DEPENDENCY_FAILED;
        ExpectError(ErrorCode::FILE_DEPENDENCY_FAILED, [&]
                    { business->DeleteFile(kOwner, id); });
        ASSERT_EQ(ReadRecord(id)->status, kFileDeleting);
        EXPECT_EQ(redis->exists("file:" + id), 0);
        EXPECT_EQ(redis->exists("worksheet:" + id), 0);
        EXPECT_EQ(originals.size(), 1U);
        downstream->drop_error = ErrorCode::SUCCESS;
        business->DeleteFile(kOwner, id);
        EXPECT_FALSE(ReadRecord(id));
    }

    // 待上传登记到期后，第二步上传会回收记录并要求重新登记。
    TEST_F(FileServiceTest, ReclaimsExpiredRegistration)
    {
        const auto id = RegisterExcel();
        Execute(*database, "UPDATE tbl_fileInfo SET uploadTime = 0 WHERE fileId = '" + id + "'");
        ExpectError(ErrorCode::FILE_UPLOAD_EXPIRED, [&] { business->UploadFile("expired", kOwner, id, kWorkbook); });
        EXPECT_FALSE(ReadRecord(id));
    }

    // 在真实的导入等待期间并发调用，覆盖命名锁及行锁各自的作用。
    TEST_F(FileServiceTest, SerializesFileMutationsWhileImportIsInFlight)
    {
        const auto id = RegisterExcel();
        const auto another = RegisterExcel();
        std::promise<std::string> importing;
        auto started = importing.get_future();
        std::promise<void> resume;
        const auto may_finish = resume.get_future();
        downstream->before_import = [&](const FileInfoEntity &file)
        {
            if (file.fileId != id)
                return;
            importing.set_value(*file.fdfsFileId);
            if (may_finish.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
        };

        auto upload = std::async(std::launch::async, [&] { business->UploadFile("first", kOwner, id, kWorkbook); });
        ASSERT_EQ(started.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        const auto uploading = ReadRecord(id);
        ASSERT_TRUE(uploading);
        ASSERT_TRUE(uploading->fdfsFileId);
        EXPECT_EQ(*uploading->fdfsFileId, started.get());
        EXPECT_EQ(uploading->status, kFileUploading);
        // 导入期间文件行可查询，同一个文件的写操作仍被命名锁拦住。
        ExpectError(ErrorCode::FILE_BUSY, [&] { business->UploadFile("duplicate", kOwner, id, kWorkbook); });
        ExpectError(ErrorCode::FILE_BUSY, [&] { business->DeleteFile(kOwner, id); });
        ExpectError(ErrorCode::FILE_UNAVAILABLE, [&] { business->GetFileInfo(kOwner, id); });
        business->UploadFile("another", kOwner, another, kWorkbook);
        EXPECT_EQ(business->GetFileList(kOwner).size(), 1U);

        resume.set_value();
        upload.get();
        EXPECT_EQ(business->GetFileList(kOwner).size(), 2U);
        EXPECT_TRUE(downstream->HasImportedFile(id));
    }

    // 第二次缓存清理失败时，MySQL 回滚文件记录和级联映射的删除。
    TEST_F(FileServiceTest, RollbackWhenSecondCacheClearFails)
    {
        const auto id = RegisterExcel();
        business->UploadFile("upload", kOwner, id, kWorkbook);
        const auto gate_id = Utils::GenerateSecureId();
        // 暂停 DELETE，使测试能在第一次删缓存之后撤销专用客户端的 DEL 权限。
        const auto gate_name = "chat2data:file:" + gate_id;
        Execute(*database, "CREATE TRIGGER pause_file_delete BEFORE DELETE ON tbl_fileInfo FOR EACH ROW "
                           "BEGIN DO GET_LOCK('" +
                               gate_name + "', 10); DO RELEASE_LOCK('" + gate_name + "'); END");
        acl_user = schema;
        auto restricted = redis_settings;
        restricted.user = acl_user;
        restricted.passwd = Utils::GenerateSecureId();
        redis->command<std::string>("ACL", "SETUSER", acl_user, "on", ">" + restricted.passwd,
                                    "~file:" + id, "~worksheet:" + id, "+del");
        FileBusiness deletion(FileData(database, Breezeredis::RedisFactory::create(restricted)),
                              MakeStorage(), std::make_unique<DownstreamStub>());

        std::future<void> remove;
        {
            FileData data(database, redis);
            auto pause = data.LockOperation(gate_id);
            remove = std::async(std::launch::async, [&]
                                { ExpectError(ErrorCode::FILE_CACHE_FAILED, [&]
                                              { deletion.DeleteFile(kOwner, id); }); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool deleting = false;
            while (!deleting && std::chrono::steady_clock::now() < deadline)
            {
                try
                {
                    Execute(*database, "SELECT id FROM tbl_fileInfo WHERE fileId = '" + id + "' "
                                                                                             "AND status = 'deleting' FOR UPDATE NOWAIT");
                }
                catch (const odb::mysql::database_exception &error)
                {
                    if (error.error() != 3572)
                        throw;
                    // 单独读取已提交状态，区分标记删除中的事务和最终 DELETE。
                    odb::transaction read(database->begin());
                    deleting = database->execute("SELECT id FROM tbl_fileInfo WHERE fileId = '" + id +
                                                 "' AND status = 'deleting'") == 1;
                    read.commit();
                }
                if (!deleting)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            EXPECT_TRUE(deleting);
            redis->command<std::string>("ACL", "SETUSER", acl_user, "-del");
        }
        remove.get();
        const auto row = ReadRecord(id);
        ASSERT_TRUE(row);
        EXPECT_EQ(row->status, kFileDeleting);
        EXPECT_TRUE(row->fdfsFileId);
        EXPECT_TRUE(originals.empty());
        {
            FileData data(database, redis);
            auto transaction = data.Begin();
            EXPECT_EQ(data.FindWorksheets(id).size(), 2U);
        }

        business->DeleteFile(kOwner, id);
        EXPECT_FALSE(ReadRecord(id));
    }

    // 使用真实 FastDFS 和解析 RPC，将工作簿交给注入的存储和 AI 替身处理。
    TEST_F(FileServiceTest, UploadsRealWorkbookWithInjectedStorageAndAi)
    {
        using Breezefdfs::FDFSClient;
        Breezefdfs::fdfs_settings settings;
        settings.trackers = {"dev-tracker:22122"};
        FDFSClient::init(settings);
        struct FastDfsGuard
        {
            ~FastDfsGuard() { FDFSClient::destroy(); }
        } guard;

        std::ifstream workbook(FILE_WORKBOOK, std::ios::binary);
        ASSERT_TRUE(workbook.good());
        const std::string bytes((std::istreambuf_iterator<char>(workbook)), std::istreambuf_iterator<char>());
        auto storage = MakeFastDfsStorage();
        // 记录本次创建的原件 ID，让断言失败时也能清理 FastDFS 数据。
        struct OriginalGuard
        {
            std::function<bool(const std::string &)> remove;
            std::optional<std::string> remote;
            ~OriginalGuard()
            {
                if (remote)
                    remove(*remote);
            }
        } original{storage.remove, std::nullopt};
        storage.upload = [upload = storage.upload, &original](const std::string &content)
        {
            original.remote = upload(content);
            return original.remote;
        };

        chat2data::ExcelParserServiceImpl parser(chat2data::ExcelParserBusiness(FDFSClient::download_to_file));
        brpc::Server server;
        ASSERT_EQ(server.AddService(&parser, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(server.Start(0, nullptr), 0);
        auto channels = std::make_shared<SvcChannels>();
        channels->setWatch("excelService");
        channels->addNode("excelService", "127.0.0.1:" + std::to_string(server.listen_address().port));

        auto dependency = std::make_unique<DownstreamStub>(MakeFileDownstream(channels, "excelService"));
        auto *services = dependency.get();
        FileBusiness file_service(FileData(database, redis), storage, std::move(dependency));
        auto info = WorkbookInfo("原始工作簿.xlsx");
        info.set_file_size(bytes.size());
        const auto id = file_service.UploadFileInfo(kOwner, info);
        file_ids.push_back(id);
        file_service.UploadFile("parse-test", kOwner, id, bytes);
        ASSERT_TRUE(original.remote);

        const auto preview = file_service.PreviewExcel(kOwner, id, "", 1, 50);
        ASSERT_GT(preview.excel_data().sheets_size(), 1);
        EXPECT_GT(preview.excel_data().sheets(0).data_size(), 0);
        EXPECT_TRUE(services->HasImportedFile(id));
        EXPECT_EQ(file_service.DownloadFile(kOwner, id).second, bytes);
        file_service.HandleFileChatSessionMap(kOwner, id, "real-workbook-session");
        EXPECT_EQ(services->SessionCount(id), 1U);

        file_service.DeleteFile(kOwner, id);
        EXPECT_FALSE(ReadRecord(id));
        EXPECT_FALSE(services->HasImportedFile(id));
        EXPECT_EQ(services->SessionCount(id), 0U);
        EXPECT_TRUE(storage.remove(*original.remote)); // Storage 返回原件已不存在时，重复清理可成功。
        original.remote.reset();
        server.Stop(0);
        server.Join();
    }

}
