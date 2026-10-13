# libSource 介绍

## 1. 职责

| 实际目录                      | 主要职责                                             | 关键入口 / 构建目标                                                            | 在 chat2excel 中的预期用途                        |
| ----------------------------- | ---------------------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------- |
| `AIModelAcessTech`          | 封装模型请求、会话历史和流式回调；提供参考聊天服务。 | `sdk/include/ChatSDK.h`；静态库 `ai_chat_sdk`；示例服务 `AIChatServer`。 | 把已经组织好的提示词发送给模型，接收回答片段。    |
| `OpenXLSX`                  | 创建、读取、修改、保存`.xlsx` 工作簿。             | `OpenXLSX/OpenXLSX.hpp`；CMake 目标 `OpenXLSX::OpenXLSX`。                 | 获取 worksheet 名称，读取指定工作表的单元格数据。 |
| `cpp-microservice-scaffold` | 封装 JSON、文件、日志及多个中间件客户端。            | `source/util.h`、`source/log.h`；静态库 `bite_scaffold`。                | 序列化表格数据、保存上传文件、输出日志等。        |

`cpp-httplib` 是额外的 HTTP 依赖，当前三个目录中没有一个独立的 `cpp-httplib` 源码目录。SDK 和参考服务通过系统可见的 `<httplib.h>` 使用它；脚手架的 `example/httplib/` 也有 HTTP 示例。

目标数据流中的分工如下：

```text
浏览器上传文件
    ↓ cpp-httplib 接收 HTTP 请求；文件工具负责保存内容
本地 .xlsx 文件
    ↓ OpenXLSX 读取工作表与单元格
表格数据 + 用户问题 + 提示词模板
    ↓ JSON 工具辅助序列化；业务层组织提示词
ChatSDK.sendMessageStream(...)
    ↓ 模型回答片段进入回调
HTTP 服务把片段编码为 SSE → 浏览器显示
```

## 2. AIModelAcessTech

### 2.1 目录

| `AIModelAcessTech/`                                                               | 作用                                                                                            |
| ----------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| `sdk/include/ChatSDK.h`、`sdk/src/ChatSDK.cpp`                                  | 对外的统一入口：初始化模型、创建/删除会话、发送普通或流式消息。                                 |
| `sdk/include/common.h`                                                            | `Message`、`Session`、`ModelInfo`、`Config`、`APIConfig`、`OllamaConfig` 数据结构。 |
| `sdk/include/LLMProvider.h`                                                       | 模型适配器抽象接口。                                                                            |
| `sdk/include/LLMManager.h`、`sdk/src/LLMManager.cpp`                            | 注册适配器、初始化模型、按模型名称分派请求。                                                    |
| `sdk/include/*Provider.h`、`sdk/src/*Provider.cpp`                              | DeepSeek、ChatGPT、Gemini 和 Ollama 的具体请求与响应解析。                                      |
| `sdk/include/SessionManager.h`、`sdk/src/SessionManager.cpp`                    | 会话管理、历史消息及时间戳管理。                                                                |
| `sdk/include/DataManager.h`、`sdk/src/DataManager.cpp`                          | 使用 SQLite 保存会话和消息。                                                                    |
| `sdk/include/util/myLog.h`、`sdk/src/util/myLog.cpp`                            | SDK 自己的日志器；先初始化，再创建 SDK。                                                        |
| `sdk/CMakeLists.txt`                                                              | 构建并安装`libai_chat_sdk.a` 和 `include/ai_chat_sdk/` 下的头文件。                         |
| `ChatServer/ChatServer.h`、`ChatServer/ChatServer.cpp`、`ChatServer/main.cpp` | 使用 SDK 的 HTTP 聊天参考服务，包含会话、模型列表和消息接口。                                   |
| `ChatServer/build/www/`                                                           | 参考聊天页面的 HTML、CSS、JavaScript；虽然位于`build`，当前源码中静态页面确实在这里。         |
| `test/testLLM.cpp`                                                                | GTest 示例，包含实际云端或本地模型调用。                                                        |

该目录顶层没有统一的 `CMakeLists.txt`，构建入口分别在 `sdk/`、`ChatServer/` 和 `test/`。

### 2.2 调用顺序

