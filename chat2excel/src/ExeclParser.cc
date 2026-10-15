#include "ExeclParser.h"

#include <OpenXLSX.hpp>
#include <bite_scaffold/log.h>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

std::vector<std::string> ExcelParser::getWorksheetNames(const std::string& filePath) {
    try {
        if (filePath.empty()) {
            throw std::invalid_argument("Excel 文件路径不能为空");
        }

        // XLDocument 在离开作用域时自动关闭文件，包括发生异常的情况。
        OpenXLSX::XLDocument document;
        document.open(filePath);
        auto worksheetNames = document.workbook().worksheetNames();
        if (bitelog::g_logger) {
            INF("获取 Excel 工作表名称成功，文件：{}，工作表数量：{}",
                filePath, worksheetNames.size());
        }
        return worksheetNames;
    } catch (const std::exception& error) {
        if (bitelog::g_logger) {
            ERR("获取 Excel 工作表名称失败，文件：{}，原因：{}", filePath, error.what());
        }
        throw;
    }
}

std::string ExcelParser::parseWorksheet(const std::string& filePath,const std::string& worksheetName)
{
    try {
        if (filePath.empty() || worksheetName.empty()) {
            throw std::invalid_argument("Excel 文件路径和工作表名称不能为空");
        }

        OpenXLSX::XLDocument document;
        document.open(filePath);
        auto worksheet = document.workbook().worksheet(worksheetName);

        // 在遍历前固定范围，不能按非空单元格数量决定输出行列数。
        const auto rowCount = worksheet.rowCount();
        const auto columnCount = worksheet.columnCount();
        std::string result;

        if (rowCount > 0 && columnCount > 0) {
            auto cellRange = worksheet.range(OpenXLSX::XLCellReference(1, 1),OpenXLSX::XLCellReference(rowCount, columnCount));
            auto cellIterator = cellRange.begin();

            // 范围迭代器按行遍历所有坐标，包括 XML 中不存在的空单元格。先检查 cellExists()，避免解引用时创建空单元格或改变工作表结构。
            for (std::uint32_t rowIndex = 1; rowIndex <= rowCount; ++rowIndex) {
                if (rowIndex > 1) {
                    result.push_back('\n');
                }
                for (std::uint32_t columnIndex = 1; columnIndex <= columnCount;
                     ++columnIndex, ++cellIterator) {
                    if (columnIndex > 1) {
                        result.push_back(',');
                    }
                    if (cellIterator.cellExists()) {
                        const OpenXLSX::XLCellValue value = cellIterator->value();
                        result += escapeCsvField(cellValueToString(value));
                    }
                    // 空字段不写文本，但保留其位置对应的列分隔符。
                }
            }
        } else if (rowCount > 0) {
            // 存在空行但没有单元格时，仍保留行数，不能凭空增加一列。
            result.assign(static_cast<std::size_t>(rowCount - 1), '\n');
        }

        if (bitelog::g_logger) {
            INF("解析 Excel 工作表成功，文件：{}，工作表：{}，行数：{}，列数：{}",filePath, worksheetName, rowCount, columnCount);
        }
        return result;
    } catch (const std::exception& error) {
        if (bitelog::g_logger) {
            ERR("解析 Excel 工作表失败，文件：{}，工作表：{}，原因：{}",filePath, worksheetName, error.what());
        }
        throw;
    }
}

std::string ExcelParser::cellValueToString(const OpenXLSX::XLCellValue& value) {
    switch (value.type()) {
        case OpenXLSX::XLValueType::Empty:
            return {};
        case OpenXLSX::XLValueType::Boolean:
            return value.get<bool>() ? "true" : "false";
        case OpenXLSX::XLValueType::Integer:
            return std::to_string(value.get<std::int64_t>());
        case OpenXLSX::XLValueType::Float: {
            std::ostringstream stream;
            // 固定小数点格式，避免系统区域设置产生与 CSV 冲突的逗号。保留可还原 double 的精度，避免默认流精度截断有效数字。
            stream.imbue(std::locale::classic());
            stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value.get<double>();
            return stream.str();
        }
        case OpenXLSX::XLValueType::String:
        case OpenXLSX::XLValueType::Error:
            // OpenXLSX 的错误值也以字符串保存，例如 #DIV/0!。
            return value.get<std::string>();
        default:
            throw std::runtime_error("不支持的 Excel 单元格值类型");
    }
}

std::string ExcelParser::escapeCsvField(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) {
        return text;
    }

    // 含分隔符的字段整体加引号；字段中的一个双引号写成两个双引号。
    // 单元格内部的换行保留在引号内，不作为新的 CSV 记录。
    std::string escapedText;
    escapedText.push_back('"');
    for (const char character : text) {
        if (character == '"') {
            escapedText.push_back('"');
        }
        escapedText.push_back(character);
    }
    escapedText.push_back('"');
    return escapedText;
}
