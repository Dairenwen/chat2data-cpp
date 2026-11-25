# Excel 解析子服务实现

## 1. Excel 解析子服务介绍

Excel 解析子服务负责使用 OpenXLSX 将用户上传的 Excel 文件解析为结构化数据，提取工作表名称、字段名称、字段类型及具体数据，为后续 MySQL 自动建表、数据存储以及大模型生成 SQL 进行数据查询和分析提供基础。

- **解析工作表列表**。获取 Excel 文件中所有 worksheet 的名称，按工作簿顺序返回给文件子服务，供调用方选择需要处理的工作表。
- **解析指定工作表**。根据指定的 worksheet 名称，解析其表头、字段类型及实际数据，并封装为结构化对象。

Excel 解析子服务是内部 brpc 服务。文件子服务先将原始 `.xlsx` 保存到 FastDFS，再通过 etcd 发现并调用解析节点。文件子服务将解析结果交给存储子服务，由存储子服务在 MySQL 中为每个导入的工作表建表、写入数据。开发范围为 Excel 解析子服务，上传、导入及查询链路用于说明调用关系。

---

开始实现前阅读 [项目规则](../mustRead/AGENTS.md)、[项目设计](../../docs/项目设计.md)和 [Excel 解析子服务设计](../../docs/Excel解析服务.md)。本文细化字段命名和类型判断规则。

## 2. 接口定义

沿用现有协议的字段名、编号和类型，包名统一为 `chat2Data`。服务名为 `ExcelParserService`，链接现有 `excel_parse_service_proto` 构建目标。见 [excelParseService.proto](../../proto/excelParseService.proto) 和 [接口设计](../../docs/接口设计.md)。

| RPC 方法          | 请求字段                                         | 成功结果                                            |
| ----------------- | ------------------------------------------------ | --------------------------------------------------- |
| `GetWorksheets` | `request_id`、`fdfs_file_id`                 | `worksheets` 为按工作簿顺序排列的工作表原名       |
| `ParseExcel`    | `request_id`、`fdfs_file_id`、`worksheets` | `worksheets` 为按请求顺序排列的 `WorksheetData` |

- 两个接口均要求非空的 `request_id` 和 FastDFS 返回的完整 `fdfs_file_id`，响应回传原始请求 ID。
- `ParseExcel` 至少接收一个非空工作表名，按原名精确匹配。
- 任何一张请求的工作表解析失败时，整次请求返回业务错误，结果列表为空。
- 成功时 `error_code = 0`，`error_msg` 为空。
- 调用方同时检查 brpc 传输状态和业务错误码。

## 3. 工作簿读取

`ExcelParserBusiness` 通过 `Breezefdfs::FDFSClient::download_to_file` 下载工作簿。每个 RPC 使用独立的临时文件，同一次 `ParseExcel` 中的工作表从同一个工作簿读取。以 RAII 管理工作簿和临时文件，确保下载失败、文件打不开或解析异常时仍能清理文件。两个 RPC 可由不同的服务实例处理，各自按文件 ID 下载。见 [BreezeFDFS.hpp](../../../../libSource/cpp-service-kit/source/BreezeFDFS.hpp)。

`ExcelParser` 使用 OpenXLSX 打开本地 `.xlsx`，读取工作表名、表头和数据。工作簿没有可读取的工作表时返回业务错误。行列范围以实际表格内容为准，保留范围内的空行、缺失单元格及其位置。完全没有可解析结构的工作表返回错误。只有表头时返回有效列信息和空数据行列表，`total_rows` 为 1。单元格读取方式参照现有 [chat2excel 解析器](../../../chat2excel/src/ExeclParser.cc)，解析结果按现有 RPC 结构封装。

## 4. 工作表业务逻辑

### 4.1 工作表表头解析

将 worksheet 的第一行作为表头，表头中的每个单元格对应 MySQL 数据表中的一个字段名称。由于 Excel 表头可以任意命名，因此需要进行规范化处理。

**字段名采用以下项目约定：**

