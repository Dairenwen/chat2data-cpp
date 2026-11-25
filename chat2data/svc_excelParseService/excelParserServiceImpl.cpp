#include "excelParserServiceImpl.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"

#include <brpc/closure_guard.h>
#include <string_view>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;

        // 检查 FastDFS 文件 ID 是否完整，避免 RPC 处理无效请求
        bool IsCompleteFileId(const std::string &file_id)
        {
            if (file_id.find('/') == std::string::npos || file_id.find_first_of("\\ \t\r\n") != std::string::npos || file_id.find('\0') != std::string::npos)
                return false;

            // FastDFS 文件 ID 由组名和远端逻辑路径组成，每一段都必须完整
            std::string_view path(file_id); // 特殊类型：字符串视图
            for (;;)
            {
                const auto slash = path.find('/');
                const auto part = path.substr(0, slash);
                if (part.empty() || part == "." || part == "..")
                    return false;
                if (slash == std::string_view::npos)
                    return true;
                path.remove_prefix(slash + 1);
            }
        }

        // 两条 RPC 共用请求校验、响应完成和业务异常映射。
        template <typename Request, typename Response, typename Run>
        void HandleExcelRpc(const Request &request, Response &response, google::protobuf::Closure *done, const char *method, Run run)
        {
            // 写入响应后由 ClosureGuard 执行回调，覆盖成功和异常路径。
            brpc::ClosureGuard guard(done);
            response.set_request_id(request.request_id());
            auto code = ErrorCode::SUCCESS;
            try
            {
                // 若请求 ID 为空或文件 ID 不完整，则直接抛出异常，避免 RPC 处理无效请求
                if (request.request_id().empty() || !IsCompleteFileId(request.fdfs_file_id()))
                    throw Chat2DataException(ErrorCode::EXCEL_INVALID_INPUT);

                INF("RPC {} 开始处理工作簿，请求 ID {}", method, request.request_id());
                run();
            }
            catch (const Chat2DataException &error)
            {
                code = error.Code();
            }
            catch (...)
            {
                code = ErrorCode::EXCEL_PARSE_FAILED;
            }

            response.set_error_code(static_cast<int>(code));
            if (code == ErrorCode::SUCCESS)
            {
                response.clear_error_msg();
                INF("RPC {} 处理成功，请求 ID {}，工作表数量 {}", method, request.request_id(), response.worksheets_size());
            }
            else
            {
                // 解析失败时只返回错误，调用方从成功响应取得完整工作表。
                response.clear_worksheets();
                response.set_error_msg(chat2Data::error2String(code));
                WRN("RPC {} 处理失败，请求 ID {}，错误码 {}，{}", method, request.request_id(), static_cast<int>(code), response.error_msg());
            }
        }
    }

    ExcelParserServiceImpl::ExcelParserServiceImpl(ExcelParserBusiness business)
        : _business(std::move(business))
    {
    }

    void ExcelParserServiceImpl::GetWorksheets(google::protobuf::RpcController *, const chat2Data::GetWorksheetsRequest *request,
                                               chat2Data::GetWorksheetsResponse *response, google::protobuf::Closure *done)
    {
        HandleExcelRpc(*request, *response, done, "GetWorksheets", [&]
                       {
                           auto names = _business.GetWorksheets(request->fdfs_file_id());
                           for (auto &name : names)
                               *response->add_worksheets() = std::move(name); });
    }

    void ExcelParserServiceImpl::ParseExcel(google::protobuf::RpcController *, const chat2Data::ParseExcelRequest *request,
                                            chat2Data::ParseExcelResponse *response, google::protobuf::Closure *done)
    {
        HandleExcelRpc(*request, *response, done, "ParseExcel", [&]
                       {
                           // 请求中必须包含至少一个工作表名称，且每个名称都不能为空
                           if (request->worksheets().empty())
                               throw Chat2DataException(ErrorCode::EXCEL_INVALID_INPUT);
                           std::vector<std::string> names;
                           names.reserve(request->worksheets_size());
                           for (const auto &name : request->worksheets())
                           {
                               if (name.empty())
                                   throw Chat2DataException(ErrorCode::EXCEL_INVALID_INPUT);
                               names.push_back(name);
                           }

                           // 解析 Excel 工作簿，返回工作表数据
                           auto worksheets = _business.ParseExcel(request->fdfs_file_id(), names);
                           for (auto &worksheet : worksheets)
                               response->add_worksheets()->Swap(&worksheet); }); // 使用swap避免深拷贝
    }
}
