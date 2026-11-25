#pragma once

#include "excelParser.hpp"

#include <functional>

namespace chat2data
{
    // 按 FastDFS 文件 ID 下载工作簿，并管理每次调用使用的本地副本。
    class ExcelParserBusiness
    {
    public:
        // 将指定文件写入已创建的临时路径，下载成功时返回 true。
        using Download = std::function<bool(const std::string &file_id, const std::string &path)>;

        /**
         * @brief 保存工作簿下载函数。
         * @param download 下载入口，服务进程使用 BreezeFDFS。
         */
        explicit ExcelParserBusiness(Download download);

        /**
         * @brief 下载工作簿并读取工作表名称。
         * @param file_id FastDFS 返回的完整文件 ID。
         * @return 按工作簿顺序排列的工作表原名。
         * @throws chat2Data::Chat2DataException 下载失败或工作簿无效。
         */
        std::vector<std::string> GetWorksheets(const std::string &file_id) const;

        /**
         * @brief 下载一次工作簿并解析所有指定的工作表。
         * @param file_id FastDFS 返回的完整文件 ID。
         * @param worksheets 待解析的工作表原名。
         * @return 按请求顺序排列的完整解析结果。
         * @throws chat2Data::Chat2DataException 下载失败、工作簿无效或任一工作表解析失败。
         */
        std::vector<chat2Data::WorksheetData> ParseExcel(const std::string &file_id, const std::vector<std::string> &worksheets) const;

    private:
        Download _download; // 下载工作簿的函数，由服务进程提供
    };
}
