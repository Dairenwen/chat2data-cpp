# 文件子服务

**系统身份** 你是一名熟悉 C++17、MySQL、ODB、Redis 和 brpc 的后端开发工程师。

**任务** 完成文件子服务的数据映射、数据访问、业务处理、RPC 接口及服务启动，按现有设计编译并验证。先阅读 [项目规则](../mustRead/AGENTS.md) 和 [项目介绍](../mustRead/Project.md)，再对照 [文件子服务设计](../../docs/文件子服务.md)、[业务分析](../../docs/业务分析.md) 和 [数据库设计](../../docs/数据库设计.md)。

实现范围为文件子服务及所需的数据库映射、构建和测试。已有网关、Excel 解析、存储和 AI 子服务按项目中的协议或设计作为调用边界。项目已定义 `fileService.proto` 和 `file_service_proto` 构建目标。存储及 AI 子服务的下游接口随各自服务落地，文件子服务实现阶段按现有接口接入，未落地的接口在测试中使用替身验证调用参数、返回结果和失败处理。

## 1. 接口定义

协议使用 `proto3` 和 `chat2Data` 包名，服务名为 `FileService`，接口签名及字段以 [fileService.proto](../../proto/fileService.proto) 为准。

| RPC 接口                     | 主要参数                              | 成功结果                      |
| ---------------------------- | ------------------------------------- | ----------------------------- |
| `UploadFileInfo`           | `file_info`，包含文件名、大小和后缀 | 业务文件 ID                   |
| `UploadFile`               | `file_id`、Excel 请求附件           | 已完成导入的文件 ID           |
| `UploadSQLiteFile`         | `filename`、SQLite 请求附件         | 已就绪的文件 ID               |
| `GetFileInfo`              | `file_id`                           | 文件元数据                    |
| `GetFileList`              | 当前用户身份                          | 已就绪文件列表                |
| `DownloadFile`             | `file_id`                           | 原文件名和响应附件            |
| `PreviewExcel`             | `file_id`、工作表原名、页码和页容量 | 文件元数据及分页预览          |
| `GetSQLiteFile`            | `file_id`                           | SQLite 原件的 FastDFS 文件 ID |
| `GetWorksheetDBTables`     | `file_id`                           | 按工作簿顺序排列的实际表名    |
| `HandleFileChatSessionMap` | `file_id`、`chat_session_id`      | 关联结果                      |
| `DeleteFile`               | `file_id`                           | 全部关联资源清理完成后的结果  |

