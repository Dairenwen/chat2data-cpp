# Chat2Data 网关服务搭建

## 一、系统身份

你是一个资深的 C++ 后端开发工程师，对微服务架构有深入了解，擅长网关设计和开发。

## 二、任务

帮我给 chat2Data 项目搭建网关服务，需要实现 HTTP 服务器、HTTP 接口定义（业务功能暂不实现）以及路由绑定，并完成 main 方法实现。

## 三、任务拆分

请按照以下步骤实现：

1. 先熟悉 `chat2data-cpp/chat2data/docs/接口设计.md` 文件接口定义。
2. 使用 `cpp-httplib` 库搭建网关服务器，网关的地址和端口通过 `gflags` 参数配置，`gflags` 参数配置在 `main.cpp` 中定义。
3. 实现 HTTP 接口定义，总共 **30 个 HTTP 接口**（29 个业务接口和 1 个健康检测接口），仅健康检测接口需要实现，其余接口绑定后返回未实现响应 **（服务器搭建和 HTTP 接口实现隔离开，在不同类中实现）。**
4. 实现网关服务器构建器（Builder 模式），完成服务器（IP 和端口）以及服务发现组件的构建，不在构建器中启动 HTTP 服务器。
5. 实现 main 方法，完成参数解析、日志初始化以及 HTTP 服务器启动。
6. 编写 `CMakeLists.txt` 文件，完成网关代码的构建。

## 四、网关服务器实现要求

1. 仅搭建 HTTP 服务器，不负责业务逻辑，业务逻辑在其他类中实现。
2. 使用 `cpp-httplib` 库搭建，网关地址和端口通过 `gflags` 参数配置。
3. 提供 `start` 方法：服务器启动时需检测服务器是否已运行，如果运行则返回，未运行再启动；监听失败时向调用方报告失败。
4. 提供 `stop` 方法：服务器停止时需检测服务器是否在运行，如果未运行则返回，运行中则停止并回收服务器线程。`stop` 在服务器对象销毁时自动调用。
5. **服务器在新线程中启动，防止阻塞主线程。**
6. 各个方法在实现时，必须添加**日志记录**，记录程序的执行流程、状态以及异常情况。
7. 服务器中需要包含接口定义实例和服务器实例，在构造方法中完成初始化。
8. 服务器类名称为 `GatewayServer`，包含在 `GatewayService` 命名空间中。
9. 头文件名称为 `gatewayServer.hpp`，源文件名称为 `gatewayServer.cpp`。

## 五、HTTP 接口实现要求

1. HTTP 接口定义和 HTTP 服务器搭建分离，在不同类中实现。
2. 以 `chat2data-cpp/chat2data/docs/接口设计.md` 中 1.2～1.6 节的具体接口定义为准，完成 30 个 HTTP 接口的定义和路由绑定。
3. **仅健康检测接口需要实现**，响应格式遵循接口设计；其余接口绑定占位处理并返回 HTTP `501 Not Implemented`，不返回业务成功。
4. 向外提供路由绑定接口，使网关服务器能够在启动前完成路由绑定。
5. 需要持有服务信道管理对象 `SvcChannels`，便于后续接口通过服务名称获取 RPC Channel；服务发现由 `SvcWatcher` 负责。
6. 类名称为 `GatewayServiceImpl`，包含在 `GatewayService` 命名空间中。
7. 头文件名称为 `gatewayServiceImpl.hpp`，源文件名称为 `gatewayServiceImpl.cpp`。

## 六、网关服务器构建器实现要求

### 1. 主要职责

负责完成网关服务器构建，以及服务发现组件的构建。

### 2. 构建流程

