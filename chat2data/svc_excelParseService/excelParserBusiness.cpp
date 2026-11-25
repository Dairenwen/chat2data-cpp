#include "excelParserBusiness.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <utility>

namespace chat2data
{
    namespace
    {
        // 每次调用独占一个本地副本，成功和异常路径均随对象析构清理。
        class TemporaryWorkbook
        {
        public:
            TemporaryWorkbook()
                : _path((std::filesystem::temp_directory_path() / "chat2data-excel-XXXXXX.xlsx").string())
            {
                // 由系统原子地创建仅当前用户可读写的文件，供 FastDFS 写入
                const int fd = mkstemps(_path.data(), 5); // 替换 "XXXXXX" 为唯一后缀，返回文件描述符
                if (fd < 0)
                    throw chat2Data::Chat2DataException(chat2Data::ErrorCode::EXCEL_PARSE_FAILED);
                close(fd);
            }

            ~TemporaryWorkbook()
            {
                if (unlink(_path.c_str()) != 0)
                    WRN("清理 Excel 工作簿临时文件失败");
            }

            TemporaryWorkbook(const TemporaryWorkbook &) = delete;
            TemporaryWorkbook &operator=(const TemporaryWorkbook &) = delete;

            const std::string &Path() const
            {
                return _path;
            }

        private:
            std::string _path;
        };

        void DownloadWorkbook(const ExcelParserBusiness::Download &download, const std::string &file_id, const TemporaryWorkbook &workbook)
        {
            if (!download(file_id, workbook.Path()))
                throw chat2Data::Chat2DataException(chat2Data::ErrorCode::EXCEL_DOWNLOAD_FAILED);
        }
    }

    ExcelParserBusiness::ExcelParserBusiness(Download download) : _download(std::move(download))
    {
    }

    std::vector<std::string> ExcelParserBusiness::GetWorksheets(const std::string &file_id) const
    {
        const TemporaryWorkbook workbook;
        DownloadWorkbook(_download, file_id, workbook);
        return ExcelParser::GetWorksheets(workbook.Path());
    }

    std::vector<chat2Data::WorksheetData> ExcelParserBusiness::ParseExcel(const std::string &file_id, const std::vector<std::string> &worksheets) const
    {
        const TemporaryWorkbook workbook;
        DownloadWorkbook(_download, file_id, workbook);
        return ExcelParser::ParseExcel(workbook.Path(), worksheets);
    }
}
