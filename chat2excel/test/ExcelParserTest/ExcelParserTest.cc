#include "ExeclParser.h"

#include <OpenXLSX.hpp>
#include <bite_scaffold/log.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

/**
 * @brief 一个输入单元格及独立指定的 CSV 预期字段。
 *
 * csvField 中的特殊字符由测试数据显式转义，不调用 ExcelParser 的任何方法。
 * formula 非空时，先写公式，再写 value 作为缓存，避免依赖 Excel 软件计算公式。
 */
struct ExpectedCell {
    OpenXLSX::XLCellValue value;
    std::string csvField;
    std::string formula;
};

struct WorksheetData {
    std::string name;
    std::vector<std::vector<ExpectedCell>> rows;
};

/// 普通文本；含 CSV 特殊字符的文本使用下方带独立预期值的构造方式。
ExpectedCell textCell(const std::string& text) {
    return {OpenXLSX::XLCellValue(text), text, {}};
}

ExpectedCell escapedTextCell(const std::string& text, const std::string& csvField) {
    return {OpenXLSX::XLCellValue(text), csvField, {}};
}

ExpectedCell integerCell(std::int64_t value) {
    return {OpenXLSX::XLCellValue(value), std::to_string(value), {}};
}

ExpectedCell decimalCell(double value, const std::string& csvField) {
    return {OpenXLSX::XLCellValue(value), csvField, {}};
}

ExpectedCell booleanCell(bool value) {
    return {OpenXLSX::XLCellValue(value), value ? "true" : "false", {}};
}

ExpectedCell errorCell(const std::string& error) {
    ExpectedCell result;
    result.value.setError(error);
    result.csvField = error;
    return result;
}

ExpectedCell formulaCell(ExpectedCell cachedValue, const std::string& formula) {
    cachedValue.formula = formula;
    return cachedValue;
}

/** @brief 将已经指定的预期字段拼接为完整结果，不读取工作簿或被测类。 */
std::string expectedWorksheetCsv(const WorksheetData& worksheet) {
    std::string expected;
    for (std::size_t row = 0; row < worksheet.rows.size(); ++row) {
        if (row != 0) expected.push_back('\n');
        const auto& cells = worksheet.rows[row];
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column != 0) expected.push_back(',');
            expected += cells[column].csvField;
        }
    }
    return expected;
}

/**
 * @brief 建立五张相互关联的业务表，主表包含 1000 笔订单、32 列及合计行。
 *
 * 原始值与预期文本共同描述测试输入；名称、引号、换行字段的 CSV 预期均明确写出。
 * 金额使用整数、折扣使用 0/0.5，使订单金额校验不受二进制小数舍入影响。
 * 高精度浮点数另有专项用例，不将财务显示格式与底层数值读取混为一谈。
 */
