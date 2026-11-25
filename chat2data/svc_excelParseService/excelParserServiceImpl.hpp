#pragma once

#include "excelParseService.pb.h"
#include "excelParserBusiness.hpp"

namespace chat2data
{
    // 校验解析请求，将业务结果或错误转换为 RPC 响应。
    class ExcelParserServiceImpl : public chat2Data::ExcelParserService
    {
    public:
        /**
         * @brief 接管负责下载和解析工作簿的业务对象。
         * @param business Excel 解析业务对象。
         */
        explicit ExcelParserServiceImpl(ExcelParserBusiness business);

        /**
         * @brief 获取工作簿中的工作表名称。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID 和 FastDFS 文件 ID。
         * @param response 回传请求 ID 和业务状态，成功时按工作簿顺序返回工作表原名。
         * @param done 响应就绪后由 ClosureGuard 执行的回调。
         */
        void GetWorksheets(google::protobuf::RpcController *controller,
                           const chat2Data::GetWorksheetsRequest *request,
                           chat2Data::GetWorksheetsResponse *response,
                           google::protobuf::Closure *done) override;

        /**
         * @brief 解析指定的工作表并返回结构化结果。
         * @param controller RPC 控制器，用于传输层状态管理。
         * @param request 包含请求 ID、FastDFS 文件 ID 和至少一个工作表原名。
         * @param response 成功时按请求顺序返回解析结果，失败时工作表列表为空。
         * @param done 响应就绪后由 ClosureGuard 执行的回调。
         */
        void ParseExcel(google::protobuf::RpcController *controller,
                        const chat2Data::ParseExcelRequest *request,
                        chat2Data::ParseExcelResponse *response,
                        google::protobuf::Closure *done) override;

    private:
        ExcelParserBusiness _business;
    };
}
