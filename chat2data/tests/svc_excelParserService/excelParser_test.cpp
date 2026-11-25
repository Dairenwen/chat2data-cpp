#include "errorHandler.hpp"
#include "excelParser.hpp"

#include <OpenXLSX.hpp>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string_view>

namespace
{
    using chat2data::ExcelParser;
    using chat2Data::Chat2DataException;

    std::string Fixture(const char *name)
    {
        return (std::filesystem::path(EXCEL_TEST_FIXTURES) / name).string();
    }

    TEST(ExcelParserTest, ReadsWorksheetNamesAndRespectsRequestedOrder)
    {
        const auto path = Fixture("multipleWorksheets.xlsx");
        const std::vector<std::string> names{"销售记录", "员工名单", "备注 表"};
        EXPECT_EQ(ExcelParser::GetWorksheets(path), names);

        const auto sheets = ExcelParser::ParseExcel(path, {"备注 表", "销售记录"});
        ASSERT_EQ(sheets.size(), 2u);
        EXPECT_EQ(sheets[0].name(), "备注 表");
        EXPECT_EQ(sheets[1].name(), "销售记录");
        ASSERT_EQ(sheets[1].rows_size(), 2);
        EXPECT_EQ(sheets[1].rows(0).cells(0).value(), "键盘");
        EXPECT_EQ(sheets[1].rows(0).cells(3).value(), "1");
        EXPECT_EQ(sheets[1].rows(1).cells(3).value(), "0");
    }

    TEST(ExcelParserTest, PreservesSparsePositionsAndHeaderOnlyWorksheets)
    {
        const auto path = Fixture("edgeCases.xlsx");
        const auto sheets = ExcelParser::ParseExcel(path, {"稀疏数据", "仅标题", "尾部空列"});
        const auto &sparse = sheets[0];
        ASSERT_GE(sparse.rows_size(), 3);
        EXPECT_EQ(sparse.total_cols(), 4);
        EXPECT_EQ(sparse.columns(2).name(), "column_3");
        for (const auto &row : sparse.rows())
            EXPECT_EQ(row.cells_size(), 4);
        EXPECT_EQ(sparse.rows(0).cells(3).value(), "通过");
        EXPECT_EQ(sparse.rows(1).cells(0).type(), "Empty");
        EXPECT_EQ(sparse.rows(2).cells(1).value(), "保留");

        EXPECT_EQ(sheets[1].total_rows(), 1);
        EXPECT_EQ(sheets[1].total_cols(), 3);
        EXPECT_TRUE(sheets[1].rows().empty());
        EXPECT_EQ(sheets[2].total_cols(), 4);
        EXPECT_EQ(sheets[2].rows(0).cells(3).type(), "Empty");
        EXPECT_THROW(ExcelParser::ParseExcel(path, {"空表"}), Chat2DataException);
    }

    TEST(ExcelParserTest, PreservesUnderlyingValuesAndFormulaCaches)
    {
        const auto values = ExcelParser::ParseExcel(Fixture("edgeCases.xlsx"), {"数据类型"});
        ASSERT_EQ(values[0].rows_size(), 1);
        ASSERT_EQ(values[0].rows(0).cells_size(), 8);
        const auto &row = values[0].rows(0);
        EXPECT_EQ(row.cells(1).value(), "9007199254740993");
        EXPECT_EQ(row.cells(1).type(), "Integer");
        EXPECT_EQ(row.cells(2).value(), "12.125");
        EXPECT_EQ(row.cells(2).type(), "Float");
        EXPECT_EQ(row.cells(5).value(), "00123");
        EXPECT_EQ(row.cells(5).type(), "String");
        EXPECT_EQ(row.cells(6).value(), "#DIV/0!");
        EXPECT_EQ(row.cells(6).type(), "Error");
        EXPECT_TRUE(row.cells(7).value().empty());
        EXPECT_EQ(row.cells(7).type(), "Empty");

        const auto sheets = ExcelParser::ParseExcel(Fixture("extendedCases.xlsx"), {"公式缓存", "日期"});
        EXPECT_EQ(sheets[0].rows(0).cells(0).value(), "42");
        EXPECT_EQ(sheets[0].rows(0).cells(1).value(), "0");
        EXPECT_EQ(sheets[1].rows(0).cells(0).value(), "45292.5");
        EXPECT_EQ(sheets[1].rows(0).cells(0).type(), "Float");
        // 样本的单元格没有日期样式，数值本身仍按普通浮点数推断。
        EXPECT_EQ(sheets[1].columns(0).type(), "DOUBLE");
    }

