#pragma once

#include <string>
#include <vector>

namespace OpenXLSX
{
    class XLCellValue;
}

/**
 * @brief 使用 OpenXLSX 读取 .xlsx 文件的静态工具类。
 *
 * 提供工作表名称查询和工作表数据解析，不修改或保存源文件。
 * 解析结果保留第一行作为标题行，按原工作表行列顺序输出 CSV 字符串。
 * 调用方可先初始化 bitelog::bitelog_init()；已初始化时使用 bite_scaffold
 * 记录操作结果和异常，未初始化时不访问空日志器。
 */
class ExcelParser {
public:
    /**
     * @brief 获取指定 Excel 文件中所有 worksheet 的名称。
     * @param filePath .xlsx 文件路径，不能为空。
     * @return 按工作簿顺序排列的工作表名称；不包含 chartsheet。
     * @throws std::invalid_argument 文件路径为空。
     * @throws std::exception 文件无法打开、内容无效等 OpenXLSX 或标准库异常。异常记录日志后原样向调用方传播。
     */
    static std::vector<std::string> getWorksheetNames(const std::string& filePath);

    /**
     * @brief 读取指定 worksheet 中全部单元格并返回 CSV 字符串。
     * @param filePath .xlsx 文件路径，不能为空。
     * @param worksheetName 工作表名称，不能为空，按名称精确匹配。
     * @return 第一行为原工作表标题行，后续行为数据行；行间使用 '\n'，列间使用 ','，末尾不额外添加行分隔符。不跳过空行或空单元格，
     *         每行均保留相同列数，范围由 rowCount()/columnCount() 确定。空单元格输出空字段，布尔值输出 true/false，错误值保留错误文本。
     *         包含逗号、双引号或换行的字段按 CSV 规则加引号并转义；此时行列数按 CSV 记录计算，而不是直接按字符拆分。
     *         无行时返回空字符串；公式读取缓存值，日期读取底层数值，不计算公式，也不套用 Excel 的显示格式。
     * @throws std::invalid_argument 文件路径或工作表名称为空。
     * @throws std::exception 文件无法打开、工作表不存在、值转换失败等，OpenXLSX 或标准库异常；记录日志后原样向调用方传播。
     */
    static std::string parseWorksheet(const std::string& filePath,const std::string& worksheetName);

private:
    ExcelParser() = delete;

    /**
     * @brief 按单元格实际类型转换文本，避免错误的类型转换或浮点精度丢失。
     * @param value OpenXLSX 单元格值。
     * @return 对应文本；空值返回空字符串。
     * @throws std::exception 不支持的值类型、值转换或字符串分配失败。
     */
    static std::string cellValueToString(const OpenXLSX::XLCellValue& value);

    /**
     * @brief 转义 CSV 字段中的分隔符和双引号，保留单元格原有内容。
     * @param text 单个单元格的文本。
     * @return 可作为单个 CSV 字段使用的文本。
     * @throws std::exception 字符串分配失败等标准库异常。
     */
    static std::string escapeCsvField(const std::string& text);
};