1. 请求使用协议中的 `request_id`、`session_id` 和 `user_id`。网关通过 `IsSessionValid` 取得可信用户 ID。内部调用方传递已鉴权的用户身份，文件子服务校验目标归属和业务状态。
2. 响应回传原请求 ID。成功时设置 `error_code = 0`，错误描述为空。调用下游时先检查 brpc 传输状态，再检查业务错误码。
3. 上传字节通过 `request_attachment()` 读取，下载字节通过 `response_attachment()` 返回。普通元数据和结果字段按现有协议填充。HTTP 转换规则见 [接口设计](../../docs/接口设计.md#13-文件子服务)。

## 2. ODB 数据库映射

1. 将文件信息表和工作表映射表放在 `data` 下，参考 [userEntity.hpp](../../data/userEntity.hpp) 和 [sessionEntity.hpp](../../data/sessionEntity.hpp) 编写 ODB 映射。通过现有 `data/CMakeLists.txt` 组织映射代码和建表 SQL 的生成及编译。
2. 根据 [数据库设计](../../docs/数据库设计.md#12-文件服务) 映射文件信息表和工作表映射表。业务文件 ID 在文件表中唯一，在映射表中引用该唯一键并设置级联删除。
3. Excel 导入完成后，在同一个事务中按工作簿顺序逐条插入映射，再更新文件的完成时间和就绪状态。按文件 ID 查询映射时显式使用 `ORDER BY id`，保持相同顺序。会话关联由 AI 子服务的业务会话表保存。

**文件信息表**

| 字段           | 类型            | 说明                           |
| -------------- | --------------- | ------------------------------ |
| `id`         | BIGINT UNSIGNED | 自增主键                       |
| `fileId`     | VARCHAR(32)     | 唯一的业务文件 ID              |
| `fileName`   | VARCHAR(64)     | 原始文件名                     |
| `fileExt`    | VARCHAR(8)      | 文件后缀                       |
| `fileSize`   | BIGINT UNSIGNED | 文件字节数                     |
| `uploadTime` | BIGINT UNSIGNED | 登记时间或完成时间，Unix 毫秒  |
| `fdfsFileId` | VARCHAR(255)    | 完整 FastDFS 文件 ID           |
| `userId`     | VARCHAR(32)     | 文件所属用户                   |
| `status`     | VARCHAR(16)     | 待上传、上传中、已就绪或删除中 |

**工作表映射表**

| 字段              | 类型            | 说明                            |
| ----------------- | --------------- | ------------------------------- |
| `id`            | BIGINT UNSIGNED | 自增主键                        |
| `fileId`        | VARCHAR(32)     | 文件信息表外键，级联删除        |
| `worksheetName` | VARCHAR(32)     | 工作簿中的原始工作表名          |
| `tableName`     | VARCHAR(64)     | 存储子服务返回的实际 MySQL 表名 |

**注意事项**

- 原件保存在 FastDFS。数据库先记录登记时间，文件就绪时将该字段更新为完成时间。对外返回 Unix 毫秒。取得完整 FastDFS ID 后先写入文件记录，再启动解析和导入。
- 状态变更使用当前状态作为更新条件。文件查询以已就绪状态为准。Excel 待上传记录从首次登记起保留 24 小时。上传和删除中的记录保留到清理完成。

## 3. 数据访问与缓存

参考用户子服务的 [userData.cpp](../../svc_userService/userData.cpp)、[sessionData.cpp](../../svc_userService/sessionData.cpp) 和 [sessionManager.cpp](../../svc_userService/sessionManager.cpp) 组织数据库操作、缓存操作和事务边界。

### 3.1 MySQL 操作

1. 保存文件登记信息，按文件 ID 查询文件并核对所属用户。业务更新使用文件行锁，按当前状态写入 FastDFS ID 或更新状态。
2. 按用户 ID 查询已就绪文件。按上传时间倒序返回，同一时间按文件 ID 升序。
3. 在同一事务内保存同一文件的整批工作表映射和完成信息。按文件 ID 查询工作表映射，在文件记录删除时由外键级联清理。
4. 删除文件记录的事务支持在提交前进行第二次缓存清理。缓存清理持续失败时回滚，供后续重试。

### 3.2 Redis 操作

缓存类型均为 Hash，`data` 字段保存 JSON。具体字段以 [数据库设计的缓存策略](../../docs/数据库设计.md#224-文件元数据缓存策略) 为准。

| 缓存键                 | TTL    | 缓存数据                                                            |
| ---------------------- | ------ | ------------------------------------------------------------------- |
| `file:{fileId}`      | 3 天   | 文件元数据，含 `userId` 和 `status`                             |
| `worksheet:{fileId}` | 1 小时 | 按工作簿顺序排列的映射数组，包含 `worksheetName` 和 `tableName` |

1. 查单个文件时先读 Redis，命中后校验当前用户和就绪状态，通过后返回。未命中时在 MySQL 文件行锁内查询，通过相同校验后回填。读取工作表映射时一次取回两个缓存键，缺失时在同一 MySQL 事务中锁定文件行，按需查库并回填。
2. 保存或更新文件元数据后使相应缓存失效。将文件标记为删除中时，先在同一文件行锁内写入状态并清除两个缓存键，再提交状态变更。缓存清除失败时回滚，提交成功后进入资源清理。
3. 最终删除文件记录时，先清除两个缓存键，再开启 MySQL 事务并删除文件记录及映射。保持事务打开，等待 500 毫秒后再次清除两个缓存键，再提交事务。每次缓存清理失败时重试一次，仍失败则报告错误，已开启的事务回滚。

## 4. 文件业务

将业务编排放在 `svc_fileService` 的 `FileBusiness` 中，由构造函数接收数据访问对象、FastDFS 客户端和下游 RPC 通道。文件类型、状态和归属校验放在业务层，附件和协议字段由 RPC 层处理。

### 4.1 上传 Excel

1. 先在 `UploadFileInfo` 校验文件名、后缀和申报大小，登记待上传状态并返回文件 ID。Excel 后缀统一为 `xlsx`，大小上限为 10 MiB。
2. 接着在 `UploadFile` 核对归属和状态，校验附件非空且实际大小等于申报大小。标记上传中，保存原件并记录 FastDFS ID。
3. 然后把 FastDFS ID 交给 `GetWorksheets` 查询原始工作表顺序，再按此顺序调用 `ParseExcel`。将返回的 `WorksheetData`、用户 ID、文件 ID 和 FastDFS ID 传给存储子服务的 `ImportExcelData`。
4. 导入成功后核对工作表原名与实际表名，按原顺序写入映射并提交已就绪状态和完成时间。最后返回业务文件 ID。

**注意事项**

- Excel 第二步失败后先确认旧操作结束，再依据已记录的 FastDFS ID 清理已建表和原件。清理完成后恢复待上传，原期限内使用同一文件 ID 重试。原期限届满后回收待上传记录。
- 上传进行中时返回文件处理中。已就绪文件再次提交时返回已上传，调用方可通过 `GetFileInfo` 确认。
- 存储子服务按文件串行执行导入和清理，核对归属、状态及 FastDFS ID。物理表名、行序及入库规则按 [Excel 上传与导入的实现注意事项](../../docs/文件子服务.md#32-上传并导入-excel) 执行。

### 4.2 上传和获取 SQLite

1. `UploadSQLiteFile` 校验 `.db` 文件名、附件大小和 SQLite 文件头。给新上传分配文件 ID，写入上传中记录。
2. 接着保存原件、记录 FastDFS ID 并提交文件完成时间及就绪状态。最后返回文件 ID。失败时先清理原件，再移除未就绪记录。
3. 存储子服务登记连接中状态后调用 `GetSQLiteFile`。文件子服务检查 SQLite 类型、归属和就绪状态，再返回完整 FastDFS ID。副本下载、打开和释放由存储子服务处理。

**注意事项** SQLite 使用最大 10 MiB 的单文件快照，上传方导出快照时合并 WAL 中尚未写入主文件的变更。连接失败时由存储子服务清理连接登记。

### 4.3 查询、下载和预览

1. `GetFileInfo` 按业务文件 ID 返回已就绪文件的元数据。 `GetFileList` 直接查询当前用户的已就绪文件。 `DownloadFile` 先复用单文件的读取和校验流程，再按 FastDFS ID 下载原件，最后返回文件名及附件。
2. `PreviewExcel` 先取得元数据和映射，核对文件类型、归属和状态。接着按工作表原名精确定位，调用 `PreviewImportedWorksheet`，向存储子服务传递用户 ID、业务文件 ID、实际表名、页码和页容量。最后组合列名、分页数据和文件元数据。
3. `GetWorksheetDBTables` 复用文件和映射的读取及校验流程，按工作簿顺序返回实际表名。

**注意事项**

- 工作表名留空时按工作簿顺序返回各表第一页。指定工作表时页码从 1 开始，0 取默认值 1；页容量为 1 至 100，0 取默认值 50。未知工作表、无效页码或页容量、响应超出 brpc 消息上限时返回业务错误。
- 总行数只计数据行。总页数向上取整，只有表头时返回一个空页。数据按原行序返回，列按导入顺序排列，SQL `NULL` 映射为空字符串。

### 4.4 关联会话和删除文件

1. `HandleFileChatSessionMap` 先确认所属 Excel 文件已就绪，接着把用户 ID、聊天会话 ID 和文件 ID 交给 AI 子服务的 `UpdateSessionFile`。由 AI 校验会话归属、保存关联，再返回结果。一个文件可关联多个会话，重复关联同一文件返回成功。
2. `DeleteFile` 先核对归属，再按缓存策略标记删除中并使缓存失效。Excel 文件先调用 `DeleteFileChatSessions` 清理关联会话及消息，再使用文件 ID 和当前 FastDFS ID 调用 `DropTableExcel` 清理物理表。SQLite 文件调用 `HasSQLiteFileConnections` 检查占用，有正在建立或已建立的连接时恢复已就绪并提示先断开。
3. 确认可继续清理后删除 FastDFS 原件，再通过延迟双删移除文件记录及级联映射。所有步骤成功后返回成功。清理失败时保留删除中记录，重试沿用原文件 ID。

**注意事项**

- 同一文件的上传、关联和删除按文件串行处理。下游确认目标资源已清理时继续，存储连接或其他依赖故障按错误处理。FastDFS 删除时区分原件已不存在与存储故障。
- AI 服务保留尚待清理的会话标识，存储子服务按文件定位已建的物理表。文件记录删除后再次请求时返回文件不可用。下游调用的业务字段见 [关联与删除流程](../../docs/文件子服务.md#35-关联聊天会话和删除文件)。

## 5. RPC、服务器和程序主流程

1. 按 [用户 RPC 实现](../../svc_userService/userServiceImpl.cpp) 的组织方式编写 `fileServiceImpl.hpp`、`fileServiceImpl.cpp`。实现类继承协议生成的服务接口，构造时接收 `FileBusiness`。每个 RPC 用 `brpc::ClosureGuard` 管理回调，先解析并校验参数，接着调用业务层，最后按协议设置响应。
2. 沿用 [公共错误定义](../../common/errorHandler.hpp) 和 BreezeLog。文件业务码使用预留的 200 至 299，区分参数无效、文件不可用、存储或解析失败及下游故障。日志重点记录上传、导入、下载、关联和删除的结果，日志字段使用请求 ID、业务文件 ID 和错误码。
3. 参考 [用户服务构建器](../../svc_userService/userServerBuilder.cpp) 组织 `FileServer`、`FileServerBuilder` 和 `main.cpp`。使用 gflags 读取配置、初始化日志、安装退出信号，再创建 MySQL、Redis、FastDFS、RPC 通道及业务对象。
4. 按设计使用端口 `9004`，etcd 默认连接 `http://dev-etcd:2379`。brpc 开始监听后，将可访问地址以 `fileService` 注册到 etcd，并发现 `excelService`、`dbService`、`aiService`。退出时停止续租和监听，等待在途 RPC 完成后释放资源。其余参数见 [配置设计](../../docs/文件子服务.md#51-启动参数规划)。
5. 添加文件子服务的 CMake 构建目标，复用现有 `file_service_proto` 和项目公共库。编译时使用项目现有 C++17 配置。实现、构建、日志和注释均按 [项目规则](../mustRead/AGENTS.md) 执行。

## 6. 验证步骤

1. 在开发容器以 `dev` 用户构建文件协议和文件子服务，核对 ODB 生成的字段、文件 ID 唯一约束及工作表级联外键。
2. 使用 Google Test 覆盖 Excel 两步上传、原 ID 重试、预览首屏和分页、文件列表和下载、SQLite 上传与获取、多会话关联、文件删除、SQLite 连接占用和下游清理失败后的重试。
3. 验证两类缓存命中时的归属和状态校验，以及未命中回填、进入删除中时缓存失效、延迟双删失败后的事务回滚。测试数据仅使用本次测试创建的资源，验证结果按实际执行情况记录。

## 依赖库及参考代码

1. `cpp-service-kit` 的 [BreezeODB](../../../../libSource/cpp-service-kit/source/BreezeODB.hpp)、[BreezeRedis](../../../../libSource/cpp-service-kit/source/BreezeRedis.hpp)、[BreezeFDFS](../../../../libSource/cpp-service-kit/source/BreezeFDFS.hpp)，分别用于 MySQL、Redis 和文件原件访问。JSON 和缓存回填参考 [公共工具](../../common/utils.cpp)。
2. 服务注册、发现和通道管理参考 [BreezeEtcd](../../../../libSource/cpp-service-kit/source/BreezeEtcd.hpp) 和 [BreezeRPC](../../../../libSource/cpp-service-kit/source/BreezeRPC.hpp)。日志参考 [BreezeLog](../../../../libSource/cpp-service-kit/source/BreezeLog.hpp)。
3. 服务组织和配置参考 [用户子服务](../../docs/用户子服务.md)、[用户子服务启动入口](../../svc_userService/main.cpp) 和 [Excel 解析服务启动入口](../../svc_excelParseService/main.cpp)。

按 ODB 映射、数据访问、文件业务、RPC、服务器及构建验证的顺序开展。实现范围以文件子服务及本提示词列出的工作为准。