const std::vector<WorksheetData>& businessWorksheets() {
    static const std::vector<WorksheetData> worksheets = [] {
        const std::vector<std::string> customerNames{"张三", "Acme, Inc.", "研发\"中心\""};
        const std::vector<std::string> customerCsv{"张三", "\"Acme, Inc.\"", "\"研发\"\"中心\"\"\""};
        const std::vector<std::string> productNames{"机械键盘", "显示器", "办公椅", "多功能\"支架\""};
        const std::vector<std::string> productCsv{"机械键盘", "显示器", "办公椅", "\"多功能\"\"支架\"\"\""};
        const std::vector<std::string> categories{"外设", "显示设备", "家具", "配件"};
        const std::vector<std::string> cities{"上海", "北京", "深圳", "杭州"};
        const std::vector<std::string> provinces{"上海", "北京", "广东", "浙江"};
        const std::vector<std::string> salespeople{"李四", "王五", "赵六"};
        const std::vector<std::string> channels{"网页", "门店", "API"};
        const std::vector<std::int64_t> prices{99, 129, 45, 8};
        const std::vector<std::string> notes{"", "加急,周五前送达", "收货前联系\"前台\"", "第一行\n第二行"};
        const std::vector<std::string> noteCsv{"", "\"加急,周五前送达\"", "\"收货前联系\"\"前台\"\"\"", "\"第一行\n第二行\""};

        WorksheetData orders{"订单明细", {}};
        std::vector<ExpectedCell> title(32);
        title[0] = textCell("2026年10月销售订单明细（自动生成）");
        orders.rows.push_back(title);
        std::vector<ExpectedCell> period(32);
        period[0] = textCell("统计期间");
        period[1] = textCell("2026-10");
        orders.rows.push_back(period);
        const std::vector<std::string> headers{
            "订单编号", "订单日期", "客户编号", "客户名称", "城市", "商品编号", "商品名称", "分类",
            "数量", "单价", "原价金额", "折扣率", "优惠金额", "运费", "应付金额", "币种",
            "已付款", "订单状态", "销售员", "渠道", "省份", "收件人", "电话", "备注",
            "外部流水号", "退货数量", "发票抬头", "发票已开", "预留空列", "税率", "预计发货日期", "校验备注"
        };
        std::vector<ExpectedCell> headerCells;
        for (const auto& header : headers) headerCells.push_back(textCell(header));
        orders.rows.push_back(headerCells);

        std::int64_t grossTotal = 0;
        std::int64_t discountTotal = 0;
        std::int64_t shippingTotal = 0;
        std::int64_t payableTotal = 0;
        std::int64_t paidOrderCount = 0;
        std::int64_t unpaidTotal = 0;
        for (std::int64_t index = 0; index < 1000; ++index) {
            const auto customer = static_cast<std::size_t>(index % 3);
            const auto product = static_cast<std::size_t>(index % 4);
            const auto quantity = 2 * (index % 5 + 1);
            const auto gross = quantity * prices[product];
            const bool discounted = index % 2 != 0;
            const auto discount = discounted ? gross / 2 : 0;
            const auto shipping = index % 3 * 10;
            const auto payable = gross - discount + shipping;
            const bool paid = index % 4 != 0;
            const auto dateSerial = 45900 + index % 28;
            const auto row = std::to_string(orders.rows.size() + 1);
            const auto customerRow = std::to_string(customer + 3);
            const auto productRow = std::to_string(product + 3);
            const auto note = static_cast<std::size_t>(index % 4);

            std::vector<ExpectedCell> cells{
                textCell("ORD-202610-" + std::to_string(100001 + index)),
                decimalCell(dateSerial + 0.5, std::to_string(dateSerial) + ".5"),
                textCell("C00" + std::to_string(customer + 1)),
                formulaCell(escapedTextCell(customerNames[customer], customerCsv[customer]), "'客户档案'!B" + customerRow),
                textCell(cities[product]), textCell("P00" + std::to_string(product + 1)),
                formulaCell(escapedTextCell(productNames[product], productCsv[product]), "'商品目录'!B" + productRow),
                textCell(categories[product]), integerCell(quantity), integerCell(prices[product]),
                formulaCell(integerCell(gross), "I" + row + "*J" + row),
                decimalCell(discounted ? 0.5 : 0.0, discounted ? "0.5" : "0"),
                formulaCell(integerCell(discount), "K" + row + "*L" + row),
                integerCell(shipping), formulaCell(integerCell(payable), "K" + row + "-M" + row + "+N" + row),
                textCell("CNY"), booleanCell(paid), textCell(paid ? "已完成" : "待付款"),
                textCell(salespeople[customer]), textCell(channels[customer]), textCell(provinces[product]),
                escapedTextCell(customerNames[customer], customerCsv[customer]),
                textCell("0138000" + std::to_string(100001 + index)),
                escapedTextCell(notes[note], noteCsv[note]),
                integerCell(std::int64_t{9007199254740993} + index), integerCell(index % 17 == 0 ? 2 : 0),
                escapedTextCell(customerNames[customer], customerCsv[customer]), booleanCell(discounted),
                {}, decimalCell(0.125, "0.125"),
                decimalCell(dateSerial + 2.5, std::to_string(dateSerial + 2) + ".5"),
                index % 101 == 0 ? errorCell("#N/A") : textCell("正常")
            };
            orders.rows.push_back(std::move(cells));
            grossTotal += gross;
            discountTotal += discount;
            shippingTotal += shipping;
            payableTotal += payable;
            paidOrderCount += paid ? 1 : 0;
            unpaidTotal += paid ? 0 : payable;
            // 每 200 笔订单插入一行完全没有单元格的空行，验证稀疏行不会丢失。
            if ((index + 1) % 200 == 0 && index != 999) {
                orders.rows.emplace_back(32);
            }
        }
        const auto lastOrderRow = std::to_string(orders.rows.size());
        std::vector<ExpectedCell> totals(32);
        totals[0] = textCell("合计");
        totals[10] = formulaCell(integerCell(grossTotal), "SUM(K4:K" + lastOrderRow + ")");
        totals[12] = formulaCell(integerCell(discountTotal), "SUM(M4:M" + lastOrderRow + ")");
        totals[13] = formulaCell(integerCell(shippingTotal), "SUM(N4:N" + lastOrderRow + ")");
        totals[14] = formulaCell(integerCell(payableTotal), "SUM(O4:O" + lastOrderRow + ")");
        orders.rows.push_back(totals);

        WorksheetData products{"商品目录", {}};
        products.rows.push_back({textCell("商品目录与库存"), {}, {}, {}, {}, {}, {}});
        products.rows.push_back({textCell("商品编号"), textCell("名称"), textCell("分类"), textCell("单价"), textCell("库存"), textCell("在售"), textCell("说明")});
        for (std::size_t index = 0; index < productNames.size(); ++index) {
            products.rows.push_back({textCell("P00" + std::to_string(index + 1)),
                escapedTextCell(productNames[index], productCsv[index]), textCell(categories[index]),
                integerCell(prices[index]), integerCell(1000 - static_cast<std::int64_t>(index) * 200),
                booleanCell(index != 3), escapedTextCell("仓库,需复核", "\"仓库,需复核\"")});
        }

        WorksheetData customers{"客户档案", {}};
        customers.rows.push_back({textCell("客户主数据"), {}, {}, {}, {}, {}, {}, {}});
        customers.rows.push_back({textCell("客户编号"), textCell("名称"), textCell("电话"), textCell("地区"), textCell("等级"), textCell("注册日期"), textCell("启用"), textCell("备注")});
        for (std::size_t index = 0; index < customerNames.size(); ++index) {
            customers.rows.push_back({textCell("C00" + std::to_string(index + 1)),
                escapedTextCell(customerNames[index], customerCsv[index]), textCell("0013800012345"),
                textCell(cities[index]), textCell(index == 0 ? "普通" : "VIP"),
                decimalCell(45000.5 + index, std::to_string(45000 + index) + ".5"), booleanCell(true),
                escapedTextCell("首选联系人\n备用联系人", "\"首选联系人\n备用联系人\"")});
        }

        WorksheetData summary{"月度汇总", {
            {textCell("月度经营汇总"), {}, {}, {}, {}, {}},
            {textCell("指标"), textCell("数值"), textCell("口径"), {}, {}, textCell("备注")},
            {textCell("订单数"), formulaCell(integerCell(1000), "COUNTA('订单明细'!A4:A" + lastOrderRow + ")"), textCell("不含分隔空行"), {}, {}, {}},
            {textCell("原价金额"), formulaCell(integerCell(grossTotal), "SUM('订单明细'!K4:K" + lastOrderRow + ")"), textCell("含隐藏行"), {}, {}, {}},
            {textCell("优惠金额"), formulaCell(integerCell(discountTotal), "SUM('订单明细'!M4:M" + lastOrderRow + ")"), {}, {}, {}, {}},
            {textCell("运费"), formulaCell(integerCell(shippingTotal), "SUM('订单明细'!N4:N" + lastOrderRow + ")"), {}, {}, {}, {}},
            {textCell("应付金额"), formulaCell(integerCell(payableTotal), "SUM('订单明细'!O4:O" + lastOrderRow + ")"), {}, {}, {}, {}},
            {textCell("已付款订单"), formulaCell(integerCell(paidOrderCount), "COUNTIF('订单明细'!Q4:Q" + lastOrderRow + ",TRUE)"), {}, {}, {}, {}},
            {textCell("欠款金额"), formulaCell(integerCell(unpaidTotal), "SUMIF('订单明细'!Q4:Q" + lastOrderRow + ",FALSE,'订单明细'!O4:O" + lastOrderRow + ")"), {}, {}, {}, {}}
        }};

        WorksheetData exceptions{"说明与异常", {
            {textCell("数据字典与异常样例"), {}, {}, {}, {}, {}, {}, {}},
            {textCell("场景"), textCell("整数"), textCell("小数"), textCell("布尔"), textCell("文本"), textCell("错误"), textCell("日期"), textCell("备注")},
            {textCell("负数与零"), integerCell(-100), decimalCell(-0.125, "-0.125"), booleanCell(false), textCell("00000"), {}, {}, {}},
            {textCell("异常计算"), integerCell(0), {}, booleanCell(true), textCell("待修复"), errorCell("#DIV/0!"), {}, {}},
            {textCell("缺失关联"), {}, {}, {}, {}, errorCell("#N/A"), {}, escapedTextCell("客户,商品缺失", "\"客户,商品缺失\"")},
            {textCell("非法值"), {}, {}, {}, {}, errorCell("#VALUE!"), {}, escapedTextCell("检查\"类型\"", "\"检查\"\"类型\"\"\"")},
            {textCell("时间字段"), {}, {}, {}, {}, {}, decimalCell(45900.5, "45900.5"), textCell("底层日期序列数")}
        }};
        return std::vector<WorksheetData>{std::move(orders), std::move(products), std::move(customers), std::move(summary), std::move(exceptions)};
    }();
    return worksheets;
}

