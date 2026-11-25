#include "excelColumn.hpp"
#include "utils.hpp"

#include <algorithm>

namespace chat2data
{
    namespace
    {
        constexpr unsigned kMaxColumnNameLength = 64; // 规范化后最多保留 64 个字符，汉字按一个完整 UTF-8 字符处理
        using chat2Data::Utils;
        using Type = ExcelSampleType;

        // 判断 UTF-8 字符是否为汉字，汉字范围包括 CJK 统一汉字、扩展 A、兼容汉字和部首扩展。
        bool IsHanCharacter(std::string_view character)
        {
            if (character.size() != 3)
                return false;
            const auto code = ((static_cast<unsigned char>(character[0]) & 0x0F) << 12) |
                              ((static_cast<unsigned char>(character[1]) & 0x3F) << 6) |
                              (static_cast<unsigned char>(character[2]) & 0x3F);
            return code == 0x3007 || (code >= 0x3400 && code <= 0x4DBF) ||
                   (code >= 0x4E00 && code <= 0x9FFF) || (code >= 0xF900 && code <= 0xFAFF);
        }

        // 将文本中的数字部分解析为整数，非数字返回 -1。
        int DatePart(std::string_view text)
        {
            if (!std::all_of(text.begin(), text.end(), Utils::IsAsciiDigit))
                return -1;
            int value = 0;
            for (const auto digit : text)
                value = value * 10 + digit - '0';
            return value;
        }

        // 根据文本识别日期和时间，返回 Skip 表示非日期类型。
        Type ClassifyDateText(std::string_view text)
        {
            const bool with_time = text.size() == 19;
            if (text.size() != 10 && !with_time)
                return Type::Text;

            // 年份在末尾的文本采用月、日、年顺序。
            const auto separator = text[4];
            int year, month, day;
            if ((separator == '-' || separator == '/' || separator == '.') && text[7] == separator)
            {
                year = DatePart(text.substr(0, 4));
                month = DatePart(text.substr(5, 2));
                day = DatePart(text.substr(8, 2));
            }
            else if (!with_time && (text[2] == '-' || text[2] == '/') && text[5] == text[2])
            {
                month = DatePart(text.substr(0, 2));
                day = DatePart(text.substr(3, 2));
                year = DatePart(text.substr(6, 4));
            }
            else
                return Type::Text;

            constexpr std::array<int, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
            if (year < 1 || month < 1 || month > 12 || day < 1 || day > days[month - 1] + (month == 2 && leap))
                return Type::Text;
            if (!with_time)
                return Type::Date;

            const auto hour = DatePart(text.substr(11, 2));
            const auto minute = DatePart(text.substr(14, 2));
            const auto second = DatePart(text.substr(17, 2));
            return separator == '-' && text[10] == ' ' && text[13] == ':' && text[16] == ':' &&
                           hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59
                       ? Type::DateTime
                       : Type::Text;
        }

        // 判断文本是否为时间间隔，格式为 Hh、Mm 或 Ss，忽略大小写。
        bool IsElapsedTime(std::string_view token)
        {
            if (token.empty() || token.size() > 2)
                return false;
            const char unit = Utils::ToAsciiLower(token.front());
            return (unit == 'h' || unit == 'm' || unit == 's') && Utils::ToAsciiLower(token.back()) == unit;
        }

        // 判断数值是否满足条件表达式，条件表达式为 <、<=、>、>=、= 或 <> 后跟一个数值。
        bool MatchesFormatCondition(std::string_view condition, double number)
        {
            const auto op = condition.find_first_not_of("<>=");
            double limit = 0;
            if (op == std::string_view::npos || !Utils::ParseFiniteDouble(condition.substr(op), limit))
                return false;

            const auto comparison = condition.substr(0, op);
            if (comparison == "<")
                return number < limit;
            if (comparison == "<=")
                return number <= limit;
            if (comparison == ">")
                return number > limit;
            if (comparison == ">=")
                return number >= limit;
            if (comparison == "=")
                return number == limit;
            return comparison == "<>" && number != limit;
        }

        // 存储自定义格式的分段信息：条件串、有没有日期标记、有没有时间标记
        struct FormatSection
        {
            std::string_view condition;
            bool date = false;
            bool time = false;

            Type TypeHint() const
            {
                return time   ? Type::DateTime
                       : date ? Type::Date
                              : Type::Skip;
            }
        };
    }

    std::string NormalizeExcelColumnName(std::string_view header, unsigned column, ExcelColumnNames &names)
    {
        std::string base;
        if (Utils::TrimAsciiWhitespace(header).empty())
            base = "column_" + std::to_string(column);
        else
        {
            if (Utils::IsAsciiDigit(header.front()))
                base = "column_";

            // 规范化后最多保留 64 个字符，汉字按一个完整 UTF-8 字符处理。
            for (unsigned length = base.size(); !header.empty() && length < kMaxColumnNameLength; ++length)
            {
                const auto width = Utils::Utf8CharWidth(header);
                const auto character = header.substr(0, width);
                const auto ch = Utils::ToAsciiLower(character.front());
                if ((width == 1 && ((ch >= 'a' && ch <= 'z') || Utils::IsAsciiDigit(ch) || ch == '_' || ch == '$')) ||
                    IsHanCharacter(character))
                    base.append(character);
                else
                    base.push_back('_');
                header.remove_prefix(width);
            }
        }

        auto [existing, inserted] = names.try_emplace(Utils::ToAsciiLower(base), 2);
        if (inserted)
            return base;

        // 截断后的前缀和新后缀也可能与已经返回的字段名重复。
        auto &next = existing->second;
        for (;;)
        {
            const auto suffix = "_" + std::to_string(next++);
            auto candidate = std::string(Utils::Utf8Prefix(base, kMaxColumnNameLength - suffix.size())) + suffix;
            if (names.try_emplace(Utils::ToAsciiLower(candidate), 2).second)
                return candidate;
        }
    }

