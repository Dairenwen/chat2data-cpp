#include "fileDependencies.hpp"

#include "BreezeFDFS.hpp"
#include "BreezeLog.hpp"
#include "BreezeRPC.hpp"
#include "errorHandler.hpp"
#include "utils.hpp"

#include <brpc/controller.h>
#include <cerrno>
#include <utility>

namespace chat2Data
{
    namespace
    {
        using Breezefdfs::FDFSClient;

        // 删除原件时先定位 Storage 节点，定位失败或原件不存在都算成功。原件删除失败时记录日志并返回 false
        bool RemoveOriginal(const std::string &fdfs_id)
        {
            if (!Utils::IsCompleteFastDfsId(fdfs_id))
                return false;
            // Tracker 的定位接口分别接收组名和远端路径，删除接口接收完整 ID。
            const auto slash = fdfs_id.find('/');
            const auto group = fdfs_id.substr(0, slash);
            auto *tracker = tracker_get_connection();
            if (!tracker)
                return false;

            // 先定位 Storage，Tracker 的查找错误与原件不存在分别处理。
            ConnectionInfo storage{};
            const int lookup = tracker_query_storage_update(tracker, &storage, group.c_str(), fdfs_id.c_str() + slash + 1);
            tracker_close_connection(tracker); // 关闭 Tracker 连接
            if (lookup != 0)
            {
                WRN("定位 FastDFS 原件所在节点失败，SDK 错误码 {}", lookup);
                return false;
            }
            storage.sock = -1;
            // 由 SDK 连接已定位的 Storage 并请求删除。ENOENT 表示本次重试可继续后续清理。
            const int result = storage_delete_file1(nullptr, &storage, fdfs_id.c_str());
            if (result == 0 || result == ENOENT)
                return true;

            WRN("删除 FastDFS 原件失败，SDK 错误码 {}", result);
            return false;
        }

        // 当前还没有实现存储子服务以及AI子服务，采用继承方便在测试中注入 RPC 依赖，生产环境使用 MakeFastDfsStorage 和 MakeFileDownstream。
        class RpcFileDownstream final : public FileDownstream
        {
        public:
            RpcFileDownstream(std::shared_ptr<SvcChannels> channels, std::string excel_service)
                : _channels(std::move(channels)), _excel_service(std::move(excel_service))
            {
            }

            std::vector<WorksheetData> ParseWorkbook(const std::string &request_id, const std::string &fdfs_id) override
            {
                const auto channel = _channels->getNode(_excel_service);
                if (!channel)
                    throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);

                // 第一步取原工作表名及顺序。每次 RPC 先判断传输结果，再检查业务错误码。
                ExcelParserService_Stub stub(channel.get());
                GetWorksheetsRequest list;
                list.set_request_id(request_id);
                list.set_fdfs_file_id(fdfs_id);
                GetWorksheetsResponse names;
                brpc::Controller list_controller;
                stub.GetWorksheets(&list_controller, &list, &names, nullptr);
                CheckParserResponse(list_controller, names.error_code(), request_id);
                if (names.worksheets().empty())
                    throw Chat2DataException(ErrorCode::FILE_PARSE_FAILED);

                // 第二步使用第一次 RPC 返回的名单，解析每个工作表的内容。
                ParseExcelRequest parse;
                parse.set_request_id(request_id);
                parse.set_fdfs_file_id(fdfs_id);
                parse.mutable_worksheets()->Swap(names.mutable_worksheets());
                ParseExcelResponse data;
                brpc::Controller parse_controller;
                stub.ParseExcel(&parse_controller, &parse, &data, nullptr);
                CheckParserResponse(parse_controller, data.error_code(), request_id);
                if (data.worksheets_size() != parse.worksheets_size())
                    throw Chat2DataException(ErrorCode::FILE_PARSE_FAILED);

                // 第三步按解析顺序返回工作表数据，原名与第一次 RPC 返回的名单一致。
                std::vector<WorksheetData> sheets;
                sheets.reserve(data.worksheets_size());
                for (int i = 0; i < data.worksheets_size(); ++i)
                {
                    if (data.worksheets(i).name() != parse.worksheets(i))
                        throw Chat2DataException(ErrorCode::FILE_PARSE_FAILED);
                    sheets.emplace_back().Swap(data.mutable_worksheets(i));
                }
                return sheets;
            }

            // 存储和 AI 的生产适配当前统一返回依赖错误，调用参数和业务约定见 FileDownstream
            std::vector<WorksheetMapping> ImportExcelData(const FileInfoEntity &, const std::vector<WorksheetData> &, bool &) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }
            void DropTableExcel(const FileInfoEntity &) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }
            FilePreviewSheet PreviewImportedWorksheet(const FileInfoEntity &, const std::string &, int, int) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }
            bool HasSQLiteFileConnections(const FileInfoEntity &) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }
            void UpdateSessionFile(const FileInfoEntity &, const std::string &) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }
            void DeleteFileChatSessions(const FileInfoEntity &) override { throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED); }

        private:
            static void CheckParserResponse(const brpc::Controller &controller, int error_code, const std::string &request_id)
            {
                if (controller.Failed())
                {
                    WRN("调用 Excel 解析服务失败，请求 ID {}，RPC 错误码 {}", request_id, controller.ErrorCode());
                    throw Chat2DataException(ErrorCode::FILE_DEPENDENCY_FAILED);
                }
                if (error_code != 0)
                {
                    WRN("Excel 解析失败，请求 ID {}，下游错误码 {}", request_id, error_code);
                    throw Chat2DataException(ErrorCode::FILE_PARSE_FAILED);
                }
            }

            std::shared_ptr<SvcChannels> _channels;
            std::string _excel_service;
        };
    }

    FileStorage MakeFastDfsStorage()
    {
        // 上传和下载复用 BreezeFDFS，删除单独保留 SDK 错误码以支持重试。
        return {FDFSClient::upload_from_buff, FDFSClient::download_to_buff, RemoveOriginal};
    }

    std::unique_ptr<FileDownstream> MakeFileDownstream(std::shared_ptr<SvcChannels> channels, std::string excel_service)
    {
        return std::make_unique<RpcFileDownstream>(std::move(channels), std::move(excel_service));
    }
}