1. 调用 `bite::Logger::initLogger(...)` 初始化 SDK 日志器。
2. 创建 `ai_chat_sdk::ChatSDK`。构造期间会打开 SQLite，默认文件是运行目录中的 `chatDB.db`。
3. 用 `APIConfig` 或 `OllamaConfig` 准备模型配置，调用 `initModels(configs)`。
4. 查看 `getAvailableModels()`，确认目标模型被标记为已初始化。
5. 调用 `createSession(modelName)` 获得 **会话 ID**。
6. 调用 `sendMessage(sessionId, message)` 获取完整回答，或调用 `sendMessageStream(sessionId, message, callback)` 接收增量回答。

流式回调类型为 `std::function<void(const std::string&, bool)>`：第一个参数是回答片段，第二个参数表示是否结束。`sendMessageStream` 还返回完整回答字符串；调用过程会进行网络请求，不等同于立即返回的后台任务。

### 2.3 依赖与现有行为

- 使用 C++17；`sdk/CMakeLists.txt` 声明 CMake 最低版本为 3.10。
- SDK 链接 `jsoncpp`、`fmt`、`spdlog`、`sqlite3`、OpenSSL，并通过头文件使用 cpp-httplib。参考服务额外链接 `gflags`；测试额外链接 `gtest`。
- 当前源码注册的云端模型名称是 `deepseek-chat`、`gpt-4o-mini`、`gemini-2.0-flash`。这些是本地代码中的标识，不表示已验证它们当前仍可在云端使用。
- 云端 `APIConfig` 没有 endpoint 字段；SDK 使用各 Provider 的默认地址。直接使用 Provider 时可以传入其支持的 `endpoint` 参数。
- `ChatSDK::initModels()` 当前总是把 `_initialized` 设为 `true` 并返回 `true`，没有汇总每个模型初始化失败的结果。`getAvailableModels()` 也只是本地初始化标志，最终连通性仍要通过实际请求确认。
- `createSession()` 不验证模型是否可用，拿到非空会话 ID 不能证明模型配置正确。
- `OllamaConfig` 同时在派生类和基类声明了 `_modelName`；使用具体的 `OllamaConfig` 对象赋值，避免通过基类指针误设另一份同名字段。
- 两套日志头文件都定义了 `DBG`、`ERR` 等宏，后续在同一编译单元中混用 SDK 与脚手架日志头文件时，需要处理宏重名。

## 3. OpenXLSX

### 3.1 解决什么问题

OpenXLSX 用于处理 `.xlsx` 文件的工作簿、工作表和单元格，不需要先把文件转换成 CSV。其底层读写 OOXML/ZIP 数据。

| 常用接口                                                   | 作用                                |
| ---------------------------------------------------------- | ----------------------------------- |
| `OpenXLSX::XLDocument`                                   | 打开、创建、保存、关闭工作簿文件。  |
| `doc.workbook()`                                         | 获取工作簿对象。                    |
| `workbook.worksheetNames()`                              | 获取工作簿中所有 worksheet 的名称。 |
| `workbook.worksheet(name)`                               | 按名称获取工作表。                  |
| `worksheet.cell("A1")` / `worksheet.cell(row, column)` | 访问单元格；行列编号从 1 开始。     |
| `cell.value()`、`value.type()`、`value.get<T>()`     | 读取单元格值及类型。                |
| `worksheet.rowCount()` / `columnCount()`               | 获取工作表范围信息，辅助遍历。      |

### 3.2 从哪里看起

| `OpenXLSX/`                                | 作用                                                  |
| -------------------------------------------- | ----------------------------------------------------- |
| `OpenXLSX/OpenXLSX.hpp`                    | 对外总头文件，示例中使用`#include <OpenXLSX.hpp>`。 |
| `OpenXLSX/headers/`、`OpenXLSX/sources/` | 文档、工作簿、单元格等接口声明与实现。                |
| `OpenXLSX/external/`                       | 随源码提供的 XML/ZIP 等依赖。                         |
| `CMakeLists.txt`                           | 顶层构建入口，控制是否构建示例、测试和文档。          |
| `OpenXLSX/CMakeLists.txt`                  | 库本身的构建入口，定义`OpenXLSX::OpenXLSX`。        |
| `Examples/Demo1.cpp`                       | 单元格值、值类型和基础操作示例。                      |
| `Examples/Demo2.cpp` 等                    | 其他工作表和遍历操作示例，可按具体需求继续阅读。      |
| `Tests/`、`Documentation/`               | 库测试与文档生成配置。                                |

