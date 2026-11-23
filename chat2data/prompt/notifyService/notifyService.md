# 通知子服务实现

## 1. 通知子服务介绍

通知子服务是**内部 brpc 服务**，为**用户子服务发送验证码**，为 **AI 子服务预留普通邮件发送**能力。验证码的生成、Redis 存储和登录校验由用户子服务负责。普通邮件的主题、HTML 正文和附件由调用方准备。

- **验证码邮件**：浏览器通过 `POST /api/user/code` 请求验证码。网关调用用户子服务的 `GetCode`，用户子服务核对注册邮箱、选择**通过 etcd 发现**的通知节点，生成并保存验证码后调用 `SendVerifyCode`。通知 RPC 成功后，浏览器收到验证码 ID，**验证码通过邮件发送**。通知 RPC 失败时，用户子服务删除本次验证码记录。见[用户子服务设计](../../docs/用户子服务.md)。
- **普通邮件**。按项目设计，AI 子服务后续通过 `SendEmail` 发送分析结果，可携带多个文件附件。见[项目介绍](../mustRead/Project.md)。

当前仓库已有通知协议、用户服务调用代码和测试接收端。以下为通知服务端的实现规格。

## 2. 接口定义

两个接口沿用现有协议的字段名、编号及 `package chat2Data`。C++ 代码由现有 `notify_service_proto` 目标生成和编译，生成类型位于 `chat2Data` 命名空间。见 [notifyService.proto](../../proto/notifyService.proto)。

```proto
syntax = "proto3";
package chat2Data;
option cc_generic_services = true;

// 发送验证码邮件。
message SendVerifyCodeRequest {
  string request_id = 1;
  string email = 2;
  string verify_code = 3; // 六位数字字符串，通过邮件发送。
}

message SendVerifyCodeResponse {
  string request_id = 1;
  int32 error_code = 2; // 0 表示邮件发送成功。
  string error_msg = 3;
}

// 发送 HTML 邮件，可附带多个文件。
message SendEmailRequest {
  message Attachment {
    string filename = 1;     // 收件人看到的文件名。
    bytes data = 2;          // 附件的原始文件字节。
    string content_type = 3; // MIME 类型，可省略。
  }

  string request_id = 1;               // 请求 ID。
  string to_email = 2;                 // 收件人邮箱。
  string subject = 3;                  // 邮件主题。
  string content = 4;                  // HTML 正文。
  repeated Attachment attachments = 5; // 附件列表，可为空。
}

message SendEmailResponse {
  string request_id = 1; // 对应的请求 ID。
  int32 error_code = 2;  // 0 表示邮件发送成功。
  string error_msg = 3;  // 错误描述。
}

service NotifyService {
  // 发送验证码邮件
  rpc SendVerifyCode(SendVerifyCodeRequest) returns (SendVerifyCodeResponse);
  // 发送普通邮件，可附带文件
  rpc SendEmail(SendEmailRequest) returns (SendEmailResponse);
}
```

两个接口均回传原始 `request_id`。`error_code = 0` 表示 SMTP 服务器已接收邮件，收件箱送达由邮件系统继续处理。业务失败返回非零错误码和 `error_msg`，调用方同时检查 brpc 传输状态。附件使用 Protobuf `bytes` 传输原始文件字节。

## 3. 封装邮件发送客户端

直接复用 `Breezecode::MailClient`。它负责 libcurl 全局初始化、SMTP 认证、HTML 和附件的 MIME 编码，以及传输资源的清理。服务进程创建一个启用队列的客户端，使用同一套 `Breezecode::mail_settings` 发送两类邮件。见 [BreezeCode 头文件](../../../../libSource/cpp-service-kit/source/BreezeCode.hpp)和[实现](../../../../libSource/cpp-service-kit/source/BreezeCode.cpp)。

- 验证码邮件调用 `sendAsync(email, verify_code)`。随服务提供 UTF-8 HTML 模板，部署时配置模板路径。模板使用 `{{code}}` 占位符，客户端替换占位符并使用内置主题。验证码邮件也可通过配置添加一个固定文件附件。
- 普通邮件调用 `sendMailAsync(message)`。将 `to_email`、`subject`、`content` 映射到 `MailMessage::to`、`subject`、`html`。每个请求附件映射为 `MailAttachment`，保留文件名和字节长度，内存附件直接交给客户端。空 MIME 类型由客户端填为 `application/octet-stream`。

## 4. 异步支持

使用带 `Breezecode::MailQueueOptions` 的 `MailClient` 构造函数。**两种邮件进入同一个进程内队列，由单个工作线程按入队顺序发送。**客户端管理任务数据、同步和线程生命周期。两个异步方法均返回本次任务的 `std::future<bool>`。

