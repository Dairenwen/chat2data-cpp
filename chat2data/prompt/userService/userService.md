# 用户子服务

## 1. 用户子服务介绍

用户子服务负责处理用户相关的业务逻辑，包括昵称和邮箱校验、用户注册、登录、退出登录及获取个人信息，通过 RPC 向网关和其他子服务提供服务。

用户信息和登录会话计划使用 MySQL 存储，并使用 Redis 缓存用户信息、登录会话和邮箱验证码。**服务启动后计划将节点注册到 etcd；发送验证码时，通过通知子服务发送邮件。**

目前已经定义 RPC 协议，实现用户和会话的 ODB 数据库映射、数据访问封装及其测试；**业务处理和 RPC 服务器尚未实现，网关中的用户接口目前也只是路由占位。**

## 2. 接口定义

用户子服务共涉及 10 个 RPC 接口：9 个对应网关的用户业务接口，另一个用于服务内部校验登录会话。

- 检测用户昵称是否唯一
- 检测用户邮箱是否唯一
- 用户注册
- 会话登录
- 密码登录
- 获取验证码
- 邮箱验证码登录
- 退出登录
- 获取用户信息
- 检查会话是否有效

具体的 RPC 接口定义如下：

```protobuf
service UserService {
  rpc ValidNickname(ValidNicknameRequest) returns (ValidNicknameResponse);
  rpc ValidEmail(ValidEmailRequest) returns (ValidEmailResponse);
  rpc UserRegister(UserRegisterRequest) returns (UserRegisterResponse);
  rpc SessionLogin(SessionLoginRequest) returns (SessionLoginResponse);
  rpc PasswdLogin(PasswdLoginRequest) returns (PasswdLoginResponse);
  rpc GetCode(GetCodeRequest) returns (GetCodeResponse);
  rpc VcodeLogin(VcodeLoginRequest) returns (VcodeLoginResponse);
  rpc Logout(LogoutRequest) returns (LogoutResponse);
  rpc GetUserInfo(GetUserInfoRequest) returns (GetUserInfoResponse);
  rpc IsSessionValid(IsSessionValidRequest) returns (IsSessionValidResponse);
}
```

具体消息定义以 [userService.proto](../../proto/userService.proto) 为准。协议使用 `proto3`、`chat2Data` 包名，开启 `cc_generic_services`，用于生成 C++ RPC 服务接口。

各接口的主要请求和响应内容如下，表中省略了所有接口共有的 `request_id`，以及响应共有的 `error_code`、`error_msg`：

| RPC 接口           | 主要请求字段                            | 主要响应字段           | 作用                      |
| ------------------ | --------------------------------------- | ---------------------- | ------------------------- |
| `ValidNickname`  | `nickname`                            | 错误码                 | 检测昵称是否可用          |
| `ValidEmail`     | `email`                               | 错误码                 | 检测邮箱是否可用          |
| `UserRegister`   | `nickname`、`password`、`email`   | 错误码                 | 注册用户                  |
| `SessionLogin`   | `session_id`                          | 错误码                 | 使用已有登录会话登录      |
| `PasswdLogin`    | `nickname`、`password`              | `session_id`         | 使用昵称或邮箱及密码登录  |
| `GetCode`        | `email`                               | `code_id`            | 发送邮箱验证码            |
| `VcodeLogin`     | `email`、`verify_code`、`code_id` | `session_id`         | 使用邮箱验证码登录        |
| `Logout`         | `session_id`                          | 错误码                 | 退出指定登录会话          |
| `GetUserInfo`    | `session_id`                          | `user_info`          | 获取当前用户信息          |
| `IsSessionValid` | `session_id`                          | `valid`、`user_id` | 校验会话并获取所属用户 ID |

**注意：**

- `PasswdLoginRequest` 中的字段名为 `nickname`，根据需求，该字段可以填写昵称或注册邮箱。
- `GetUserInfo` 返回的 `user_info` 包含 `user_id`、`nickname`、`email`，不包含密码哈希。
- `IsSessionValid` 不单独对应一个用户 HTTP 接口，网关或其他子服务可在处理请求时调用。接口文档只列出了该 RPC 名称，当前协议中的 `valid` 和 `user_id` 是为内部鉴权补充的字段。
- `error_code` 为 0 表示成功，非 0 表示业务或服务错误；具体用户业务错误码尚待定义。RPC 传输失败还需检查调用状态。
- HTTP 的字段和响应格式见[接口设计](../../docs/接口设计.md)，网关需要完成 HTTP 与 RPC 消息之间的转换。

## 3. 数据库设计

用户子服务产生的用户信息和登录会话计划存储在 MySQL 中，涉及用户信息表和会话表。具体设计依据[数据库设计](../../docs/数据库设计.md)；ODB 映射类和建表文件已生成。

