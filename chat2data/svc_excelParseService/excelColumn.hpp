#pragma once

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chat2data
{
    // 列推断所用的样本类型，单元格响应使用工作簿原始类型
    enum class ExcelSampleType
    {
        Integer,
        Float,
        Boolean,
        Date,
        DateTime,
        Text,
        Count,
        Skip
    };

    // 保存同一工作表已经分配的列名，以及每个重名列下一次可尝试的编号。
    using ExcelColumnNames = std::unordered_map<std::string, unsigned>;

    /**
     * @brief 整理表头，在同一工作表内按不区分 ASCII 大小写的规则去重。
     * @param header 原始表头文本。
     * @param column 从 1 开始的列号，用于为空表头生成列名。
     * @param names 当前工作表已分配的列名记录。
     * @return 符合项目命名约定的列名。
     */
    std::string NormalizeExcelColumnName(std::string_view header, unsigned column, ExcelColumnNames &names);

    // 按数值、布尔、日期的优先级识别文本样本，空白和 N/A 返回 Skip。
    ExcelSampleType ClassifyExcelText(std::string_view text);

    // 根据数值选取自定义格式段，普通数值格式返回 Skip。
    ExcelSampleType ClassifyExcelNumberFormat(std::string_view format, double value);

    // 根据内置数字格式 ID 识别日期和时间，普通数值格式返回 Skip。
    ExcelSampleType ClassifyExcelBuiltinFormat(unsigned id);

    // 汇总单列采样结果并映射到协议支持的列类型。
    struct ExcelColumnSamples
    {
        // date_serial 表示数值单元格按日期或时间格式显示。
        void Add(ExcelSampleType type, bool date_serial);

        // 采用票数最高的样本类型，空样本、并列和无法映射的日期类型使用 TEXT。
        const char *ColumnType() const;

    private:
        std::array<unsigned, static_cast<unsigned>(ExcelSampleType::Count)> _counts{};
        bool _has_date_serial = false;
    };
}