/** @brief 临时禁用日志器；即使用例致命断言提前返回，也恢复原来的全局日志器。 */
class ScopedLoggerPause {
public:
    ScopedLoggerPause() : savedLogger(std::move(bitelog::g_logger)) {}
    ~ScopedLoggerPause() { bitelog::g_logger = std::move(savedLogger); }
    ScopedLoggerPause(const ScopedLoggerPause&) = delete;
    ScopedLoggerPause& operator=(const ScopedLoggerPause&) = delete;
private:
    std::shared_ptr<spdlog::logger> savedLogger;
};

/** @brief 可移植的逗号小数点环境，无需系统安装特定国家的 locale。 */
class CommaDecimalPunctuation : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

class ScopedGlobalLocale {
public:
    explicit ScopedGlobalLocale(const std::locale& replacement)
        : savedLocale(std::locale::global(replacement)) {}
    ~ScopedGlobalLocale() { std::locale::global(savedLocale); }
    ScopedGlobalLocale(const ScopedGlobalLocale&) = delete;
    ScopedGlobalLocale& operator=(const ScopedGlobalLocale&) = delete;
private:
    std::locale savedLocale;
};

/** @brief 按二进制读取完整文件，用于验证解析操作没有重写原始 xlsx 压缩包。 */
std::string readFileBytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取测试文件：" + path);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) throw std::runtime_error("读取测试文件失败：" + path);
    return bytes;
}

const std::vector<double>& precisionSamples() {
    static const std::vector<double> samples{
        1.2345678901234567, -1.2345678901234567, 1.25e-200, 1.25e200,
        std::numeric_limits<double>::min(), std::numeric_limits<double>::max()
    };
    return samples;
}

/**
 * @brief ExcelParser 的 gtest 测试夹具。
 *
 * 通过 OpenXLSX 创建真实工作簿，测试读取已保存的 .xlsx 文件。
 * 文件保留在本测试源码目录，便于打开检查；再次运行会重新生成。
 */
class ExcelParserTest : public ::testing::Test {
protected:
    /** @brief 所有输入文件均固定在测试目录，执行目录不影响文件查找。 */
    static std::string testFilePath(const std::string& fileName) {
        return (std::filesystem::path(EXCEL_PARSER_TEST_DIRECTORY) / fileName).string();
    }

    static void SetUpTestSuite() {
        // 生成失败属于测试准备失败，使用致命断言阻止后续测试继续。
        ASSERT_NO_THROW({
            // gtest_repeat 会重复调用本函数；已初始化时不能重复注册同名日志器。
            if (!bitelog::g_logger) {
                bitelog::log_settings settings;
                settings.level = spdlog::level::off;
                bitelog::bitelog_init(settings);
            }
            std::filesystem::create_directories(EXCEL_PARSER_TEST_DIRECTORY);
            createSingleWorksheetFile();
            createMultipleWorksheetsFile();
            createEdgeCasesFile();
            createExtendedCasesFile();
            createBusinessFile();
            createInvalidFiles();
        });
    }

    /** @brief 生成文档中的学生成绩示例：一个工作表、3 行、4 列。 */
    static void createSingleWorksheetFile() {
        OpenXLSX::XLDocument document;
        document.create(testFilePath("singleWorksheet.xlsx"), OpenXLSX::XLForceOverwrite);
        auto worksheet = document.workbook().worksheet("Sheet1");
        worksheet.setName("成绩表");

        worksheet.cell("A1").value() = "姓名";
        worksheet.cell("B1").value() = "性别";
        worksheet.cell("C1").value() = "年龄";
        worksheet.cell("D1").value() = "C语言成绩";
        worksheet.cell("A2").value() = "张三";
        worksheet.cell("B2").value() = "男";
        worksheet.cell("C2").value() = 18;
        worksheet.cell("D2").value() = 90;
        worksheet.cell("A3").value() = "李四";
        worksheet.cell("B3").value() = "女";
        worksheet.cell("C3").value() = 19;
        worksheet.cell("D3").value() = 95;

        document.save();
        document.close();
    }

    /** @brief 生成三个内容和尺寸各不相同的工作表，检查按名称选择工作表。 */
    static void createMultipleWorksheetsFile() {
        OpenXLSX::XLDocument document;
        document.create(testFilePath("multipleWorksheets.xlsx"), OpenXLSX::XLForceOverwrite);
        auto workbook = document.workbook();
        auto sales = workbook.worksheet("Sheet1");
        sales.setName("销售记录");
        workbook.addWorksheet("员工名单");
        workbook.addWorksheet("备注 表");

        sales.cell("A1").value() = "商品";
        sales.cell("B1").value() = "数量";
        sales.cell("C1").value() = "单价";
        sales.cell("D1").value() = "在售";
        sales.cell("A2").value() = "键盘";
        sales.cell("B2").value() = 2;
        sales.cell("C2").value() = 99.5;
        sales.cell("D2").value() = true;
        sales.cell("A3").value() = "鼠标";
        sales.cell("B3").value() = 3;
        sales.cell("C3").value() = 49.25;
        sales.cell("D3").value() = false;

        auto employees = workbook.worksheet("员工名单");
        employees.cell("A1").value() = "姓名";
        employees.cell("B1").value() = "部门";
        employees.cell("C1").value() = "编号";
        employees.cell("A2").value() = "王五";
        employees.cell("B2").value() = "研发";
        employees.cell("C2").value() = 20260001;
        employees.cell("A3").value() = "赵六";
        employees.cell("B3").value() = "财务";
        employees.cell("C3").value() = 20260002;

        auto notes = workbook.worksheet("备注 表");
        notes.cell("A1").value() = "字段";
        notes.cell("B1").value() = "内容";
        notes.cell("A2").value() = "备注";
        notes.cell("B2").value() = "包含,逗号";
        notes.cell("A3").value() = "引用";
        notes.cell("B3").value() = "他说\"你好\"";

        document.save();
        document.close();
    }

