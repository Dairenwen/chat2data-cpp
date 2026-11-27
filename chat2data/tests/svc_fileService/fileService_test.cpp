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
#include <future>
#include <iterator>
#include <map>
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

    // 提供解析、存储和 AI 的业务替身，原件操作由测试夹具单独提供。
    class DownstreamStub final : public FileDownstream
    {
    public:
        std::vector<WorksheetData> ParseWorkbook(const std::string &, const std::string &) override
        {
            std::vector<WorksheetData> sheets(2);
            sheets[0].set_name("汇总");
            sheets[1].set_name("明细");
            for (auto &sheet : sheets)
            {
                sheet.add_columns()->set_name("金额");
                sheet.add_rows()->add_cells()->set_value("10");
                sheet.set_total_rows(2);
            }
            return sheets;
        }

        std::vector<WorksheetMapping> ImportExcelData(const FileInfoEntity &file, const std::vector<WorksheetData> &sheets, bool &import_started) override
        {
            EXPECT_EQ(file.status, kFileUploading);
            EXPECT_EQ(file.userId, kOwner);
            EXPECT_FALSE(file.fileId.empty());
            EXPECT_TRUE(file.fdfsFileId);
            EXPECT_EQ(sheets[0].rows_size(), 1);
            import_started = true;
            if (fail_import)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            return {{sheets[0].name(), "sheet_summary"}, {sheets[1].name(), "sheet_detail"}};
        }

        void DropTableExcel(const FileInfoEntity &file) override
        {
            EXPECT_TRUE(file.fdfsFileId);
            if (fail_drop)
                throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
            ++drop_calls;
        }

        FilePreviewSheet PreviewImportedWorksheet(const FileInfoEntity &, const std::string &table, int page, int page_size) override
        {
            FilePreviewSheet sheet;
            sheet.add_columns("金额");
            if (table == "sheet_summary")
            {
                sheet.set_total_rows(3);
                const int start = (page - 1) * page_size;
                for (int i = start; i < std::min(start + page_size, 3); ++i)
                    sheet.add_data()->add_cells(std::to_string(i + 1));
            }
            return sheet;
        }

        bool HasSQLiteFileConnections(const FileInfoEntity &) override { return sqlite_in_use; }

        void UpdateSessionFile(const FileInfoEntity &file, const std::string &session) override
        {
            EXPECT_EQ(file.userId, kOwner);
            sessions.insert(session);
        }

        void DeleteFileChatSessions(const FileInfoEntity &) override { sessions.clear(); }

        bool fail_import = false;
        bool fail_drop = false;
        bool sqlite_in_use = false;
        int drop_calls = 0;
        std::set<std::string> sessions;
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
                    originals.emplace(id, bytes);
                    return id;
                },
                [this](const std::string &id, std::string &bytes)
                {
                    const auto found = originals.find(id);
                    if (found == originals.end())
                        return false;
                    bytes = found->second;
                    return true;
                },
                [this](const std::string &id)
                {
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
        EXPECT_EQ(mappings[0].table_name, "sheet_summary");
        EXPECT_EQ(mappings[1].table_name, "sheet_detail");
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
        EXPECT_EQ(downstream->sessions.size(), 2U);
        business->DeleteFile(kOwner, id);
        EXPECT_TRUE(downstream->sessions.empty());
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
        downstream->fail_drop = true;
        ExpectError(ErrorCode::FILE_DEPENDENCY_FAILED, [&]
                    { business->UploadFile("first", kOwner, id, kWorkbook); });
        const auto failed = ReadRecord(id);
        ASSERT_TRUE(failed);
        EXPECT_EQ(failed->status, kFileUploading);
        ASSERT_TRUE(failed->fdfsFileId);
        EXPECT_EQ(failed->uploadTime, registered_at);
        ASSERT_EQ(originals.size(), 1U);
        EXPECT_TRUE(business->GetFileList(kOwner).empty());

        downstream->fail_drop = false;
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

        downstream->fail_drop = true;
        ExpectError(ErrorCode::FILE_DEPENDENCY_FAILED, [&]
                    { business->DeleteFile(kOwner, id); });
        ASSERT_EQ(ReadRecord(id)->status, kFileDeleting);
        EXPECT_EQ(redis->exists("file:" + id), 0);
        EXPECT_EQ(redis->exists("worksheet:" + id), 0);
        EXPECT_EQ(originals.size(), 1U);
        downstream->fail_drop = false;
        business->DeleteFile(kOwner, id);
        EXPECT_FALSE(ReadRecord(id));
    }

    // 分别校验登记过期回收和同一文件的并发上传限制。
    TEST_F(FileServiceTest, ReclaimsExpiredRegistrationAndSerializesFileMutation)
    {
        const auto id = RegisterExcel();
        Execute(*database, "UPDATE tbl_fileInfo SET uploadTime = 0 WHERE fileId = '" + id + "'");
        ExpectError(ErrorCode::FILE_UPLOAD_EXPIRED, [&]
                    { business->UploadFile("expired", kOwner, id, kWorkbook); });
        EXPECT_FALSE(ReadRecord(id));

        const auto active_id = RegisterExcel();
        {
            FileData other_instance(database, redis);
            auto active_upload = other_instance.LockOperation(active_id);
            ExpectError(ErrorCode::FILE_BUSY, [&]
                        { business->UploadFile("busy", kOwner, active_id, kWorkbook); });
        }
        business->UploadFile("upload", kOwner, active_id, kWorkbook);
        EXPECT_EQ(ReadRecord(active_id)->status, kFileReady);
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

    // 使用开发环境的 FastDFS 和真实解析 RPC，验证原件传输及重复清理。
    TEST(FileDependenciesTest, ParsesRealWorkbookFromFastDfs)
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
        const auto storage = MakeFastDfsStorage();
        const auto remote = storage.upload(bytes);
        ASSERT_TRUE(remote);
        struct OriginalGuard
        {
            const FileStorage &storage;
            const std::string &remote;
            ~OriginalGuard() { storage.remove(remote); }
        } original{storage, *remote};

        chat2data::ExcelParserServiceImpl parser(chat2data::ExcelParserBusiness(FDFSClient::download_to_file));
        brpc::Server server;
        ASSERT_EQ(server.AddService(&parser, brpc::SERVER_DOESNT_OWN_SERVICE), 0);
        ASSERT_EQ(server.Start(0, nullptr), 0);
        auto channels = std::make_shared<SvcChannels>();
        channels->setWatch("excelService");
        channels->addNode("excelService", "127.0.0.1:" + std::to_string(server.listen_address().port));

        auto dependency = MakeFileDownstream(channels, "excelService");
        const auto sheets = dependency->ParseWorkbook("parse-test", *remote);
        ASSERT_GT(sheets.size(), 1U);
        EXPECT_GT(sheets[0].rows_size(), 0);
        std::string downloaded;
        EXPECT_TRUE(storage.download(*remote, downloaded));
        EXPECT_EQ(downloaded, bytes);
        EXPECT_TRUE(storage.remove(*remote));
        EXPECT_TRUE(storage.remove(*remote));
        server.Stop(0);
        server.Join();
    }

}
