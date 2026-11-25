#pragma once

#include "excelParseService.pb.h"

#include <string>
#include <vector>

namespace chat2data
{
    // 读取本地 Excel 工作簿，按现有 RPC 协议组织解析结果。
    class ExcelParser
    {
    public:
        /**
         * @brief 获取工作簿中的工作表原名。
         * @param path 本地 .xlsx 文件路径。
         * @return 按工作簿顺序排列的工作表名称。
         * @throws chat2Data::Chat2DataException 工作簿无效或没有工作表。
         */
        static std::vector<std::string> GetWorksheets(const std::string &path);

        /**
         * @brief 在同一个工作簿中解析指定的工作表。
         * @param path 本地 .xlsx 文件路径。
         * @param worksheets 按原名精确匹配的工作表名称列表。
         * @return 按请求顺序排列的工作表数据，首行作为表头，数据行保留空单元格位置。
         * @throws chat2Data::Chat2DataException 工作簿无效、工作表不存在、无数据结构或解析失败。
         */
        static std::vector<chat2Data::WorksheetData> ParseExcel(const std::string &path, const std::vector<std::string> &worksheets);
    };
}
