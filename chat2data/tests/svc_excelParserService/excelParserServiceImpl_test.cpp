#include "errorHandler.hpp"
#include "excelParserServiceImpl.hpp"

#include <brpc/channel.h>
#include <brpc/server.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <unordered_set>

namespace
{
    using chat2Data::ErrorCode;
    using namespace chat2Data;

    constexpr char kWorkbook[] = "group1/M00/00/00/workbook.xlsx";
    constexpr char kCorruptWorkbook[] = "group1/M00/00/00/corrupt.xlsx";
    constexpr char kMissingWorkbook[] = "group1/M00/00/00/missing.xlsx";

    class ExcelParserServiceTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            // 回环 RPC 执行真实业务与解析代码，下载入口提供可重复的文件和部分写入失败。
            chat2data::ExcelParserBusiness business([this](const std::string &id, const std::string &path)
                                                   { return Download(id, path); });
            _service = std::make_unique<chat2data::ExcelParserServiceImpl>(std::move(business));
            ASSERT_EQ(_server.AddService(_service.get(), brpc::SERVER_DOESNT_OWN_SERVICE), 0);

            brpc::ServerOptions server_options;
            server_options.has_builtin_services = false;
            ASSERT_EQ(_server.Start("127.0.0.1:0", &server_options), 0);

            brpc::ChannelOptions options;
            options.timeout_ms = 3000;
            options.max_retry = 0;
            ASSERT_EQ(_channel.Init(_server.listen_address(), &options), 0);
        }

        void TearDown() override
        {
            _server.Stop(0);
            _server.Join();
            const std::lock_guard<std::mutex> lock(_download_mutex);
            for (const auto &path : _downloads)
                EXPECT_FALSE(std::filesystem::exists(path));
        }

        template <typename Request, typename Response>
        void Call(void (ExcelParserService_Stub::*method)(google::protobuf::RpcController *, const Request *,
                                                          Response *, google::protobuf::Closure *),
                  const Request &request, Response &response, ErrorCode expected)
        {
            ExcelParserService_Stub stub(&_channel);
            brpc::Controller controller;
            (stub.*method)(&controller, &request, &response, nullptr);
            ASSERT_FALSE(controller.Failed()) << controller.ErrorText();
            EXPECT_EQ(response.request_id(), request.request_id());
            EXPECT_EQ(response.error_code(), static_cast<int>(expected));
            EXPECT_EQ(response.error_msg(), expected == ErrorCode::SUCCESS ? "" : chat2Data::error2String(expected));
            if (expected != ErrorCode::SUCCESS)
                EXPECT_TRUE(response.worksheets().empty());
        }

        std::size_t DownloadCount()
        {
            const std::lock_guard<std::mutex> lock(_download_mutex);
            return _downloads.size();
        }

    private:
        bool Download(const std::string &id, const std::string &path)
        {
            {
                const std::lock_guard<std::mutex> lock(_download_mutex);
                EXPECT_TRUE(_downloads.insert(path).second);
            }

            if (id == kMissingWorkbook)
            {
                std::ofstream(path) << "部分下载数据";
                return false;
            }

            const auto fixture = id == kWorkbook ? "multipleWorksheets.xlsx" : "corruptWorkbook.xlsx";
            return std::filesystem::copy_file(std::filesystem::path(EXCEL_TEST_FIXTURES) / fixture, path,
                                              std::filesystem::copy_options::overwrite_existing);
        }

        std::mutex _download_mutex;
        std::unordered_set<std::string> _downloads;
        std::unique_ptr<chat2data::ExcelParserServiceImpl> _service;
        brpc::Server _server;
        brpc::Channel _channel;
    };

    TEST_F(ExcelParserServiceTest, ReturnsCompleteResultsAndDownloadsOncePerRpc)
    {
        GetWorksheetsRequest list;
        list.set_request_id("list-worksheets");
        list.set_fdfs_file_id(kWorkbook);
        GetWorksheetsResponse listed;
        Call(&ExcelParserService_Stub::GetWorksheets, list, listed, ErrorCode::SUCCESS);
        ASSERT_EQ(listed.worksheets_size(), 3);
        EXPECT_EQ(listed.worksheets(0), "销售记录");
        EXPECT_EQ(listed.worksheets(1), "员工名单");
        EXPECT_EQ(listed.worksheets(2), "备注 表");

        ParseExcelRequest parse;
        parse.set_request_id("parse-worksheets");
        parse.set_fdfs_file_id(kWorkbook);
        parse.add_worksheets(listed.worksheets(2));
        parse.add_worksheets(listed.worksheets(0));
        ParseExcelResponse parsed;
        Call(&ExcelParserService_Stub::ParseExcel, parse, parsed, ErrorCode::SUCCESS);
        ASSERT_EQ(parsed.worksheets_size(), 2);
        EXPECT_EQ(parsed.worksheets(0).name(), "备注 表");
        EXPECT_EQ(parsed.worksheets(1).name(), "销售记录");
        EXPECT_EQ(parsed.worksheets(1).rows(0).cells(0).value(), "键盘");
        EXPECT_EQ(DownloadCount(), 2u);
    }

    TEST_F(ExcelParserServiceTest, RejectsInvalidRequestsBeforeDownload)
    {
        GetWorksheetsRequest list;
        list.set_request_id("invalid-file-id");
        list.set_fdfs_file_id("workbook.xlsx");
        GetWorksheetsResponse listed;
        Call(&ExcelParserService_Stub::GetWorksheets, list, listed, ErrorCode::EXCEL_INVALID_INPUT);
        list.set_fdfs_file_id(kWorkbook);
        list.clear_request_id();
        Call(&ExcelParserService_Stub::GetWorksheets, list, listed, ErrorCode::EXCEL_INVALID_INPUT);

        ParseExcelRequest parse;
        parse.set_request_id("invalid-worksheet");
        parse.set_fdfs_file_id(kWorkbook);
        ParseExcelResponse parsed;
        Call(&ExcelParserService_Stub::ParseExcel, parse, parsed, ErrorCode::EXCEL_INVALID_INPUT);
        parse.add_worksheets("");
        Call(&ExcelParserService_Stub::ParseExcel, parse, parsed, ErrorCode::EXCEL_INVALID_INPUT);
        EXPECT_EQ(DownloadCount(), 0u);
    }

    TEST_F(ExcelParserServiceTest, ReportsDownloadAndWorkbookFailuresAndDropsPartialResults)
    {
        GetWorksheetsRequest list;
        list.set_request_id("download-failed");
        list.set_fdfs_file_id(kMissingWorkbook);
        GetWorksheetsResponse listed;
        Call(&ExcelParserService_Stub::GetWorksheets, list, listed, ErrorCode::EXCEL_DOWNLOAD_FAILED);
        list.set_request_id("invalid-xlsx");
        list.set_fdfs_file_id(kCorruptWorkbook);
        Call(&ExcelParserService_Stub::GetWorksheets, list, listed, ErrorCode::EXCEL_WORKBOOK_INVALID);

        ParseExcelRequest parse;
        parse.set_request_id("missing-worksheet");
        parse.set_fdfs_file_id(kWorkbook);
        parse.add_worksheets("销售记录");
        parse.add_worksheets("销售记录 ");
        ParseExcelResponse parsed;
        Call(&ExcelParserService_Stub::ParseExcel, parse, parsed, ErrorCode::EXCEL_WORKSHEET_NOT_FOUND);
        EXPECT_EQ(DownloadCount(), 3u);
    }
}