    /** @brief 生成空表、稀疏表、不同值类型和特殊字符的边界数据。 */
    static void createEdgeCasesFile() {
        OpenXLSX::XLDocument document;
        document.create(testFilePath("edgeCases.xlsx"), OpenXLSX::XLForceOverwrite);
        auto workbook = document.workbook();
        workbook.worksheet("Sheet1").setName("空表");

        workbook.addWorksheet("仅标题");
        auto headerOnly = workbook.worksheet("仅标题");
        headerOnly.cell("A1").value() = "姓名";
        headerOnly.cell("B1").value() = "年龄";
        headerOnly.cell("C1").value() = "成绩";

        workbook.addWorksheet("单单元格");
        workbook.worksheet("单单元格").cell("A1").value() = "唯一标题";

        workbook.addWorksheet("稀疏数据");
        auto sparse = workbook.worksheet("稀疏数据");
        sparse.cell("A1").value() = "编号";
        sparse.cell("B1").value() = "备注";
        sparse.cell("D1").value() = "结果";
        sparse.cell("A2").value() = 1;
        sparse.cell("D2").value() = "通过";
        // 第三行和整列 C 均不创建单元格，检查稀疏坐标是否保留。
        sparse.cell("B4").value() = "保留";
        sparse.row(5).setHeight(15.0F); // 保留末尾空行，仅创建行节点。

        workbook.addWorksheet("尾部空列");
        auto trailingColumns = workbook.worksheet("尾部空列");
        trailingColumns.cell("A1").value() = "项目";
        trailingColumns.cell("B1").value() = "数量";
        // 显式保留 D1 的空单元格，使表范围包含 C、D 两个尾部空列。
        trailingColumns.cell("D1").value().clear();
        trailingColumns.cell("A2").value() = "样例";
        trailingColumns.cell("B2").value() = 0;

        workbook.addWorksheet("空行无列");
        workbook.worksheet("空行无列").row(3).setHeight(15.0F);

        workbook.addWorksheet("数据类型");
        auto types = workbook.worksheet("数据类型");
        types.cell("A1").value() = "整数";
        types.cell("B1").value() = "大整数";
        types.cell("C1").value() = "小数";
        types.cell("D1").value() = "真值";
        types.cell("E1").value() = "假值";
        types.cell("F1").value() = "文本";
        types.cell("G1").value() = "错误";
        types.cell("H1").value() = "空值";
        types.cell("A2").value() = -42;
        types.cell("B2").value() = std::int64_t{9007199254740993};
        types.cell("C2").value() = 12.125;
        types.cell("D2").value() = true;
        types.cell("E2").value() = false;
        types.cell("F2").value() = "00123";
        types.cell("G2").value().setError("#DIV/0!");

        workbook.addWorksheet("特殊字符");
        auto special = workbook.worksheet("特殊字符");
        special.cell("A1").value() = "内容";
        special.cell("B1").value() = "值";
        special.cell("A2").value() = "逗号";
        special.cell("B2").value() = "甲,乙";
        special.cell("A3").value() = "引号";
        special.cell("B3").value() = "他说\"你好\"";
        special.cell("A4").value() = "换行";
        special.cell("B4").value() = "第一行\n第二行";
        special.cell("A5").value() = "回车";
        special.cell("B5").value() = "第一行\r第二行";
        special.cell("A6").value() = "组合";
        special.cell("B6").value() = "\"甲,乙\"\n结束";
        special.cell("A7").value() = "空格";
        special.cell("B7").value() = " 前后空格 ";

        document.save();
        document.close();
    }
    /** @brief 生成坐标、列边界、公式缓存、长文本和高精度浮点数专项输入。 */
    static void createExtendedCasesFile() {
        OpenXLSX::XLDocument document;
        document.create(testFilePath("extendedCases.xlsx"), OpenXLSX::XLForceOverwrite);
        auto workbook = document.workbook();
        workbook.worksheet("Sheet1").setName("成绩表");
        auto otherSource = workbook.worksheet("成绩表");
        otherSource.cell("A1").value() = "来源";
        otherSource.cell("A2").value() = "另一个文件";

        workbook.addWorksheet("偏移数据");
        auto offset = workbook.worksheet("偏移数据");
        offset.cell("B3").value() = "标题";
        offset.cell("C3").value() = "值";
        offset.cell("B4").value() = "张三";
        offset.cell("C4").value() = 90;

        workbook.addWorksheet("单列数据");
        auto singleColumn = workbook.worksheet("单列数据");
        singleColumn.cell("A1").value() = "值";
        singleColumn.cell("A2").value() = 7;
        singleColumn.cell("A4").value() = -1;
        singleColumn.row(5).setHeight(15.0F);

        workbook.addWorksheet("跨字母列");
        auto wide = workbook.worksheet("跨字母列");
        wide.cell("A1").value() = "首列";
        wide.cell("Z1").value() = "第26列";
        wide.cell("AA1").value() = "第27列";
        wide.cell("AB1").value() = "第28列";
        wide.cell("A2").value() = 1;
        wide.cell("Z2").value() = 26;
        wide.cell("AA2").value() = 27;
        wide.cell("AB2").value() = 28;

        workbook.addWorksheet("末列");
        auto lastColumn = workbook.worksheet("末列");
        lastColumn.cell("A1").value() = "起点";
        lastColumn.cell("XFD1").value() = "终点";

        workbook.addWorksheet("公式缓存");
        auto formulas = workbook.worksheet("公式缓存");
        formulas.cell("A1").value() = "缓存值";
        formulas.cell("B1").value() = "默认缓存";
        formulas.cell("A2").formula() = "1+2";
        formulas.cell("A2").value() = 42;
        // OpenXLSX 新建公式时默认写入缓存 0，不会计算出 3。
        formulas.cell("B2").formula() = "1+2";

        workbook.addWorksheet("日期");
        auto dates = workbook.worksheet("日期");
        dates.cell("A1").value() = "日期";
        dates.cell("A2").value() = OpenXLSX::XLDateTime(45292.5);

        workbook.addWorksheet("浮点精度");
        auto precision = workbook.worksheet("浮点精度");
        precision.cell("A1").value() = "数值";
        for (std::size_t index = 0; index < precisionSamples().size(); ++index) {
            precision.cell(static_cast<std::uint32_t>(index + 2), 1).value() = precisionSamples()[index];
        }

        workbook.addWorksheet("长文本");
        auto longText = workbook.worksheet("长文本");
        longText.cell("A1").value() = "内容";
        longText.cell("A2").value() = std::string(4096, 'x') + "中文🙂,含\"引号\"\n第二行";

        workbook.addWorksheet(" Report ");
        workbook.worksheet(" Report ").cell("A1").value() = "空格属于名称";
        workbook.addWorksheet(std::string(31, 'N'));
        workbook.worksheet(std::string(31, 'N')).cell("A1").value() = "31字符名称";

        workbook.addWorksheet("特殊表头");
        auto specialHeader = workbook.worksheet("特殊表头");
        specialHeader.cell("A1").value() = "姓名,全称";
        specialHeader.cell("B1").value() = "备注\"文本\"";
        specialHeader.cell("C1").value() = "标题\n续行";
        specialHeader.cell("A2").value() = "张三";
        specialHeader.cell("B2").value() = "text";
        specialHeader.cell("C2").value() = "尾列";

        document.save();
        document.close();
    }