1. 构建服务信道管理对象，并设置需要监控的服务。
2. 构建服务发现（监控）对象，定义回调函数，筛选所配置的服务，处理其节点的添加和移除，并打印日志记录。
3. 在独立线程中调用 `SvcWatcher::watch()`，并保证监听对象、回调及信道管理对象的生命周期覆盖监控线程。当前 `watch()` 在 etcd 不可用时会持续重试且没有公开停止接口，需设计可停止的监听方案，确保退出时监控线程能够安全结束。
4. 构建 HTTP 接口定义对象。
5. 构建 `cpp-httplib` 的 HTTP 服务器对象，并设置读写超时时间为 5 分钟。
6. 绑定路由。
7. 构建网关服务器对象。
8. 返回网关服务器对象；服务器由 `main` 启动。

### 3. 类定义要求

- 构建器类名称为 `GatewayServerBuilder`。
- 构建器和网关服务器放在同一个文件中实现。

## 七、main.cpp 实现要求

### 1. 定义 gflags 参数

包括：

- `BreezeLog` 日志级别配置。
- 网关监听地址：`0.0.0.0`，端口：`9000`。
- ETCD 地址：`http://dev-etcd:2379`。
- 服务名称：
  - 用户子服务：`userService`
  - 文件子服务：`fileService`
  - 数据库子服务：`dbService`
  - AI 子服务：`aiService`

服务名称应与各子服务注册到 ETCD 的名称完全一致；上述名称参考当前错误处理代码中的服务名称，具体注册名称需与后续子服务统一。

### 2. 定义信号处理函数

定义 `SIGINT` 和 `SIGTERM` 信号处理函数，用于处理程序退出信号。

程序退出时：

1. 信号处理函数仅设置 `volatile std::sig_atomic_t` 退出标志。
2. 主线程检测到退出标志后停止服务器并记录日志。

### 3. main 方法实现流程

1. 解析 `gflags` 参数。
2. 初始化日志记录。
3. 注册信号处理函数，处理 `SIGINT` 和 `SIGTERM` 信号。
4. 配置服务发现，监控用户子服务、文件子服务、数据库子服务、AI 子服务的上线和下线事件。
5. 构建网关服务器对象。
6. 启动服务器。
7. 循环检测程序退出信号和服务器状态，监听失败或收到退出信号时结束循环，安全停止并回收相关线程。

## 八、CMakeLists.txt 实现要求

编写 `CMakeLists.txt` 文件，完成网关代码的构建。

具体要求：

1. CMake 最低版本为 `3.16`，与项目根配置保持一致；若直接构建源码中的 BreezeKit，则按其要求使用 CMake 3.20 或更新版本。
2. 设置项目名称为 `GatewayService`。
3. 设置 C++17 标准。
4. 添加可执行文件。
5. 添加链接库；按实际接入方式链接 `cpp-httplib`、`gflags` 和 BreezeKit 所需组件，并将网关目标接入项目根构建。

## 九、其他要求

1. 日志采用 `BreezeLog` 库，可查看以下文件：

   - `libSource/cpp-service-kit/source/BreezeLog.hpp`
   - `libSource/cpp-service-kit/source/BreezeLog.cpp`
2. 服务发现实现可参考以下文件：

   - `libSource/cpp-service-kit/test/BreezeEtcd/rpc_test.cpp`

## 十、输出要求

### 1. 文件存放目录

所有文件创建在以下目录下：

`chat2data-cpp/chat2data/svc_gatewayService`

### 2. 需要生成的文件

```text
chat2data-cpp/
└── chat2data/
    └── svc_gatewayService/
        ├── gatewayServer.hpp
        ├── gatewayServer.cpp
        ├── gatewayServiceImpl.hpp
        ├── gatewayServiceImpl.cpp
        ├── main.cpp
        └── CMakeLists.txt
```

### 3. 代码要求

1. 所有代码必须符合项目规则。代码生成完成后，必须检查是否符合项目规则文档中的要求，务必确保符合项目规则约定。
2. 无需生成测试代码。

在实现之前必须先完整阅读 `AGENTS.md` 以及 `Project.md`，之后再按照文档要求完成实现。