    ExcelSampleType ClassifyExcelText(std::string_view text)
    {
        text = Utils::TrimAsciiWhitespace(text);
        if (text.empty())
            return Type::Skip;

        const auto lower = Utils::ToAsciiLower(text);
        if (lower == "n/a")
            return Type::Skip;
        // 带前导零的纯数字文本按标识保留，普通 0 和 1 优先作为数值统计。
        if (text.size() > 1 && text.front() == '0' && std::all_of(text.begin(), text.end(), Utils::IsAsciiDigit))
            return Type::Text;

        double number;
        if (Utils::ParseFiniteDouble(text, number))
            return Type::Float;
        if (lower == "true" || lower == "t" || lower == "yes" || lower == "y" ||
            lower == "false" || lower == "f" || lower == "no" || lower == "n")
            return Type::Boolean;
        return ClassifyDateText(text);
    }

    // 处理自定义数值格式的分段，识别是否为日期或时间格式，返回 Skip 表示普通数值格式。
    ExcelSampleType ClassifyExcelNumberFormat(std::string_view format, double value)
    {
        std::array<FormatSection, 3> sections{};
        unsigned section = 0;
        // 跳过引号、转义和修饰符。分号只在这些内容之外开始下一段数值格式。
        for (std::size_t i = 0; i < format.size(); ++i)
        {
            const char ch = Utils::ToAsciiLower(format[i]);
            auto &part = sections[section];
            if (ch == '"')
            {
                i = format.find('"', i + 1);
                if (i == std::string_view::npos)
                    break;
            }
            else if (ch == '\\' || ch == '_' || ch == '*')
                ++i;
            else if (ch == '[')
            {
                const auto end = format.find(']', i + 1);
                if (end == std::string_view::npos)
                    break;
                const auto token = format.substr(i + 1, end - i - 1);
                if (!token.empty() && std::string_view("<>=").find(token.front()) != std::string_view::npos)
                    part.condition = token;
                part.time |= IsElapsedTime(token);
                i = end;
            }
            else if (ch == ';')
            {
                if (section == 2)
                    break;
                ++section;
            }
            else
            {
                part.date |= ch == 'y' || ch == 'm' || ch == 'd';
                part.time |= ch == 'h' || ch == 's';
            }
        }

        if (std::any_of(sections.begin(), sections.end(), [](const FormatSection &part)
                        { return !part.condition.empty(); }))
        {
            for (unsigned i = 0; i <= section; ++i)
            {
                if (sections[i].condition.empty() || MatchesFormatCondition(sections[i].condition, value))
                    return sections[i].TypeHint();
            }
            return Type::Skip;
        }

        // 无条件段按正数、负数、零排列，共用格式时取第一段。
        const unsigned selected = section >= 1 && value < 0 ? 1 : section == 2 && value == 0 ? 2
                                                                                             : 0;
        return sections[selected].TypeHint();
    }

    // 根据内置数字格式 ID 识别日期和时间，普通数值格式返回 Skip。
    ExcelSampleType ClassifyExcelBuiltinFormat(unsigned id)
    {
        // 14-17, 27-31, 34-36, 50-58 为日期格式，18-22, 32, 33, 45-47 为日期时间格式
        if ((id >= 14 && id <= 17) || (id >= 27 && id <= 31) || (id >= 34 && id <= 36) || (id >= 50 && id <= 58))
            return Type::Date;
        if ((id >= 18 && id <= 22) || id == 32 || id == 33 || (id >= 45 && id <= 47))
            return Type::DateTime;
        return Type::Skip;
    }

    // 向列样本中添加一个样本，类型为 type，是否为日期序列。
    void ExcelColumnSamples::Add(ExcelSampleType type, bool date_serial)
    {
        if (type == Type::Skip)
            return;
        ++_counts[static_cast<unsigned>(type)];
        _has_date_serial |= date_serial;
    }

    // 采用票数最高的样本类型，空样本、并列和无法映射的日期类型使用 TEXT。
    const char *ExcelColumnSamples::ColumnType() const
    {
        const auto winner = std::max_element(_counts.begin(), _counts.end());
        if (*winner == 0 || std::count(_counts.begin(), _counts.end(), *winner) > 1)
            return "TEXT";

        switch (static_cast<Type>(winner - _counts.begin()))
        {
        case Type::Integer:
            return "BIGINT";
        case Type::Float:
            return "DOUBLE";
        case Type::Boolean:
            return "BOOLEAN";
        // 日期序列以数值返回，含序列的日期列先用 TEXT 提示存储方保留原值。
        case Type::Date:
            return _has_date_serial ? "TEXT" : "DATE";
        default:
            return "TEXT";
        }
    }
}