    class ExcelRulesTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "chat2data-excel-rules-XXXXXX").string();
            ASSERT_NE(mkdtemp(pattern.data()), nullptr);
            _directory = pattern;
            _file = (_directory / "rules.xlsx").string();
        }

        void TearDown() override
        {
            if (!_directory.empty())
                std::filesystem::remove_all(_directory);
        }

        std::filesystem::path _directory;
        std::string _file;
    };

    TEST_F(ExcelRulesTest, NormalizesHeadersAndKeepsMergedPositions)
    {
        OpenXLSX::XLDocument document;
        document.create(_file, false);
        auto sheet = document.workbook().worksheet("Sheet1");
        sheet.cell("B1").value() = "2026销售额";
        sheet.cell("C1").value() = "销售额(元)";
        sheet.cell("D1").value() = "Name";
        sheet.cell("E1").value() = "Name_2";
        sheet.cell("F1").value() = "name";
        std::string long_name;
        for (int i = 0; i < 65; ++i)
            long_name += "字";
        sheet.cell("G1").value() = long_name;
        sheet.cell("H1").value() = long_name + "外";
        sheet.cell("I1").value() = "select";
        sheet.cell("J1").value() = "aé😀b";
        sheet.cell("A2").value() = "左上角";
        sheet.cell("B2").value() = "合并区值";
        sheet.mergeCells("A2:B3");
        sheet.cell("C2").formula() = "1+2";
        sheet.cell("C2").value().clear();
        // 仅供显示的样式位于内容范围外。
        sheet.cell("T50").setCellFormat(document.styles().cellFormats().create());
        document.save();
        document.close();

        const auto sheets = ExcelParser::ParseExcel(_file, {"Sheet1"});
        const auto &result = sheets.front();
        ASSERT_EQ(result.total_cols(), 10);
        ASSERT_EQ(result.total_rows(), 3);
        const std::vector<std::string> names{
            "column_1", "column_2026销售额", "销售额_元_", "Name", "Name_2", "name_3",
            long_name.substr(0, 64 * std::string_view("字").size()),
            long_name.substr(0, 62 * std::string_view("字").size()) + "_2", "select", "a__b"};
        ASSERT_EQ(result.columns_size(), static_cast<int>(names.size()));
        for (std::size_t col = 0; col < names.size(); ++col)
            EXPECT_EQ(result.columns(col).name(), names[col]) << col;
        EXPECT_EQ(result.rows(0).cells(0).value(), "左上角");
        EXPECT_EQ(result.rows(0).cells(1).type(), "Empty");
        EXPECT_EQ(result.rows(0).cells(2).type(), "Empty");
        EXPECT_EQ(result.rows(1).cells(0).type(), "Empty");
    }

    TEST_F(ExcelRulesTest, SamplesTextAndNumberFormatsInFirstHundredDataRows)
    {
        OpenXLSX::XLDocument document;
        document.create(_file, false);
        auto sheet = document.workbook().worksheet("Sheet1");
        for (unsigned col = 1; col <= 16; ++col)
            sheet.cell(1, col).value() = "header" + std::to_string(col);

        sheet.cell("A2").value() = " 123 ";
        sheet.cell("B2").value() = "00123";
        sheet.cell("C2").value() = " YeS ";
        sheet.cell("C3").value() = "f";
        sheet.cell("C4").value() = "n/A";
        sheet.cell("D2").value() = "1";
        sheet.cell("D3").value() = "0";
        const char *const dates[]{"2024-02-29", "2024/02/29", "02-29-2024", "02/29/2024", "2024.02.29"};
        unsigned row_number = 2;
        for (const auto *date : dates)
            sheet.cell(row_number++, 5).value() = date;
        sheet.cell("F2").value() = "2023-02-29";
        sheet.cell("F3").value() = "04/31/2024";
        sheet.cell("G2").value() = "2024-02-29 23:59:59";
        sheet.cell("G3").value() = "2024-02-29 24:00:01";
        sheet.cell("H2").value() = 7;
        sheet.cell("H3").value() = 7.5;
        sheet.cell("I2").value() = true;
        sheet.cell("I102").value() = "第 101 个数据行";
        sheet.cell("J2").value() = 45292;
        sheet.cell("J3").value() = "2024-02-29";
        sheet.cell("K2").value() = 45292;
        sheet.cell("L2").value() = 123.5;
        sheet.cell("M2").value() = 0.15;
        sheet.cell("N2").value() = 1234.5;
        sheet.cell("O2").value() = 0;
        sheet.cell("P2").value() = 1.5;

        auto &styles = document.styles();
        auto set_format = [&](const char *address, unsigned id, const char *format = nullptr)
        {
            if (format)
            {
                const auto index = styles.numberFormats().create();
                ASSERT_TRUE(styles.numberFormats()[index].setNumberFormatId(id));
                ASSERT_TRUE(styles.numberFormats()[index].setFormatCode(format));
            }
            const auto style = styles.cellFormats().create();
            ASSERT_TRUE(styles.cellFormats()[style].setNumberFormatId(id));
            ASSERT_TRUE(sheet.cell(address).setCellFormat(style));
        };
        set_format("J2", 14);
        set_format("K2", 165, "[>=1]yyyy-mm-dd;0");
        set_format("L2", 166, "0.00 \"days\"");
        set_format("M2", 10);
        set_format("N2", 167, "[$¥-804]#,##0.00");
        set_format("O2", 168, "yyyy-mm-dd;0;@");
        set_format("P2", 46);
        document.save();
        document.close();

        const auto sheets = ExcelParser::ParseExcel(_file, {"Sheet1"});
        const auto &result = sheets.front();
        const std::vector<std::string> types{
            "DOUBLE", "TEXT", "BOOLEAN", "DOUBLE", "DATE", "TEXT",
            "TEXT", "TEXT", "BOOLEAN", "TEXT", "TEXT", "DOUBLE",
            "DOUBLE", "DOUBLE", "TEXT", "TEXT"};
        ASSERT_EQ(result.columns_size(), static_cast<int>(types.size()));
        for (unsigned i = 0; i < types.size(); ++i)
            EXPECT_EQ(result.columns(i).type(), types[i]) << result.columns(i).name();

        EXPECT_EQ(result.rows(0).cells(0).value(), " 123 ");
        EXPECT_EQ(result.rows(0).cells(2).value(), " YeS ");
        EXPECT_EQ(result.rows(0).cells(9).value(), "45292");
        EXPECT_EQ(result.rows(0).cells(9).type(), "Integer");
        EXPECT_DOUBLE_EQ(std::stod(result.rows(0).cells(12).value()), 0.15);
        EXPECT_EQ(result.rows(0).cells(13).value(), "1234.5");
        EXPECT_EQ(result.rows(0).cells(14).value(), "0");
        EXPECT_EQ(result.rows(0).cells(15).value(), "1.5");
        ASSERT_EQ(result.rows_size(), 101);
        EXPECT_EQ(result.rows(100).cells(8).value(), "第 101 个数据行");
    }

    TEST_F(ExcelRulesTest, ReadsInlineRichTextNativeDatesAndImplicitReferences)
    {
        OpenXLSX::XLDocument document;
        document.create(_file, false);
        document.close();

        // 写入生成器实际可使用的 XML 形式，覆盖 OpenXLSX 写入接口未直接生成的单元格。
        const char *worksheet = R"xml(
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <sheetData>
    <row><c t="inlineStr"><is><t>表头</t></is></c></row>
    <row>
      <c t="inlineStr"><is><t xml:space="preserve"> 内联 </t></is></c>
      <c t="inlineStr"><is><r><t>收</t></r><r><t>入</t></r></is></c>
      <c><v>12345.67</v></c>
      <c><v>1E+3</v></c>
      <c t="d"><v>2024-02-29</v></c>
      <c t="d"><v>2024-02-29T13:00:00</v></c>
      <c t="b"><v>0</v></c>
    </row>
    <row r="4"><c r="C4"/><c><v>5</v></c></row>
  </sheetData>
</worksheet>)xml";
        OpenXLSX::XLZipArchive archive;
        archive.open(_file);
        archive.deleteEntry("xl/worksheets/sheet1.xml");
        archive.addEntry("xl/worksheets/sheet1.xml", worksheet);
        archive.save(_file);
        archive.close();

        const auto sheets = ExcelParser::ParseExcel(_file, {"Sheet1"});
        const auto &result = sheets.front();
        ASSERT_EQ(result.rows_size(), 3);
        ASSERT_EQ(result.columns_size(), 7);
        const auto &row = result.rows(0);
        EXPECT_EQ(row.cells(0).value(), " 内联 ");
        EXPECT_EQ(row.cells(1).value(), "收入");
        EXPECT_EQ(row.cells(2).type(), "Float");
        EXPECT_DOUBLE_EQ(std::stod(row.cells(2).value()), 12345.67);
        EXPECT_EQ(row.cells(3).value(), "1000");
        EXPECT_EQ(result.columns(4).type(), "DATE");
        EXPECT_EQ(row.cells(4).type(), "String");
        EXPECT_EQ(row.cells(4).value(), "2024-02-29");
        EXPECT_EQ(row.cells(5).value(), "2024-02-29T13:00:00");
        EXPECT_EQ(row.cells(6).type(), "Boolean");
        EXPECT_EQ(row.cells(6).value(), "0");
        EXPECT_EQ(result.rows(1).cells(3).type(), "Empty");
        EXPECT_EQ(result.rows(2).cells(3).value(), "5");
    }
}