- BreezeCode 默认限额为 **32 个任务和 512 MiB 邮件负载**，排队和正在发送的任务都计入。单个请求还受 brpc 请求体和 SMTP 邮件大小限制。
- 每个任务从提交时开始计时，预算覆盖排队和 SMTP 传输。BreezeCode 的默认值为 30 秒。服务的建议默认值为 25 秒，为现有 30 秒 RPC 信道留出处理余量。部署调整时，通知 RPC 超时应覆盖邮件任务预算及返回开销，网关到用户服务的超时还应覆盖用户服务自身处理。见 [BreezeRPC 信道配置](../../../../libSource/cpp-service-kit/source/BreezeRPC.cpp)。
- RPC 等待每封邮件的发送结果。提交后以单调时钟和任务预算设置等待截止时间，可在 bthread 中用 `wait_for(0)` 查询结果。队列拒绝、任务过期和 SMTP 失败均由客户端返回 `false`。发送链路沿用用户服务的零自动重试策略。
- 服务退出时先停止接收新 RPC 并等待在途请求，再销毁客户端。BreezeCode 会处理已接收的任务，随后回收后台线程。

## 5. 邮件发送业务逻辑实现

`NotifyBusiness` 通过构造函数取得启用队列的 `MailClient`，在 `notifyBusiness.hpp` 和 `notifyBusiness.cpp` 中实现。业务层校验邮件输入、构造邮件并提交任务，成功提交后把 `future` 交给 RPC 层等待。

- `SendVerifyCode` 校验收件人和六位 ASCII 数字验证码，按字符串原样提交，保留前导零。收件邮箱以调用方传入的单个邮箱地址为准。
- `SendEmail` 校验单个收件邮箱、非空主题和附件文件名。收件地址、主题、附件文件名与 MIME 类型应排除 CR、LF 和 NUL。正文按 HTML 原样传递，附件数据按字节长度保留，包括其中的 NUL 字节。

## 6. RPC 接口实现

`chat2data::NotifyServiceImpl` 继承生成的 `chat2Data::NotifyService`，在 `notifyServiceImpl.hpp` 和 `notifyServiceImpl.cpp` 中覆盖 `SendVerifyCode` 和 `SendEmail`。构造时注入 `NotifyBusiness`。RPC 处理方式参照[用户子服务实现](../../svc_userService/userServiceImpl.cpp)。

1. 用 `brpc::ClosureGuard` 管理 `done`，保证所有返回路径完成一次响应。校验请求 ID 后调用业务层，提交时让队列持有邮件数据。
2. 等待发送结果。SMTP 确认后返回成功。参数无效、任务失败或超时均返回对应业务错误。队列拒绝和 SMTP 失败共用发送失败错误码，底层异常统一转换为业务处理失败，错误码可自行添加，但要写好对应的描述信息。
3. 在通知服务预留范围 `500–599` 中定义所需错误码及描述，复用 `Chat2DataException` 和 `error2String`。响应包含 `request_id`、`error_code`、`error_msg`，成功时错误信息为空。日志记录请求 ID、RPC 方法和错误码，密码、验证码、正文和附件字节仅用于邮件处理。见 [errorHandler.hpp](../../common/errorHandler.hpp) 和 [errorHandler.cpp](../../common/errorHandler.cpp)。

## 7. RPC 服务器搭建

`NotifyServer` 和 `NotifyServerBuilder` 放在 `notifyServer.hpp` 和 `notifyServer.cpp` 中。参照用户服务的构建与停机顺序。见 [userServer.cpp](../../svc_userService/userServer.cpp)和 [userServerBuilder.cpp](../../svc_userService/userServerBuilder.cpp)。

- `NotifyServer` 管理 `brpc::Server`、`NotifyServiceImpl` 和 BreezeKit 的 `SvcProvider`。用智能指针明确所有权，调用 `AddService` 时可使用 `SERVER_DOESNT_OWN_SERVICE` 将实现对象借给 brpc。实现对象的生命周期覆盖服务器的 `Stop` 和 `Join`。
- `NotifyServerBuilder` 接收监听端口、注册中心地址、服务名、注册地址、邮件配置和队列配置。校验配置并用带超时的请求检查 etcd 连通性，再构造客户端、业务对象和 RPC 服务。先启动监听，成功后通过 `SvcProvider::registry()` 注册。注册失败时通过 RAII 清理已启动的服务器和邮件资源。
- etcd 注册键格式为 `/<服务名>/<实例 ID>`，值是调用方可达的 `host:port`。默认服务名 `notifyService` 与用户服务的 `--watch_service` 一致。停机时先释放注册续租对象，节点随租约到期下线，再停止监听并等待在途 RPC，最后释放实现与邮件客户端。见 [BreezeEtcd.cpp](../../../../libSource/cpp-service-kit/source/BreezeEtcd.cpp)。

## 8. 主流程

`main.cpp` 解析 gflags，初始化 `BreezeLog`，配置构建器并启动通知服务。参考[用户子服务入口](../../svc_userService/main.cpp)。收到 `SIGINT` 或 `SIGTERM` 时，信号处理函数设置退出标志，主线程负责清理资源。启动失败记录项目错误码并以非零状态退出。