    /**
     * @brief 生成可直接打开查看的复杂业务工作簿。
     *
     * 包含五张关联表；主表为 1008 行 × 32 列，有 1000 笔订单、四个分隔空行、
     * 三行表头和一行合计。合并单元格、隐藏行列及显示格式不应改变解析结果。
     */
    static void createBusinessFile() {
        OpenXLSX::XLDocument document;
        document.create(testFilePath("复杂业务 数据.xlsx"), OpenXLSX::XLForceOverwrite);
        auto workbook = document.workbook();
        const auto& worksheets = businessWorksheets();
        workbook.worksheet("Sheet1").setName(worksheets.front().name);

        auto& styles = document.styles();
        auto& fonts = styles.fonts();
        auto& formats = styles.cellFormats();
        const auto boldFont = fonts.create();
        fonts[boldFont].setBold();
        fonts[boldFont].setFontSize(14);
        fonts[boldFont].setFontColor(OpenXLSX::XLColor("FF1F4E78"));
        const auto headingFormat = formats.create();
        formats[headingFormat].setFontIndex(boldFont);
        const auto dateFormat = formats.create();
        formats[dateFormat].setNumberFormatId(22); // Excel 内置日期及时间格式。
        const auto percentageFormat = formats.create();
        formats[percentageFormat].setNumberFormatId(10);
        const auto moneyFormat = formats.create();
        formats[moneyFormat].setNumberFormatId(4);

        for (std::size_t sheetIndex = 0; sheetIndex < worksheets.size(); ++sheetIndex) {
            const auto& source = worksheets[sheetIndex];
            if (sheetIndex != 0) workbook.addWorksheet(source.name);
            auto worksheet = workbook.worksheet(source.name);
            for (std::size_t row = 0; row < source.rows.size(); ++row) {
                for (std::size_t column = 0; column < source.rows[row].size(); ++column) {
                    const auto& expected = source.rows[row][column];
                    // 真正的缺失字段不创建 XML 单元格，保留稀疏表场景。
                    if (expected.value.type() == OpenXLSX::XLValueType::Empty) continue;
                    auto cell = worksheet.cell(static_cast<std::uint32_t>(row + 1),
                                               static_cast<std::uint16_t>(column + 1));
                    if (!expected.formula.empty()) cell.formula() = expected.formula;
                    if (expected.value.type() == OpenXLSX::XLValueType::Error) {
                        // 显式设置错误码，避免通用赋值将不同错误统一为 #N/A。
                        cell.value().setError(expected.value.get<std::string>());
                    } else {
                        cell.value() = expected.value;
                    }
                    if (sheetIndex == 0 && row >= 3) {
                        if (column == 1 || column == 30) cell.setCellFormat(dateFormat);
                        if (column == 11 || column == 29) cell.setCellFormat(percentageFormat);
                        if (column == 9 || column == 10 || (column >= 12 && column <= 14)) {
                            cell.setCellFormat(moneyFormat);
                        }
                    }
                }
            }

            const auto columns = static_cast<std::uint16_t>(source.rows.front().size());
            worksheet.mergeCells(worksheet.range(OpenXLSX::XLCellReference(1, 1),
                                                   OpenXLSX::XLCellReference(1, columns)));
            worksheet.row(1).setHeight(30.0F);
            worksheet.range(OpenXLSX::XLCellReference(1, 1), OpenXLSX::XLCellReference(1, columns))
                .setFormat(headingFormat);
            const std::uint32_t headerRow = sheetIndex == 0 ? 3 : 2;
            worksheet.range(OpenXLSX::XLCellReference(headerRow, 1),
                            OpenXLSX::XLCellReference(headerRow, columns)).setFormat(headingFormat);
            for (std::uint16_t column = 1; column <= columns; ++column) {
                worksheet.column(column).setWidth(column == 1 ? 28.0F : 20.0F);
            }
        }

        auto orders = workbook.worksheet("订单明细");
        orders.mergeCells("B2:F2");
        orders.row(5).setHidden(true);
        orders.row(404).setHidden(true);
        orders.column(23).setHidden(true); // 电话列仍须参与完整数据解析。
        orders.column(29).setHidden(true); // 整列数据为空，但保留其位置。
        orders.column(24).setWidth(38.0F);
        document.save();
        document.close();
    }

    /** @brief 无效输入故意不生成合法 Excel，用于验证两个公开接口的错误处理。 */
    static void createInvalidFiles() {
        std::ofstream corrupt(testFilePath("corruptWorkbook.xlsx"), std::ios::binary | std::ios::trunc);
        corrupt.exceptions(std::ios::badbit | std::ios::failbit);
        corrupt << "This is not an XLSX ZIP archive.";
        corrupt.close();
        std::ofstream empty(testFilePath("emptyWorkbook.xlsx"), std::ios::binary | std::ios::trunc);
        empty.exceptions(std::ios::badbit | std::ios::failbit);
        empty.close();
    }
};

