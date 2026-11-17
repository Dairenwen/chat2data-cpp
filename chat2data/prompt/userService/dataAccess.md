# 用户子服务数据访问封装

**系统身份：** 你是一名熟悉 C++、MySQL、ODB 和 Redis 的后端开发工程师。

**任务：** 为**用户子服务封装用户信息、登录会话和验证码的数据访问操作**。复用现有 ODB 映射类及项目依赖，完成 MySQL、Redis 的读写，并为后续用户业务提供清晰的数据访问接口。

本任务依据[用户子服务设计](./userService.md)和[数据库设计](../../docs/数据库设计.md)。当前已存在 [UserEntity](../../data/userEntity.hpp)、[SessionEntity](../../data/sessionEntity.hpp) 及 ODB 生成代码，用户数据访问封装分别见 [userData.hpp](../../svc_userService/userData.hpp)、[sessionData.hpp](../../svc_userService/sessionData.hpp) 和 [verifyCodeData.hpp](../../svc_userService/verifyCodeData.hpp)。

数据访问接口分别操作 MySQL 或 Redis；下述缓存回填、缓存失效及会话延迟双删由用户业务层按顺序组合执行。业务层还需处理并发回填及缓存操作失败，避免已退出会话重新生效。

## 用户信息操作

对 `UserEntity` 对应的 MySQL 和 Redis 操作进行封装。

### MySQL 操作

1. 保存用户信息到用户表。
2. 通过用户 ID 查询用户信息。
3. 通过昵称查询用户信息。
4. 通过邮箱查询用户信息。
5. 检测昵称是否存在。
6. 检测邮箱是否存在。

**注意事项：**

- 用户 ID、昵称和邮箱具有数据库唯一约束；注册前的查询不能替代数据库对并发写入的唯一性检查。
- 用户密码字段**保存带盐的密码哈希**。数据访问层保存传入的哈希，不负责生成、验证或返回明文密码。
- `UserEntity::id` 是数据库自增主键，保存用户时由数据库生成并由 ODB 赋值给对象。

### Redis 操作

1. 按用户 ID 保存用户信息到缓存。
2. 通过用户 ID 查询缓存中的用户信息。
3. 通过用户 ID 删除用户信息缓存。

缓存类型和字段按照[数据库设计](../../docs/数据库设计.md)中的用户信息缓存执行：

- 键：`user:{userId}`；Hash 的 `data` 字段保存用户信息 JSON（需要序列化）。
- JSON 字段：`userId`、`nickname`、`passwordHash`、`email`、`status`；其中 `passwordHash` 对应 `UserEntity::password`。
- TTL：**3600 秒**。
- **默认不维护昵称和邮箱的缓存副本；按昵称或邮箱查询、检查是否存在时，以 MySQL 查询和唯一约束为准。**
- 用户信息写入 MySQL 成功后删除对应缓存；读取时先查缓存，未命中再查 MySQL 并回填。

`{userId}` 表示实际用户 ID。当前缓存 JSON 不含数据库主键 `id`，因此缓存返回的信息不能直接视为包含完整主键的 `UserEntity`；需要主键的数据库操作应通过 MySQL 获取。

## 登录会话操作

对 `SessionEntity` 对应的 MySQL 和 Redis 操作进行封装。

### MySQL 操作

1. 保存登录会话。
2. 通过会话 ID 查询会话信息。
3. 通过会话 ID 删除会话。

**注意事项：**

- 会话 ID 唯一；同一用户可以有多个登录会话，用户 ID 不设置唯一约束。
- `SessionEntity::id` 是数据库自增主键，保存会话时由数据库生成并由 ODB 赋值给对象。
- `expiresAt` 为 Unix 毫秒。读取到已过期会话时，不能将其作为有效登录凭证或回填为有效缓存。
- Redis 的 TTL 只会使缓存键过期，不会自动删除 MySQL 中的会话记录；读取缓存或数据库时均需检查会话有效期。

### Redis 操作

1. 保存登录会话到缓存。
2. 通过会话 ID 查询缓存中的会话信息。
3. 通过会话 ID 删除会话缓存。

缓存类型和字段按照[数据库设计](../../docs/数据库设计.md)中的会话缓存执行：

- 键：`session:{sessionId}`；Hash 的 `data` 字段保存会话 JSON。
- JSON 字段：`sessionId`、`userId`、`expiresAt`。
- 登录会话有效期为 **3 天**；回填缓存时 TTL 不超过数据库记录的剩余有效期，不因回填重新延长会话。
- 会话创建先写数据库，再删除缓存；读取时先查缓存，未命中再查数据库。
- 删除会话按照现有设计先删缓存、再删数据库、等待 500 毫秒后再次删缓存。