| 建议参数                                                                       | 默认值或来源                                    | 用途                                             |
| ------------------------------------------------------------------------------ | ----------------------------------------------- | ------------------------------------------------ |
| `--notify_port`                                                              | `9002`                                        | brpc 监听端口                                    |
| `--notify_service`                                                           | `notifyService`                               | etcd 服务名，与调用方的发现配置一致              |
| `--notify_address`                                                           | 空值时使用 `dev-env-service:<notify_port>`    | 通告给调用方的地址，其他部署按网络环境设置       |
| `--registry_address`                                                         | `http://dev-etcd:2379`                        | etcd 注册端点                                    |
| `--log_level`                                                                | `info`                                        | BreezeLog 日志级别                               |
| `--smtp_url`                                                                 | 部署配置，例如 `smtps://smtp.example.com:465` | SMTP 连接地址，生产环境使用 TLS 和默认的证书校验 |
| `--smtp_username`、`--smtp_password`、`--smtp_from`                      | 部署配置                                        | SMTP 登录账号、客户端授权码、发件邮箱            |
| `--smtp_login_options`                                                       | 空值                                            | 按邮件服务商要求设置的认证选项                   |
| `--mail_html_path`、`--mail_attachment_path`                               | 部署配置，附件路径可留空                        | 验证码模板与固定附件                             |
| `--mail_max_tasks`、`--mail_max_payload_bytes`、`--mail_task_timeout_ms` | `32`、`536870912`、`25000`                | 队列任务数、总负载字节数和任务时间预算           |

不清楚或不明确的配置信息留空，等待我来配置，**不采用从配置文件或环境变量读取信息的策略**。仓库中的示例配置使用占位符。启动时检查正数队列参数、监听端口、注册信息和验证码模板可读性。见 [gflags 参数文件说明](../../../../libSource/cpp-service-kit/docs/gflags.md)。

## 9. 实现流程

1. 在 `chat2data/svc_notifyService/` 中放置验证码 HTML 模板，使用 `Breezecode::MailClient` 验证两类邮件数据到发送接口的映射，**验证码发送附件路径配置为空，本项目不随验证码发送附件。**
2. 实现 `notifyBusiness.hpp/.cpp` 和 `notifyServiceImpl.hpp/.cpp`，接入队列结果、现有协议和公共错误码。
3. 在 `notifyServer.hpp/.cpp` 中实现服务器及构建器，再通过 `main.cpp` 接入配置、日志和退出流程。
4. 添加模块的 `CMakeLists.txt`，在根构建中通过 `add_subdirectory(svc_notifyService)` 接入，目标名建议为 `notify_service`。保持 C++17 并关闭编译器扩展。见 [Chat2Data 根 CMake](../../CMakeLists.txt)。
5. 跟随现有 `BREEZEKIT_SOURCE_DIR` 源码接入方式，编译复用的 `BreezeCode.cpp` 和 `BreezeEtcd.cpp`，使用已有 `chat2data_common` 的 BreezeLog 与 BreezeUtil。用 BreezeKit 的 `cmake/dependencies/code.cmake` 查找 `CURL::libcurl`，复用 RPC、etcd 依赖脚本，并链接 `notify_service_proto`、brpc、gflags、etcd-cpp-api 及线程库。静态链接依赖顺序参照[用户服务 CMake](../../svc_userService/CMakeLists.txt)。
6. 后续实现时，在 `tests/svc_notifyService/` 用 Google Test 和 CTest 验证两个 RPC。测试应当为真实的业务调用流程，实现简单，代码清晰可读，写好配套注释，并确保编译通过。

服务实现类使用 `chat2data` 命名空间，协议和公共错误类型使用现有的 `chat2Data`，BreezeCode 类型使用 `Breezecode`。自有资源采用 RAII，Protobuf RPC 回调签名沿用框架规定的非拥有指针。代码与文档遵循[项目规则](../mustRead/AGENTS.md)。

## 依赖库及参考代码

1. 邮件客户端、队列和 MIME 投递测试。 [BreezeCode.hpp](../../../../libSource/cpp-service-kit/source/BreezeCode.hpp) · [BreezeCode.cpp](../../../../libSource/cpp-service-kit/source/BreezeCode.cpp) · [code_test.cpp](../../../../libSource/cpp-service-kit/test/BreezeCode/code_test.cpp)
2. 通知协议与现有验证码调用。 [接口设计](../../docs/接口设计.md) · [notifyService.proto](../../proto/notifyService.proto) · [userBusiness.cpp](../../svc_userService/userBusiness.cpp)
3. brpc 服务实现、服务器构建与 etcd 注册。 [userServiceImpl.cpp](../../svc_userService/userServiceImpl.cpp) · [userServer.cpp](../../svc_userService/userServer.cpp) · [userServerBuilder.cpp](../../svc_userService/userServerBuilder.cpp) · [BreezeEtcd.cpp](../../../../libSource/cpp-service-kit/source/BreezeEtcd.cpp)
4. 程序入口及构建。 [用户服务 main.cpp](../../svc_userService/main.cpp) · [协议 CMake](../../proto/CMakeLists.txt) · [BreezeCode 依赖		](../../../../libSource/cpp-service-kit/cmake/dependencies/code.cmake)