- 字段名称最长不能超过 64 个字符。按 UTF-8 字符计数和截断，保留完整字符。
- 字段名称允许包含 ASCII 数字、大小写字母、下划线、`$` 符号及 Unicode 基本多文种平面内的汉字。
- 字段名称不包含其他特殊字符，也不以数字开头。
- 规范化后的字段名以未加引号的文本返回。存储子服务构建 SQL 时，对列名和表名统一使用反引号包裹，涵盖中文字段名和 SQL 保留字。

| 异常情况     | 处理方式                            | 示例                                    |
| ------------ | ----------------------------------- | --------------------------------------- |
| 表头为空     | 使用 `column_列号`，列号从 1 开始 | `column_1`                            |
| 首字符为数字 | 添加 `column_` 前缀               | `2026销售额` → `column_2026销售额` |
| 包含特殊字符 | 将特殊字符替换为 `_`              | `销售额(元)` → `销售额_元_`        |
| 名称过长     | 截断至允许的长度                    | 最多保留 64 个字符                      |
| 字段名称重复 | 添加唯一编号                        | `姓名` → `姓名_2`                  |
| SQL 保留字   | 存储子服务建表时使用反引号包裹      | `` `select` ``                          |

- 完成替换、加前缀和截断后，按列顺序去重。
- 列名按 MySQL 不区分大小写的规则比较。
- 后续重名列从 `_2` 开始尝试后缀，为后缀预留长度，并检查生成的名称是否与已有列名冲突。
- 工作表原名保留在 `WorksheetData.name` 中，由存储子服务结合文件 ID 生成数据库表名。
- 见 [MySQL 标识符规则](https://dev.mysql.com/doc/refman/8.4/en/identifiers.html)、[标识符长度限制](https://dev.mysql.com/doc/refman/8.4/en/identifier-length.html)和[标识符大小写规则](https://dev.mysql.com/doc/refman/8.4/en/identifier-case-sensitivity.html)。

### 4.2 工作表字段类型推断

Excel 中同一列的单元格可以具有不同的数据类型，而 MySQL 建表时需要为每个字段指定统一的数据类型，因此需要对每列数据进行类型推断。

#### 4.2.1 数据采样与统计

- 从第二行开始，采样 **worksheet 的前 100 行数据**，不包含表头。
- 如果数据不足 100 行，则对**全部数据**进行分析。
- 分别读取每列单元格的数据类型和值，统计各种数据类型出现的次数。
- 最终选择**出现次数最多的数据类型**作为该列的字段类型。没有有效样本或最高次数并列时使用 `TEXT`。

#### 4.2.2 单元格类型判断

OpenXLSX 可以获取单元格的原始数据类型及对应的数据值，但部分字符串可能实际表示数值、布尔值或者日期，因此需要进一步分析。

- **空单元格和 Excel 错误单元格**跳过类型统计。文本去掉首尾空白后，内容为空或不区分大小写等于 `N/A` 时，同样跳过类型统计。
- 对于 OpenXLSX 能够直接识别的整型、浮点型、布尔型，直接使用其数据类型。对数值单元格先检查日期格式，见下文日期类型判断。
- 对于字符串类型，依次判断数值、布尔值和日期。用于判断的临时文本可以去掉首尾空白，响应保留原始字符串。
- 如果均不符合，则按照**普通字符串**处理。

**（1）数值类型判断**

去除字符串首尾的空白字符，判断整个字符串能否完整转换为有限数值。如果可以，则按照浮点型处理。纯数字文本含前导零且有多位时按字符串处理，保留编号、学号等标识的含义。

```text
" 123 "  → 浮点型
"12.56"  → 浮点型
"123abc" → 字符串类型
"00123"  → 字符串类型
```

**（2）布尔类型判断**

将字符串统一转换为大写或小写，再与预设的布尔值枚举进行匹配。

| 布尔值 | 匹配内容                               |
| ------ | -------------------------------------- |
| True   | `true`、`t`、`1`、`yes`、`y` |
| False  | `false`、`f`、`0`、`no`、`n` |

- 符合任意一种枚举形式，即可识别为布尔类型。
- `1` 和 `0` 同时符合数值类型的判断规则，**本项目优先判断数值，因此字符串形式的 `1`、`0` 计入浮点样本；**
- OpenXLSX 原生的布尔单元格计入布尔样本。

**（3）日期类型判断**

对于无法直接获取日期类型的文本单元格，需要根据字符串内容进行日期格式匹配。支持以下日期格式：

```text
YYYY-MM-DD
YYYY/MM/DD
MM-DD-YYYY
MM/DD/YYYY
YYYY.MM.DD
YYYY-MM-DD HH:MM:SS
```

- 匹配日期格式后，还应检查日期是否合法，包括闰年、月份、实际天数和时分秒范围。
- 年份在末尾的两种格式均按月、日、年解释。
- 对于 Excel 使用数值序列号存储的日期，还需要结合单元格的日期格式进行识别，避免将日期误判为普通数值。
- 根据工作簿实际的数字格式识别日期序列，输出时保留序列原值和原始数值类型。
- 把序列写入 MySQL 日期字段需要确认工作簿的日期系统，并由后续导入约定定义转换方式。

**（4）字符串类型判断**

如果单元格内容不符合数值、布尔值、日期等类型的判断规则，则**统一作为字符串类型**处理。

#### 4.2.3 确定字段类型

统计每列中各种有效数据类型出现的次数，将出现次数最多的类型作为该列的最终类型，并在后续建表时映射为对应的 MySQL 字段类型。`ColumnInfo.type` 取值遵循现有协议。

| 推断类型   | 建议的 MySQL 字段类型  | 当前 RPC 列类型                                                           |
| ---------- | ---------------------- | ------------------------------------------------------------------------- |
| 整型       | `BIGINT`             | `BIGINT`                                                                |
| 浮点型     | `DOUBLE`             | `DOUBLE`                                                                |
| 布尔型     | `BOOLEAN`            | `BOOLEAN`                                                               |
| 日期型     | `DATE`               | 可验证的日期文本使用 `DATE`。日期列样本含 Excel 日期序列时使用 `TEXT` |
| 日期时间型 | `DATETIME`           | `TEXT`，保留日期和时间                                                  |
| 字符串型   | `VARCHAR` / `TEXT` | `TEXT`                                                                  |

推断结果是采样得到的列类型提示。整数和浮点数混合时仍按采样规则确定类型，行数据保留各单元格的类型和值。后续导入需要将整数提升为 `DOUBLE` 时，先检查精度是否可保留。

### 4.3 工作表实际数据解析

从 worksheet 的第二行开始解析全部数据，即跳过表头，逐行读取每个单元格的内容，并按照单元格类型进行转换。`CellData.type` 记录原始类型，取 `Empty`、`Integer`、`Float`、`Boolean`、`String` 或 `Error`。`CellData.value` 保存对应的文本表示。

| 单元格类型 | 数据转换方式                                    |
| ---------- | ----------------------------------------------- |
| 整型       | 以 `int64` 读取，输出其完整十进制文本         |
| 浮点型     | 以 `double` 读取，输出可还原为原值的文本      |
| 布尔型     | 转换为 `0` 或 `1`                           |
| 字符串型   | 保留字符串原文，包括前导零和首尾空白            |
| 日期型     | 日期文本保留原文，日期序列保留原始数值文本      |
| 错误类型   | 按照字符串保留 Excel 错误文本，例如 `#DIV/0!` |
| 空单元格   | 保存空文本和 `Empty` 类型                     |

- 将每行数据封装为 `RowData` 对象，并按照原始行列顺序保存，确保数据与对应的字段保持一致，为后续数据库数据插入提供数据来源。
- 每行保留 `total_cols` 个单元格。
- 合并单元格**保留左上角的值，其余位置按空单元格处理**。
- 公式读取工作簿中已缓存的计算结果，无缓存结果按空单元格处理。

### 4.4 结构化数据封装

将解析得到的工作表名称、列信息、行数据、总行数及总列数统一封装到 Protobuf 定义的 `WorksheetData` 对象中。

| 字段           | 存储内容                       | 业务用途                           |
| -------------- | ------------------------------ | ---------------------------------- |
| `name`       | 工作表原名                     | 为存储子服务生成数据库表名提供依据 |
| `columns`    | 按原列顺序排列的字段名称及类型 | 构建数据库表结构                   |
| `rows`       | 从第二行开始的实际行数据       | 向数据库插入数据                   |
| `total_rows` | 总行数，包含第一行表头         | 记录数据规模、辅助数据插入         |
| `total_cols` | 解析范围内的总列数             | 记录字段数量、校验数据结构         |

将 `WorksheetData` 作为子服务之间传输 Excel 解析结果的结构化数据对象。存储子服务根据工作表名、文件 ID 和列信息组织建表，再按行列顺序导入数据。

## 5. RPC 接口实现

`chat2data::ExcelParserServiceImpl` 继承生成的 `chat2Data::ExcelParserService`。构造时注入 `ExcelParserBusiness`，由接口层校验请求、调用业务层并封装响应。见 [通知服务 RPC 实现](../../svc_notifyService/notifyServiceImpl.cpp)。

1. 使用 `brpc::ClosureGuard` 管理 `done`，保证所有返回路径完成一次响应。进入业务流程前校验必填字段。
2. 使用公共 `ErrorCode` 的 Excel 解析预留范围 `400–499` 表达参数错误、文件下载失败、工作簿无效、工作表缺失和解析失败。复用 `Chat2DataException::Code()` 和 `error2String`，成功时返回完整结果。见 [公共错误处理](../../common/errorHandler.hpp)。
3. 使用 `BreezeLog` 记录 RPC 方法、请求 ID、处理阶段和结果。表头和值只用于解析及响应。

## 6. RPC 服务器搭建

`ExcelParserServer` 管理 `brpc::Server`、`ExcelParserServiceImpl` 和 BreezeKit 的 `SvcProvider`。`ExcelParserServerBuilder` 接收监听、注册和 FastDFS 配置，参照 [通知服务服务器](../../svc_notifyService/notifyServer.cpp)组织资源生命周期。

- 校验配置并**初始化 FastDFS** 客户端，再创建业务和 RPC 实现对象。FastDFS SDK 按进程初始化一次和销毁一次，生命周期覆盖所有下载请求。
- 监听成功后由 `SvcProvider` 注册和续租。注册键使用 `/<服务名>/<实例 ID>`，注册值是调用方可达的 `host:port`。注册失败时清理已启动的 RPC 服务器及其依赖。
- 退出时先停止续租，再停止 RPC 监听并等待在途请求，最后释放实现对象和 FastDFS 客户端。见 [BreezeEtcd.cpp](../../../../libSource/cpp-service-kit/source/BreezeEtcd.cpp)。

## 7. 主流程

`main.cpp` 使用 gflags 解析参数，初始化 BreezeLog，配置构建器并启动服务。收到 `SIGINT` 或 `SIGTERM` 后由信号处理函数设置退出标志，主线程负责清理。启动失败记录项目错误码并以非零状态退出。参照 [通知服务入口](../../svc_notifyService/main.cpp)。

| 建议参数                                               | 规划默认值                                  | 用途                                |
| ------------------------------------------------------ | ------------------------------------------- | ----------------------------------- |
| `--excel_port`                                       | `9003`                                    | brpc 监听端口                       |
| `--excel_service`                                    | `excelService`                            | etcd 服务名，与调用方的发现配置一致 |
| `--excel_address`                                    | 空值时使用 `dev-env-service:<excel_port>` | 注册给调用方的地址                  |
| `--registry_address`                                 | `http://dev-etcd:2379`                    | etcd 注册端点                       |
| `--fdfs_trackers`                                    | `dev-tracker:22122`                       | FastDFS Tracker 地址列表            |
| `--fdfs_connect_timeout`、`--fdfs_network_timeout` | 各 `30` 秒                                | FastDFS 连接和网络读写超时          |
| `--log_level`                                        | `info`                                    | BreezeLog 日志级别                  |

## 8. 实现流程与验证

1. 在 `svc_excelParseService/` 实现 `ExcelParser`，按本文工作表规则读取本地工作簿。继续使用工作区现有的 OpenXLSX。
2. 实现 `ExcelParserBusiness` 和 `ExcelParserServiceImpl`，接入 FastDFS 下载、临时文件清理、现有协议和公共错误处理。
3. 实现服务器、构建器和 `main.cpp`，接通 gflags、BreezeLog 和 etcd 注册。
4. 通过模块和根 CMake 接入服务，建议可执行目标为 `excel_parse_service`。依赖沿用项目的 `BREEZEKIT_SOURCE_DIR` 和现有脚本，链接 `excel_parse_service_proto`、`chat2data_common` 和所需的 OpenXLSX、FastDFS、brpc、gflags、etcd 依赖。使用 C++17 并关闭编译器扩展。见 [根 CMake](../../CMakeLists.txt)、[协议 CMake](../../proto/CMakeLists.txt)和 [通知服务 CMake](../../svc_notifyService/CMakeLists.txt)。
5. 使用本地 `.xlsx` 样本验证工作表顺序、表头清理和重名、列类型采样、布尔别名、日期有效性、原始值和空白位置。RPC 测试验证两个接口的成功响应，以及无效文件 ID、下载失败、工作表缺失、文件损坏和临时文件清理。接入 CTest 并确保构建和测试通过。上传及 MySQL 导入的完整链路在文件和存储子服务接入后验证。
6. 服务实现使用 `chat2data` 命名空间，协议和公共错误类型沿用项目现有命名空间。代码及简体中文注释遵循 [项目规则](../mustRead/AGENTS.md)。

## 依赖库及参考代码

1. **OpenXLSX 与工作簿解析**。复用工作区已有的 `OpenXLSX::OpenXLSX`。参照 `chat2excel` 读取工作表顺序、保留空单元格、处理数值精度和公式缓存值。识别 Excel 日期序列时，参考 `XLCell::cellFormat()` 和样式中的数字格式。 [OpenXLSX CMake](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/OpenXLSX/OpenXLSX/CMakeLists.txt) · [XLCell.hpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/OpenXLSX/OpenXLSX/headers/XLCell.hpp) · [XLStyles.hpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/OpenXLSX/OpenXLSX/headers/XLStyles.hpp) · [ExeclParser.cc](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2excel/src/ExeclParser.cc)
2. **BreezeFDFS 与文件下载**。复用 `Breezefdfs::FDFSClient`，通过 `download_to_file` 将工作簿下载到当前请求的临时文件。参考现有测试中的真实上传和下载流程。 [BreezeFDFS.hpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/cpp-service-kit/source/BreezeFDFS.hpp) · [FastDFS 依赖配置](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/cpp-service-kit/cmake/dependencies/fdfs.cmake) · [fdfs_test.cpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/cpp-service-kit/test/BreezeFDFS/fdfs_test.cpp)
3. **Protobuf、brpc 与错误处理**。链接现有 `excel_parse_service_proto`。参考通知服务的 `brpc::ClosureGuard`、请求校验和业务错误响应，复用公共错误码与 `Chat2DataException`。 [Excel 协议](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/proto/excelParseService.proto) · [协议 CMake](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/proto/CMakeLists.txt) · [通知 RPC 实现](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/svc_notifyService/notifyServiceImpl.cpp) · [公共错误处理](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/common/errorHandler.hpp)
4. **etcd、gflags、BreezeLog 与服务构建**。复用 `SvcProvider` 注册和续租。参考通知服务的监听与停机顺序、gflags 参数、日志初始化和依赖链接方式。 [BreezeEtcd.cpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/libSource/cpp-service-kit/source/BreezeEtcd.cpp) · [通知服务服务器](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/svc_notifyService/notifyServer.cpp) · [通知服务入口](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/svc_notifyService/main.cpp) · [通知服务 CMake](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/svc_notifyService/CMakeLists.txt)
5. **测试样本与真实 RPC 测试**。复用 `chat2excel` 的 `.xlsx` 样本，覆盖多工作表、仅表头、稀疏数据和损坏文件，可先复制到现有工作区，注意不要改动chat2excel项目。Excel 子服务的断言以 `WorksheetData` 为准。RPC 测试参考通知服务启动本地 brpc 服务并通过客户端发起调用的写法。 [ExcelParserTest.cc](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2excel/test/ExcelParserTest/ExcelParserTest.cc) · [notifyServiceImpl_test.cpp](/home/drw/workspace/bite-dev-environment/dev-environment/workspace/chat2data-cpp/chat2data/tests/svc_notifyService/notifyServiceImpl_test.cpp)