### 3.1 用户信息表设计

用户信息表用于保存用户的基本信息：

| 字段     | 类型            | 约束 | 备注                  |
| -------- | --------------- | ---- | --------------------- |
| ID       | BIGINT UNSIGNED | PK   | 自增主键              |
| 用户 ID  | VARCHAR(32)     | UNI  | 用户业务 ID，必须唯一 |
| 用户昵称 | VARCHAR(32)     | UNI  | 必须唯一              |
| 用户邮箱 | VARCHAR(255)    | UNI  | 必须唯一              |
| 用户密码 | TEXT            |      | 存储带盐的密码哈希    |
| 用户状态 | INT             |      | 登录状态或离线状态    |

**注意：**

- 昵称和邮箱唯一性可以提前检查，最终仍由数据库唯一约束保证。
- 用户密码只保存带盐的密码哈希，不保存明文密码。
- 用户状态使用 `0` 表示离线、`1` 表示在线。同一用户有至少一个未过期且未退出的登录会话时为在线。
- 注册时状态为离线。密码登录、验证码登录、会话登录及退出登录后按 MySQL 中的有效会话校正状态并使用户缓存失效。会话自然到期后，在下次访问该会话时校正状态。

### 3.2 登录会话表设计

登录会话表用于保存用户登录后的会话信息：

| 字段     | 类型            | 约束 | 备注                          |
| -------- | --------------- | ---- | ----------------------------- |
| ID       | BIGINT UNSIGNED | PK   | 自增主键                      |
| 会话 ID  | VARCHAR(32)     | UNI  | 登录会话 ID，必须唯一         |
| 用户 ID  | VARCHAR(32)     |      | 所属用户 ID                   |
| 过期时间 | BIGINT UNSIGNED |      | Unix 毫秒，到期后不能用于认证 |

**注意：**

- 同一用户可以在多个设备登录，因此用户 ID 可以在会话表中出现多次。
- 本表中的登录会话 ID 与 AI 聊天会话 ID 不同。
- 用户表和会话表计划通过 ODB 持久化类映射。ODB 可以生成访问代码及建表 SQL；生成 SQL 后仍需通过部署或迁移流程创建数据库表。

## 4. 缓存设计

缓存使用 Redis。用户信息和会话数据以 MySQL 为数据来源，按 Cache-Aside（旁路缓存）策略读取和失效；验证码仅保存在 Redis 中。具体策略与[数据库设计](../../docs/数据库设计.md)保持一致。

当前数据库设计使用 Redis Hash，每个缓存键的 `data` 字段保存序列化后的 JSON 字符串；TTL 设置在整个缓存键上。下文 `{...}` 表示键中的实际值。

### 4.1 用户信息缓存

过期时间：1 小时。

缓存策略：

- 写策略：注册和单独校正状态时，数据库提交后删除用户缓存。退出登录在会话删除事务提交前清除用户缓存。
- 读策略：先查 Redis，未命中时在用户行锁内查 MySQL 并回填。
- 按昵称或邮箱查询时，以 MySQL 查询和唯一约束为准。

> 这里新增行锁目的是：让同一条会话记录的数据库修改和缓存回填不能并发交错执行，防止旧数据重新写入缓存。

缓存类型：Hash。

键：`user:{userId}`。

可选键：`user:nickname:{nickname}`、`user:email:{email}`。当前设计优先**使用用户 ID 键，避免维护三份用户信息副本。**

`data` 中的 JSON 字段：

| 字段             | 说明           |
| ---------------- | -------------- |
| `userId`       | 用户 ID        |
| `nickname`     | 用户昵称       |
| `passwordHash` | 带盐的密码哈希 |
| `email`        | 用户邮箱       |
| `status`       | 用户状态       |

**注意：** 密码哈希属于敏感数据，只有密码验证等内部处理需要使用；获取用户信息的 RPC 响应不返回该字段。如果后续调整缓存字段，需要同步评估数据库设计及密码登录的读取方式。

### 4.2 登录会话缓存

过期时间：会话有效期为 3 天；缓存 TTL 不超过会话在数据库中的剩余有效期。

缓存策略：

- 创建会话：先写数据库，再删除对应缓存；后续按需回填。
- 验证会话：先查 Redis，未命中时查数据库；只回填尚未过期的会话。
- 删除会话：先删除 Redis，再在 MySQL 事务中直接执行删除语句，等待 500 毫秒后再次删除缓存。退出登录在同一事务内校正用户状态并清除用户缓存，然后提交。**会话缓存删除失败时重试一次，持续失败则报告错误并回滚未提交的删除。**

缓存类型：Hash。

键：`session:{sessionId}`。

`data` 中的 JSON 字段：