// 单表查询必须只返回实际表名，不能遗留生成器默认的 Sheet1。
TEST_F(ExcelParserTest, ReturnsSingleWorksheetName) {
    const std::vector<std::string> expectedNames{"成绩表"};
    EXPECT_EQ(ExcelParser::getWorksheetNames(testFilePath("singleWorksheet.xlsx")),
              expectedNames);
}

// 同时检查名称、数量和原始顺序，防止漏表、重复表或擅自排序。
TEST_F(ExcelParserTest, ReturnsMultipleWorksheetNamesInWorkbookOrder) {
    const std::vector<std::string> expectedNames{"销售记录", "员工名单", "备注 表"};
    EXPECT_EQ(ExcelParser::getWorksheetNames(testFilePath("multipleWorksheets.xlsx")),
              expectedNames);
}

// 整串比较文档中的例子，覆盖标题、所有单元格和分隔符，而非只校验部分字段。
TEST_F(ExcelParserTest, ParsesAllSingleWorksheetDataIncludingHeader) {
    const std::string expectedData =
        "姓名,性别,年龄,C语言成绩\n"
        "张三,男,18,90\n"
        "李四,女,19,95";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("singleWorksheet.xlsx"), "成绩表"),
              expectedData);
}

// 各表具有不同形状和数据，防止按名称读取时误用第一张表或混入其他表的数据。
TEST_F(ExcelParserTest, ParsesEveryWorksheetWithoutMixingTheirData) {
    const auto filePath = testFilePath("multipleWorksheets.xlsx");
    // 独立写出每张表的完整预期结果，不用被测解析器生成期望值。
    const std::vector<std::string> worksheetNames{"销售记录", "员工名单", "备注 表"};
    const std::vector<std::string> expectedData{
        "商品,数量,单价,在售\n键盘,2,99.5,true\n鼠标,3,49.25,false",
        "姓名,部门,编号\n王五,研发,20260001\n赵六,财务,20260002",
        "字段,内容\n备注,\"包含,逗号\"\n引用,\"他说\"\"你好\"\"\""
    };
    for (std::size_t index = 0; index < worksheetNames.size(); ++index) {
        SCOPED_TRACE(worksheetNames[index]);
        EXPECT_EQ(ExcelParser::parseWorksheet(filePath, worksheetNames[index]),
                  expectedData[index]);
    }
}

// 空工作表不应凭空生成 A1 的字段、标题或换行。
TEST_F(ExcelParserTest, ReturnsEmptyStringForEmptyWorksheet) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "空表"), "");
}

// 仅有标题时不能补出一条数据行。
TEST_F(ExcelParserTest, ParsesWorksheetContainingOnlyHeaders) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "仅标题"),
              "姓名,年龄,成绩");
}

// 最小非空范围为 1×1，结果不能出现多余的逗号或行分隔符。
TEST_F(ExcelParserTest, ParsesSingleCellWithoutExtraSeparators) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "单单元格"),
              "唯一标题");
}

// 中间空行、完全空列及末尾空行都占据原有坐标，不能按非空值压缩结果。
TEST_F(ExcelParserTest, PreservesSparseCellsEmptyColumnsAndEmptyRows) {
    const std::string expectedData =
        "编号,备注,,结果\n"
        "1,,,通过\n"
        ",,,\n"
        ",保留,,\n"
        ",,,";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "稀疏数据"),
              expectedData);
}

// 最右侧的空单元格决定表宽；每一行都要补足相同数量的空字段。
TEST_F(ExcelParserTest, PreservesTrailingEmptyColumnsInEveryRow) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "尾部空列"),
              "项目,数量,,\n样例,0,,");
}

// 有行格式但无单元格时保留行分隔，不擅自创建内容列。
TEST_F(ExcelParserTest, PreservesRowsWithoutCreatingAColumn) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "空行无列"),
              "\n\n");
}

// 大整数超过 double 精确整数范围，另检查前导零文本和实际错误码没有丢失。
TEST_F(ExcelParserTest, ParsesIntegerFloatBooleanTextErrorAndEmptyValues) {
    const std::string expectedData =
        "整数,大整数,小数,真值,假值,文本,错误,空值\n"
        "-42,9007199254740993,12.125,true,false,00123,#DIV/0!,";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "数据类型"),
              expectedData);
}

// 文本中的逗号、引号和换行属于同一字段，首尾空格也必须保留。
TEST_F(ExcelParserTest, EscapesCsvSpecialCharactersAndPreservesWhitespace) {
    // OpenXLSX 的 XML parse_eol 设置会将文件中的字面回车归一化为换行。
    // 独立核对已保存的输入值，再检查解析器的完整输出，避免错误地要求还原回车。
    OpenXLSX::XLDocument document;
    document.open(testFilePath("edgeCases.xlsx"));
    ASSERT_EQ(document.workbook().worksheet("特殊字符").cell("B5").value().get<std::string>(),
              "第一行\n第二行");
    document.close();

    const std::string expectedData =
        "内容,值\n"
        "逗号,\"甲,乙\"\n"
        "引号,\"他说\"\"你好\"\"\"\n"
        "换行,\"第一行\n第二行\"\n"
        "回车,\"第一行\n第二行\"\n"
        "组合,\"\"\"甲,乙\"\"\n结束\"\n"
        "空格, 前后空格 ";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("edgeCases.xlsx"), "特殊字符"),
              expectedData);
}

// 重复调用分别对照已知结果，避免“两次都返回空串”也被误判为成功。
TEST_F(ExcelParserTest, RepeatedParsingReturnsTheSameData) {
    const auto filePath = testFilePath("singleWorksheet.xlsx");
    const std::string expected = "姓名,性别,年龄,C语言成绩\n张三,男,18,90\n李四,女,19,95";
    EXPECT_EQ(ExcelParser::parseWorksheet(filePath, "成绩表"), expected);
    EXPECT_EQ(ExcelParser::parseWorksheet(filePath, "成绩表"), expected);
    EXPECT_EQ(ExcelParser::getWorksheetNames(filePath), std::vector<std::string>{"成绩表"});
}

// 参数校验应抛出明确的 invalid_argument，不能靠后续文件打开失败代替。
TEST_F(ExcelParserTest, RejectsEmptyFilePathAndWorksheetName) {
    EXPECT_THROW(ExcelParser::getWorksheetNames(""), std::invalid_argument);
    EXPECT_THROW(ExcelParser::parseWorksheet("", "成绩表"), std::invalid_argument);
    EXPECT_THROW(ExcelParser::parseWorksheet(testFilePath("singleWorksheet.xlsx"), ""),
                 std::invalid_argument);
}