**注意：**延迟双删不能保证强一致，退出登录后仍需确保旧缓存不能使会话继续通过认证，并对失效失败进行重试或降级处理。缓存 JSON 不包含数据库主键 `id`，需要主键的操作应通过 MySQL 获取。

## 验证码操作

验证码只保存在 Redis 中，不需要创建 MySQL 表。

1. 保存验证码信息并设置过期时间。
2. 通过验证码 ID 获取验证码信息。
3. 通过验证码 ID 删除验证码信息。
4. 校验验证码 ID、邮箱和验证码；校验成功后原子消费该验证码，保证并发情况下只能成功使用一次。

缓存类型和字段按照[数据库设计](../../docs/数据库设计.md)中的验证码缓存执行：

- 键：`verifyCode:{codeId}`；Hash 的 `data` 字段保存验证码 JSON。
- JSON 字段：`codeId`、`verifyCode`、`email`、`createdAt`。
- TTL：300 秒；`verifyCode` 使用字符串保存，保留六位验证码的前导零。
- 验证码过期后不能继续使用；验证码不写入日志。
- 验证码键过期后由 Redis 删除，无法与从未存在区分；两种情况统一返回“不存在或已过期”。

单独执行“读取后删除”无法保证并发单次消费；校验和成功删除应在 Redis 中作为一个原子操作完成。生成验证码、发送邮件和登录业务由后续业务层处理。

## 实现要求

1. 在 `chat2Data` 命名空间中组织数据访问封装，分别覆盖用户、登录会话和验证码操作。接口、类和文件应与职责对应，保持简单，供后续用户业务调用。
2. 复用现有 ODB 映射和生成的查询代码；按业务 ID、昵称、邮箱或会话 ID 查询，数据库事务按项目实际操作管理。
3. 用户和会话缓存遵循上述 Cache-Aside 策略；MySQL、Redis 和 JSON 处理失败不能被当成“数据不存在”或成功。**数据库操作成功但缓存失效失败时，需要向调用方报告失败或采取可靠措施，不能静默忽略。**
4. 对查询未找到、数据已过期和依赖操作失败明确区分；验证码键不存在或已过期按上述约定合并。沿用项目现有异常和错误机制，用户业务错误码可以根据实际的开发场景自行定义。
5. 缓存创建和设置 TTL 应保证不会留下永久有效的缓存键；对会话缓存同时检查 `expiresAt`。敏感信息和连接凭证不写入日志。
6. 编写必要的 CMake 配置，确保新增数据访问代码能够与现有 ODB 映射代码一起构建；不修改 `cpp-service-kit` 中的任何文件。
7. 使用 Google Test 验证用户信息查询及唯一约束、会话读写和过期、验证码过期与并发单次消费、缓存失效及依赖错误。测试数据应隔离，清理仅限本次测试创建的数据，不清空 Redis 或删除已有业务数据。

## 连接信息及参考代码

1. MySQL：复用 [BreezeODB.hpp](../../../../libSource/cpp-service-kit/source/BreezeODB.hpp) 中的 `BreezeODB::mysql_settings` 和 `BreezeODB::DBFactory`。开发容器中可使用 `dev-mysql:3306`；数据库名、用户名和密码使用实际开发配置。设置 `cset = "utf8mb4"`，与现有建表脚本保持一致。
2. Redis：复用 [BreezeRedis.hpp](../../../../libSource/cpp-service-kit/source/BreezeRedis.hpp) 中的 `Breezeredis::redis_settings` 和 `Breezeredis::RedisFactory`。开发容器中可使用 `dev-redis:6379`；用户名和密码使用实际开发配置。
3. JSON：参考 [BreezeUtil.hpp](../../../../libSource/cpp-service-kit/source/BreezeUtil.hpp) 中的 `BreezeUtil::JSON`。
4. ODB 映射与构建：[userEntity.hpp](../../data/userEntity.hpp)、[sessionEntity.hpp](../../data/sessionEntity.hpp)、[data/CMakeLists.txt](../../data/CMakeLists.txt)。
5. 日志与异常：参考 [BreezeLog.hpp](../../../../libSource/cpp-service-kit/source/BreezeLog.hpp) 和 [errorHandler.hpp](../../common/errorHandler.hpp)。
6. 开发环境：使用 `docker exec -it --user dev dev-env-service bash` 进入容器，在项目目录内构建和测试。测试程序使用 gflags 接收连接参数，默认连接 `dev-mysql:3306` 和 `dev-redis:6379`；用户名、密码和数据库名按实际情况指定，不把密码写入源码或日志。

请先阅读 `prompt/mustRead` 下的文档，再结合上述用户信息、登录会话及验证码操作完成封装。禁止开发没有规定的功能和越界行为。