### 3.3 使用边界

- 当前代码要求 C++17、CMake 3.15 或更新版本。
- 库的 CMake 默认类型是静态库 `STATIC`；以源码配置为准，README 中部分描述与此不同。
- 顶层声明版本 `0.4.1`，库子目录声明 `0.3.2`，不能只凭其中一个数字认定整个源码快照的版本。后续接入应固定当前源码版本或提交。
- 这里的快速上手针对 `.xlsx`，不把旧 `.xls`、CSV 或加密文件当作可直接打开的输入。
- 读取值时要区分字符串、整数、浮点数、布尔、空值和错误值；不能对所有单元格直接调用 `get<std::string>()`。
- 读取公式的文本/缓存值不代表重新计算公式；日期数值也需要结合格式或 `XLDateTime` 解释。
- 行列范围不等于业务数据条数。空白、格式、合并单元格等会影响提取策略；大型文件不宜不加限制地遍历并发送给模型。

## 4. cpp-microservice-scaffold

### 4.1 与 chat2excel 相关的部分

| `cpp-microservice-scaffold/`        | 接口 / 用途                                                                                                                                                     |
| ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `source/util.h`、`source/util.cc` | `biteutil::JSON::serialize/unserialize`，返回 `std::optional`；`FUTIL::read/write` 处理文件；`STR::split` 分割字符串；`Random` 生成随机字符串或数字。 |
| `source/log.h`、`source/log.cc`   | `bitelog::bitelog_init`、`log_settings`，以及 `INF`、`ERR` 等日志宏。使用工具前先初始化日志器。                                                         |
| `example/gflags/`                   | 命令行参数、配置文件使用示例。                                                                                                                                  |
| `example/httplib/`                  | HTTP 客户端、服务端和静态资源示例。                                                                                                                             |
| `test/util/`、`test/jsoncpp/`     | 工具和 JSON 学习入口。                                                                                                                                          |

图片提到的 gflags 参数解析来自 **gflags 依赖本身**，不是 `util.h` 中的一个统一封装接口。需要使用时在业务程序中声明参数并链接 gflags。

### 4.2 其他封装

| 头文件               | 作用                           |
| -------------------- | ------------------------------ |
| `source/redis.h`   | Redis 客户端创建。             |
| `source/odb.h`     | ODB/MySQL 数据库对象创建。     |
| `source/etcd.h`    | 服务注册和服务发现监听。       |
| `source/rpc.h`     | brpc 通道、服务端等工具。      |
| `source/mq.h`      | 消息队列客户端、发布与订阅。   |
| `source/fdfs.h`    | FastDFS 文件操作。             |
| `source/elastic.h` | Elasticsearch 请求与查询封装。 |
| `source/mail.h`    | 邮件相关客户端。               |
| `source/hls.h`     | 基于 FFmpeg 的 HLS 转码工具。  |

这些能力可以按后续项目需求使用。图片描述的 Excel 分析流程没有要求接入全部中间件。

### 4.3 构建与使用注意事项

- 顶层构建 `libbite_scaffold.a`，默认构建产物在构建目录的 `lib/` 中；安装头文件位于 `include/bite_scaffold/`。
- `cmake/Finddepends.cmake` 会检查全部依赖并全量链接，没有只启用 JSON/日志的模块开关。完整构建需要 AMQP、brpc、ETCD、FastDFS、FFmpeg、ODB、Redis 等多个开发库，即使当前只想使用文件工具。
- 安装的 `bite_scaffoldConfig.cmake.in` 在导入目标之前调用 `find_my_depends()`；该函数使用调用方的 `${PROJECT_NAME}` 执行 `target_link_libraries`。如果调用方还没有同名目标，`find_package(bite_scaffold)` 可能报“目标未构建”等错误。因此手册先给出单独使用 `util.cc`、`log.cc` 的方式，完整包接入需要先确认这段配置的行为。
- `JSON::serialize/unserialize` 失败时返回空 `optional`，调用方应检查后再取值。
- `FUTIL::write` 会截断并覆盖已有文件，不会自动创建父目录。
- `Random::code(16)` 的当前实现会在 16 个随机字符后追加 4 位编号，实际返回 20 个字符；不能把参数值直接当作最终长度，也不能把随机结果当作绝对唯一的文件 ID。
- 随库的 README 仍是平台模板文字；使用说明应优先看真实源码与示例。