// 使用确定存在的有效文件，并要求传播 OpenXLSX 的具体工作表输入异常。
TEST_F(ExcelParserTest, RejectsWorksheetThatDoesNotExist) {
    EXPECT_THROW(ExcelParser::parseWorksheet(testFilePath("multipleWorksheets.xlsx"),
                                            "不存在的工作表"),
                 OpenXLSX::XLInputError);
}

// 主表逐字比较所有记录、所有 32 列、合并标题、空行和合计，不采用抽样检查。
TEST_F(ExcelParserTest, ParsesEveryCellOfThousandOrderBusinessWorksheet) {
    const auto& orders = businessWorksheets().front();
    ASSERT_EQ(orders.rows.size(), 1008U);
    ASSERT_EQ(orders.rows.front().size(), 32U);
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("复杂业务 数据.xlsx"), orders.name),
              expectedWorksheetCsv(orders));
}

// 所有维表和汇总表同样做全量校验，包含跨表公式的缓存、错误值和多行字段。
TEST_F(ExcelParserTest, ParsesAllRelatedBusinessWorksheets) {
    const auto& worksheets = businessWorksheets();
    for (std::size_t index = 1; index < worksheets.size(); ++index) {
        SCOPED_TRACE(worksheets[index].name);
        EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("复杂业务 数据.xlsx"), worksheets[index].name),
                  expectedWorksheetCsv(worksheets[index]));
    }
}

// 独立检查实际落盘文件的复杂度，保证测试的确包含隐藏结构、跨表公式及空白区域。
TEST_F(ExcelParserTest, BusinessFixtureContainsComplexWorkbookStructures) {
    const std::vector<std::string> expectedNames{"订单明细", "商品目录", "客户档案", "月度汇总", "说明与异常"};
    const auto filePath = testFilePath("复杂业务 数据.xlsx");
    EXPECT_EQ(ExcelParser::getWorksheetNames(filePath), expectedNames);
    OpenXLSX::XLDocument document;
    document.open(filePath);
    auto orders = document.workbook().worksheet("订单明细");
    EXPECT_EQ(orders.rowCount(), 1008U);
    EXPECT_EQ(orders.columnCount(), 32U);
    EXPECT_TRUE(orders.row(5).isHidden());
    EXPECT_TRUE(orders.row(404).isHidden());
    EXPECT_TRUE(orders.column(23).isHidden());
    EXPECT_TRUE(orders.column(29).isHidden());
    EXPECT_TRUE(orders.findCell(204, 1).empty());
    const std::string totalFormula = orders.cell("K4").formula();
    const std::string customerFormula = orders.cell("D4").formula();
    EXPECT_EQ(totalFormula, "I4*J4");
    EXPECT_EQ(customerFormula, "'客户档案'!B3");
}

// 数据从 B3 起始时仍从 A1 输出，不能将非空区域左上角重新当作标题行。
TEST_F(ExcelParserTest, PreservesLeadingEmptyRowsAndColumns) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "偏移数据"),
              ",,\n,,\n,标题,值\n,张三,90");
}

// 单列中的空行没有逗号可作占位，尤其要保留末尾空行对应的换行。
TEST_F(ExcelParserTest, PreservesEmptyRowsInSingleColumnWorksheet) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "单列数据"),
              "值\n7\n\n-1\n");
}

// 检查 Z 到 AA/AB 的列编号转换及其间 24 个空字段，防止列号只支持单字母。
TEST_F(ExcelParserTest, ParsesColumnsBeyondZWithoutShiftingCells) {
    const auto gap = std::string(25, ',');
    const auto expected = "首列" + gap + "第26列,第27列,第28列\n1" + gap + "26,27,28";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "跨字母列"), expected);
}

// .xlsx 最后一列为 XFD（16384），检查完整范围和无符号列计数的边界。
TEST_F(ExcelParserTest, PreservesTheLastSupportedExcelColumn) {
    const auto expected = "起点" + std::string(16383, ',') + "终点";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "末列"), expected);
}

// 缓存刻意与公式计算结果不同，验证返回缓存 42/0 而非重算为 3 或输出公式文本。
TEST_F(ExcelParserTest, ReadsFormulaCachesWithoutRecalculating) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "公式缓存"),
              "缓存值,默认缓存\n42,0");
}

// Excel 日期是序列数；解析器的职责不包含按操作系统地区格式化日期。
TEST_F(ExcelParserTest, ReturnsUnderlyingExcelDateSerial) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "日期"),
              "日期\n45292.5");
}

// 不复用被测类的格式化算法：把输出独立读回 double，要求精度和极值均可还原。
TEST_F(ExcelParserTest, PreservesFloatingPointPrecisionAndMagnitude) {
    const auto actual = ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "浮点精度");
    std::istringstream rows(actual);
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(rows, line)));
    ASSERT_EQ(line, "数值");
    for (const double expected : precisionSamples()) {
        SCOPED_TRACE(expected);
        ASSERT_TRUE(static_cast<bool>(std::getline(rows, line)));
        std::istringstream number(line);
        number.imbue(std::locale::classic());
        double restored = 0;
        ASSERT_TRUE(static_cast<bool>(number >> restored));
        EXPECT_EQ(restored, expected);
        number >> std::ws;
        EXPECT_TRUE(number.eof()) << "数值后出现多余字符：" << line;
    }
    EXPECT_FALSE(static_cast<bool>(std::getline(rows, line)));
}

// 修改 C++ 全局 locale 后，数字仍使用小数点，不能被地区设置拆成多个 CSV 列。
TEST_F(ExcelParserTest, IsIndependentOfGlobalNumericLocale) {
    const ScopedGlobalLocale localeGuard(std::locale(std::locale::classic(), new CommaDecimalPunctuation));
    ASSERT_EQ(std::use_facet<std::numpunct<char>>(std::locale()).decimal_point(), ',');
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("multipleWorksheets.xlsx"), "销售记录"),
              "商品,数量,单价,在售\n键盘,2,99.5,true\n鼠标,3,49.25,false");
}

