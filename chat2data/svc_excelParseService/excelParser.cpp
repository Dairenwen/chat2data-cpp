#include "excelParser.hpp"

#include "errorHandler.hpp"
#include "excelColumn.hpp"

#include <OpenXLSX.hpp>
#include <XLXmlParser.hpp>
#include <brpc/protocol.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace chat2data
{
    namespace
    {
        using chat2Data::Chat2DataException;
        using chat2Data::ErrorCode;
        using chat2Data::WorksheetData;
        using OpenXLSX::XLValueType;

        constexpr unsigned kSampleRows = 100; // 从首行表头后的第一条数据开始按行采样

        // 继承 XLXmlFile 以访问 protected 的 xmlDocument()，并提供工作表数据节点。
        class WorksheetXml : private OpenXLSX::XLXmlFile
        {
        public:
            explicit WorksheetXml(const OpenXLSX::XLWorksheet &sheet) : XLXmlFile(sheet)
            {
            }

            // 获取工作表数据节点，若无数据则返回空节点。
            OpenXLSX::XMLNode Data() const
            {
                return xmlDocument().document_element().child("sheetData");
            }
        };

        // 打开工作簿并返回按顺序排列的工作表名称，若无工作表则抛出异常
        std::vector<std::string> OpenWorkbook(OpenXLSX::XLDocument &document, const std::string &path)
        {
            std::vector<std::string> names;
            try
            {
                document.open(path);
                names = document.workbook().worksheetNames();
            }
            catch (const std::exception &)
            {
                throw Chat2DataException(ErrorCode::EXCEL_WORKBOOK_INVALID);
            }
            if (names.empty())
                throw Chat2DataException(ErrorCode::EXCEL_WORKBOOK_INVALID);
            return names;
        }

        // 将 double 转为十进制字符串，保证往返还原，避免使用 std::to_string 受 locale 影响。
        std::string FormatDouble(double number)
        {
            // general 格式输出可往返还原 double 的十进制表示，与进程区域设置无关。
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::general, std::numeric_limits<double>::max_digits10);
            if (result.ec != std::errc{})
                throw Chat2DataException(ErrorCode::EXCEL_PARSE_FAILED);
            return std::string(buffer, result.ptr);
        }

        // 解析后的单元格结构体，包含类型、值、样本类型和日期序列标识
        struct ParsedCell
        {
            const char *type;
            std::string value;
            ExcelSampleType sample = ExcelSampleType::Skip;
            bool date_serial = false;
        };

        // 读取工作簿的样式信息，样式信息一般为自定义格式，内置格式可通过 ID 识别
        class WorkbookNumberFormats
        {
        public:
            explicit WorkbookNumberFormats(OpenXLSX::XLStyles &styles) : _cell_formats(styles.cellFormats())
            {
                const auto &formats = styles.numberFormats();
                for (std::size_t i = 0; i < formats.count(); ++i)
                {
                    const auto format = formats[i];
                    _custom_formats.emplace(format.numberFormatId(), format.formatCode());
                }
            }

            // 根据单元格样式索引和数值识别日期和时间格式，返回 Skip 表示非日期类型。
            ExcelSampleType Classify(std::size_t style, double number) const
            {
                // 样式索引超出范围表示单元格未设置样式，按普通数值处理
                if (style >= _cell_formats.count())
                    return ExcelSampleType::Skip;
                const auto id = _cell_formats[style].numberFormatId();
                const auto custom = _custom_formats.find(id);
                // 找到id说明是自定义格式，未找到则按内置格式识别日期和时间
                return custom == _custom_formats.end() ? ClassifyExcelBuiltinFormat(id) : ClassifyExcelNumberFormat(custom->second, number);
            }

        private:
            const OpenXLSX::XLCellFormats &_cell_formats;              // 样式表中按索引排列的单元格样式
            std::unordered_map<unsigned, std::string> _custom_formats; // ID 到格式字符串的映射
        };

        // 解析单元格，返回类型、值、样本类型和日期序列标识
        ParsedCell ReadCell(const OpenXLSX::XMLNode &node, const OpenXLSX::XLSharedStrings &strings, const WorkbookNumberFormats &formats, bool sample)
        {
            const auto cached = node.child("v");
            const std::string_view raw(cached.text().get());
            const std::string_view xml_type(node.attribute("t").value());
            // 公式只读取已保存的结果。带 str 类型的空值节点可表示缓存的空字符串。
            if (node.child("f") && (!cached || (raw.empty() && xml_type != "str")))
                return {"Empty", {}};

            const OpenXLSX::XLCell cell(node, strings);
            const auto &value = cell.value();
            switch (value.type())
            {
            case XLValueType::Empty:
                return {"Empty", {}};
            case XLValueType::Boolean:
                return {"Boolean", value.get<bool>() ? "1" : "0", ExcelSampleType::Boolean};
            case XLValueType::Integer:
            case XLValueType::Float:
            {
                // 某些 Excel 生成器使用不含小数点的科学计数法，按原始文本完整读取。
                const bool floating = value.type() == XLValueType::Float || raw.find_first_of("eE") != raw.npos;
                std::int64_t integer;
                double number;
                auto numeric_text = raw;
                if (!numeric_text.empty() && numeric_text.front() == '+')
                    numeric_text.remove_prefix(1);
                if (numeric_text.empty())
                    return {"Empty", {}};
                const auto end = numeric_text.data() + numeric_text.size();
                const auto parsed = floating ? std::from_chars(numeric_text.data(), end, number) : std::from_chars(numeric_text.data(), end, integer);
                if (parsed.ec != std::errc{} || parsed.ptr != end)
                    throw Chat2DataException(ErrorCode::EXCEL_PARSE_FAILED);
                if (!floating)
                    number = static_cast<double>(integer);

                // 识别日期样式用于推断列类型，响应保留数值类型及 Excel 日期序列原文。
                const auto date = formats.Classify(cell.cellFormat(), number);
                const bool date_serial = date != ExcelSampleType::Skip;
                const auto numeric = floating ? ExcelSampleType::Float : ExcelSampleType::Integer;
                const char *type = floating ? "Float" : "Integer";
                if (date_serial)
                    return {type, std::string(raw), date, true};
                return {type, floating ? FormatDouble(number) : std::to_string(integer), numeric};
            }
            case XLValueType::String:
            {
                auto text = value.get<std::string>();
                const auto type = sample ? ClassifyExcelText(text) : ExcelSampleType::Skip;
                return {"String", std::move(text), type};
            }
            case XLValueType::Error:
                return {"Error", value.get<std::string>()};
            }
            throw Chat2DataException(ErrorCode::EXCEL_PARSE_FAILED);
        }

        // 遍历单元格，过滤无内容仅带样式的格子；合并格只回调左上角，回调传入节点、单元格坐标、合并块右下坐标
        template <typename Visit>
        void VisitCells(const OpenXLSX::XMLNode &data, const OpenXLSX::XLMergeCells &merges, Visit visit)
        {
            for (auto row = data.child("row"); row; row = row.next_sibling("row"))
            {
                for (auto node = row.child("c"); node; node = node.next_sibling("c"))
                {
                    // 保留显式记录的空单元格，只有样式的单元格留在数据范围之外。
                    if (node.attribute("s") && !node.child("v") && !node.child("is") && !node.child("f"))
                        continue;

                    const OpenXLSX::XLCellReference address(node.attribute("r").value());
                    const auto merge = merges.findMergeByCell(address);
                    auto last = address;
                    if (merge != OpenXLSX::XLMergeNotFound)
                    {
                        // 以左上角保存合并值，并将右下角坐标计入工作表范围。
                        const std::string range(merges[merge]);
                        const auto colon = range.find(':');
                        if (OpenXLSX::XLCellReference(range.substr(0, colon)) != address)
                            continue;
                        if (colon != std::string::npos)
                            last = OpenXLSX::XLCellReference(range.substr(colon + 1));
                    }
                    visit(node, address, last);
                }
            }
        }

        // 读取工作表数据，按行列范围填充空值，再用已有单元格覆盖对应坐标，返回工作表数据结构
        WorksheetData ReadWorksheet(OpenXLSX::XLWorksheet &sheet, const OpenXLSX::XLSharedStrings &strings, const WorkbookNumberFormats &formats, const std::string &name)
        {
            // 1.获取工作表数据节点以及合并单元格信息
            const auto data = WorksheetXml(sheet).Data();
            const auto &merges = sheet.merges();
            unsigned rows = 0;
            unsigned columns = 0;
            // 2. 遍历全部有效单元格（含合并格），顺带靠合并块的右下边界，算出整张表整体行列范围
            VisitCells(data, merges, [&](const auto &, const auto &, const auto &last)
                       {
                           rows = std::max(rows, static_cast<unsigned>(last.row()));
                           columns = std::max(columns, static_cast<unsigned>(last.column())); });

            if (rows == 0)
                throw Chat2DataException(ErrorCode::EXCEL_EMPTY_WORKSHEET);

            // 每个回复单元格至少占用 9 个 Protobuf 字节。先排除无法由 brpc 发送的矩形。
            constexpr auto kMinCellBytes = 9;
            if (std::uint64_t(rows - 1) * columns > brpc::FLAGS_max_body_size / kMinCellBytes)
                throw Chat2DataException(ErrorCode::EXCEL_PARSE_FAILED);

            // 3. 解析单元格，按行列范围填充空值，再用已有单元格覆盖对应坐标，返回工作表数据结构
            WorksheetData result;
            result.set_name(name);
            result.set_total_rows(rows);
            result.set_total_cols(columns);
            std::vector<std::string> headers(columns);
            std::vector<ExcelColumnSamples> samples(columns);
            // 先按实际行列范围填充空值
            result.mutable_rows()->Reserve(rows - 1);
            for (unsigned row = 1; row < rows; ++row)
            {
                auto *cells = result.add_rows()->mutable_cells();
                cells->Reserve(columns);
                for (unsigned column = 0; column < columns; ++column)
                    cells->Add()->set_type("Empty");
            }

            // 4. 遍历所有有效单元格，把第 1 行当成表头存入 headers，其余行写入结果结构体；
            // 同时对前若干行采集单元格样本，准备按列推断类型，空单元格不计入样本
            VisitCells(data, merges, [&](const auto &node, const auto &address, const auto &)
                       {
                           const auto row = address.row();
                           const auto column = address.column() - 1;
                           const bool sample = row > 1 && row <= kSampleRows + 1;
                           auto cell = ReadCell(node, strings, formats, sample);
                           if (row == 1)
                           {
                               headers[column] = std::move(cell.value);
                               return;
                           }

                           auto *destination = result.mutable_rows(row - 2)->mutable_cells(column);
                           destination->set_type(cell.type);
                           destination->set_value(std::move(cell.value));
                           if (sample)
                               samples[column].Add(cell.sample, cell.date_serial); });
            // 5. 整理表头和列类型
            ExcelColumnNames names;
            for (unsigned column = 0; column < columns; ++column)
            {
                auto *info = result.add_columns();
                info->set_name(NormalizeExcelColumnName(headers[column], column + 1, names));
                info->set_type(samples[column].ColumnType());
            }
            return result;
        }
    }

    std::vector<std::string> ExcelParser::GetWorksheets(const std::string &path)
    {
        OpenXLSX::XLDocument document;
        return OpenWorkbook(document, path);
    }

    std::vector<WorksheetData> ExcelParser::ParseExcel(const std::string &path, const std::vector<std::string> &worksheets)
    {
        OpenXLSX::XLDocument document;
        // 1.打开工作簿，获取按顺序排列的工作表名称，若无工作表则抛出异常
        const auto names = OpenWorkbook(document, path);
        auto workbook = document.workbook();
        // 2.建立数字格式与样式映射，如果为自定义格式则保存格式字符串，内置格式按 ID 识别日期和时间。
        const WorkbookNumberFormats formats(document.styles());
        // 3.按请求顺序解析工作薄
        std::vector<WorksheetData> result;
        result.reserve(worksheets.size());
        for (const auto &name : worksheets)
        {
            if (std::find(names.begin(), names.end(), name) == names.end())
                throw Chat2DataException(ErrorCode::EXCEL_WORKSHEET_NOT_FOUND);
            // 获取工作表
            auto sheet = workbook.worksheet(name);
            try
            {
                result.push_back(ReadWorksheet(sheet, document.sharedStrings(), formats, name));
            }
            catch (const OpenXLSX::XLException &)
            {
                throw Chat2DataException(ErrorCode::EXCEL_PARSE_FAILED);
            }
        }
        return result;
    }
}