| 字段          | 说明                    |
| ------------- | ----------------------- |
| `sessionId` | 登录会话 ID             |
| `userId`    | 所属用户 ID             |
| `expiresAt` | 绝对过期时间，Unix 毫秒 |

回填期间持有 MySQL 会话行锁。已开始的回填在删除语句执行前结束，由第二次缓存删除清理。删除语句执行后，等待行锁的回填要到事务提交后才能继续，此时会话记录已删除，可以保证数据一致性。

MySQL 与 Redis 均按原始字节匹配会话 ID。

### 4.3 验证码缓存

过期时间：5 分钟。

缓存策略：

- 写策略：生成验证码后写入 Redis，并设置 5 分钟 TTL。
- 读策略：通过验证码 ID 读取缓存，校验邮箱和验证码。
- 删除策略：验证成功后删除，未使用的验证码由 TTL 自动过期。

缓存类型：Hash。

键：`verifyCode:{codeId}`。

`data` 中的 JSON 字段：

| 字段           | 说明                                 |
| -------------- | ------------------------------------ |
| `codeId`     | 验证码 ID                            |
| `verifyCode` | 六位数字验证码，使用字符串保留前导零 |
| `email`      | 接收验证码的邮箱                     |
| `createdAt`  | 创建时间，Unix 毫秒                  |

**注意：** 验证码仅存 Redis，校验和消费需要保证单次使用；键不存在或已过期统一处理。**验证码通过通知子服务发送到邮箱，不在获取验证码的 RPC 响应中返回。**

用户业务通过 `SvcChannels` 选择 `notifyService` 节点，使用 [notifyService.proto](../../proto/notifyService.proto) 的 `SendVerifyCode` 向与输入完全一致的注册邮箱发送验证码。发送失败时删除已写入的验证码并报告失败。验证码登录先原子消费验证码，再核对注册邮箱并创建登录会话。

## 5. 实现流程

用户子服务的后续实现流程如下：

1. 编写用户表和登录会话表的 ODB 持久化类，根据数据库设计配置字段和约束，生成数据库访问代码及建表 SQL。
2. 实现用户表和会话表的数据库操作，以及用户信息、会话和验证码的 Redis 操作。
3. 实现会话创建、有效期校验和删除，处理数据库与缓存的一致性。
4. 实现昵称和邮箱校验、注册、密码登录、验证码登录、会话登录、退出登录和获取用户信息。
5. 实现验证码生成和单次消费，并调用通知子服务发送验证码邮件。
6. 根据 `userService.proto` 实现 RPC 服务，通过 brpc 启动服务，并向 etcd 注册用户服务节点。
7. 完成参数解析、日志初始化和依赖初始化，编写用户服务的 CMake 构建配置。
8. 实现网关用户路由与用户服务的 RPC 对接，使用 Google Test 测试用户业务及服务之间的调用。

**注意：** 用户注册需要满足需求文档中的昵称、邮箱唯一性及密码要求。验证码邮件投递依赖部署的通知子服务。

## 依赖库及参考代码

1. `libSource/cpp-service-kit`：项目使用的 C++ 服务组件库，可根据实际需要使用其中的组件：
   - 服务注册与发现：[BreezeEtcd.hpp](../../../../libSource/cpp-service-kit/source/BreezeEtcd.hpp)，提供 `SvcProvider` 和 `SvcWatcher`。
   - RPC 服务和信道管理：[BreezeRPC.hpp](../../../../libSource/cpp-service-kit/source/BreezeRPC.hpp)，提供 `RpcServerFactory` 和 `SvcChannels`。
   - Redis 连接：[BreezeRedis.hpp](../../../../libSource/cpp-service-kit/source/BreezeRedis.hpp)，提供 `Breezeredis::RedisFactory`。
   - ODB 数据库连接：[BreezeODB.hpp](../../../../libSource/cpp-service-kit/source/BreezeODB.hpp)，提供 `BreezeODB::DBFactory`。
   - JSON 序列化：[BreezeUtil.hpp](../../../../libSource/cpp-service-kit/source/BreezeUtil.hpp)，提供 `BreezeUtil::JSON`。
   - 日志：[BreezeLog.hpp](../../../../libSource/cpp-service-kit/source/BreezeLog.hpp)。
2. 网关服务配置与日志初始化：[main.cpp](../../svc_gatewayService/main.cpp)。当前网关默认监听的用户服务名称为 `userService`，默认 etcd 地址为 `http://dev-etcd:2379`。
3. 网关服务发现与路由：[gatewayServer.cpp](../../svc_gatewayService/gatewayServer.cpp)、[gatewayServiceImpl.cpp](../../svc_gatewayService/gatewayServiceImpl.cpp)。
4. 项目需求与接口：[需求文档](../../docs/需求文档.md)、[接口设计](../../docs/接口设计.md)、[数据库设计](../../docs/数据库设计.md)。