// 长 UTF-8 文本和组合特殊字符应完整保留，检查截断及重复转义问题。
TEST_F(ExcelParserTest, PreservesLongUnicodeTextAndCombinedCsvCharacters) {
    const auto expected = "内容\n\"" + std::string(4096, 'x') + "中文🙂,含\"\"引号\"\"\n第二行\"";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "长文本"), expected);
}

// 标题行和普通数据行应采用相同的 CSV 转义规则。
TEST_F(ExcelParserTest, EscapesSpecialCharactersInHeaders) {
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "特殊表头"),
              "\"姓名,全称\",\"备注\"\"文本\"\"\",\"标题\n续行\"\n张三,text,尾列");
}

// 保留合法名称中的空格和 31 字符边界；查表不能自动修剪空格或改变大小写。
TEST_F(ExcelParserTest, PreservesWorksheetNamesAndMatchesThemExactly) {
    const auto filePath = testFilePath("extendedCases.xlsx");
    const std::vector<std::string> expectedNames{
        "成绩表", "偏移数据", "单列数据", "跨字母列", "末列", "公式缓存", "日期",
        "浮点精度", "长文本", " Report ", std::string(31, 'N'), "特殊表头"
    };
    EXPECT_EQ(ExcelParser::getWorksheetNames(filePath), expectedNames);
    EXPECT_EQ(ExcelParser::parseWorksheet(filePath, " Report "), "空格属于名称");
    EXPECT_EQ(ExcelParser::parseWorksheet(filePath, std::string(31, 'N')), "31字符名称");
    EXPECT_THROW(ExcelParser::parseWorksheet(filePath, "Report"), OpenXLSX::XLInputError);
    EXPECT_THROW(ExcelParser::parseWorksheet(filePath, " report "), OpenXLSX::XLInputError);
}

// 不同文件存在同名工作表时不能复用错误的内容或历史文件状态。
TEST_F(ExcelParserTest, IsolatesWorksheetsWithTheSameNameInDifferentFiles) {
    const std::string expected = "姓名,性别,年龄,C语言成绩\n张三,男,18,90\n李四,女,19,95";
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("singleWorksheet.xlsx"), "成绩表"), expected);
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("extendedCases.xlsx"), "成绩表"),
              "来源\n另一个文件");
    EXPECT_EQ(ExcelParser::parseWorksheet(testFilePath("singleWorksheet.xlsx"), "成绩表"), expected);
}

// 缺失文件是打开失败，两个公开接口都不能将失败伪装为空的成功结果。
TEST_F(ExcelParserTest, RejectsMissingWorkbookForBothPublicMethods) {
    const auto missingPath = testFilePath("missingFixture/absent.xlsx");
    ASSERT_FALSE(std::filesystem::exists(missingPath));
    EXPECT_THROW(ExcelParser::getWorksheetNames(missingPath), std::exception);
    EXPECT_THROW(ExcelParser::parseWorksheet(missingPath, "Sheet1"), std::exception);
}

// 一个带 xlsx 后缀的文本文件和一个零字节文件都不是合法的 Excel 压缩包。
TEST_F(ExcelParserTest, RejectsCorruptAndZeroByteWorkbooksForBothPublicMethods) {
    for (const auto& fileName : {"corruptWorkbook.xlsx", "emptyWorkbook.xlsx"}) {
        SCOPED_TRACE(fileName);
        const auto filePath = testFilePath(fileName);
        ASSERT_TRUE(std::filesystem::exists(filePath));
        EXPECT_THROW(ExcelParser::getWorksheetNames(filePath), std::exception);
        EXPECT_THROW(ExcelParser::parseWorksheet(filePath, "Sheet1"), std::exception);
    }
}

// 比较整个 ZIP 包字节，既覆盖正常读取，也覆盖打开后遇到无效表名的异常退出。
TEST_F(ExcelParserTest, DoesNotModifySourceWorkbookOnSuccessOrFailure) {
    const auto filePath = testFilePath("复杂业务 数据.xlsx");
    const auto originalBytes = readFileBytes(filePath);
    ASSERT_FALSE(originalBytes.empty());
    EXPECT_NO_THROW(ExcelParser::getWorksheetNames(filePath));
    EXPECT_NO_THROW(ExcelParser::parseWorksheet(filePath, "订单明细"));
    EXPECT_THROW(ExcelParser::parseWorksheet(filePath, "不存在的表"), OpenXLSX::XLInputError);
    EXPECT_EQ(readFileBytes(filePath), originalBytes);
}

// 覆盖全局日志器为空时的正常和异常分支，并通过 RAII 保证后续用例不受影响。
TEST_F(ExcelParserTest, SupportsUninitializedLoggerOnSuccessAndFailure) {
    const auto originalLogger = bitelog::g_logger;
    ASSERT_TRUE(originalLogger);
    {
        const ScopedLoggerPause loggerGuard;
        ASSERT_FALSE(bitelog::g_logger);
        const auto filePath = testFilePath("singleWorksheet.xlsx");
        EXPECT_EQ(ExcelParser::getWorksheetNames(filePath), std::vector<std::string>{"成绩表"});
        EXPECT_EQ(ExcelParser::parseWorksheet(filePath, "成绩表"),
                  "姓名,性别,年龄,C语言成绩\n张三,男,18,90\n李四,女,19,95");
        EXPECT_THROW(ExcelParser::getWorksheetNames(""), std::invalid_argument);
        EXPECT_THROW(ExcelParser::parseWorksheet(filePath, "不存在的表"), OpenXLSX::XLInputError);
        EXPECT_FALSE(bitelog::g_logger);
    }
    EXPECT_EQ(bitelog::g_logger, originalLogger);
}

// 失败之后立即解析有效文件，验证文件及工作表错误没有污染后续调用。
TEST_F(ExcelParserTest, RecoversAfterFileAndWorksheetErrors) {
    EXPECT_THROW(ExcelParser::getWorksheetNames(testFilePath("corruptWorkbook.xlsx")), std::exception);
    const auto validPath = testFilePath("singleWorksheet.xlsx");
    EXPECT_EQ(ExcelParser::getWorksheetNames(validPath), std::vector<std::string>{"成绩表"});
    EXPECT_THROW(ExcelParser::parseWorksheet(validPath, "不存在的表"), OpenXLSX::XLInputError);
    EXPECT_EQ(ExcelParser::parseWorksheet(validPath, "成绩表"),
              "姓名,性别,年龄,C语言成绩\n张三,男,18,90\n李四,女,19,95");
}

} // namespace
