
**系统身份：** 你是一个资深的C++后端开发工程师，熟悉使用brpc框架实现rpc服务。

**任务：** 只完成用户子服务中所有rpc服务器搭建、服务器构建器实现，以及程序主流程实现，并完成配套编译的CMakeLists.txt文件。

**要求如下**

1. 定义`UserServer`类，负责搭建rpc服务器；定义`UserServerBuilder`类，负责构建rpc服务器和其他相关配置。
2. `UserServer`类头文件`userServer.hpp`，实现文件`userServer.cpp`；`UserServerBuilder`类头文件`userServerBuilder.hpp`，实现文件`userServerBuilder.cpp`,放置在`chat2data-cpp/chat2data/svc_userService`目录下。
3. `UserServer`类仅负责`RPC服务器`搭建以及启动，业务逻辑层已经在其他类中实现。
4. `UserServer`类内包含：

   - `UserServiceImpl`实例指针，用于调用业务逻辑层的函数
   - `SvcWatcher`实例指针，用于服务监控
   - `brpc::Server`对象，用于搭建rpc服务器
   - `start`方法，用于启动rpc服务器
   - 服务器搭建参考`chat2data-cpp/chat2data/svc_gatewayService`，**使用build模式进行搭建。**
5. `UserServerBuilder`类使用建造者模式，使用独立接口初始化其内部成员：

   - **设置mysql配置信息(MysqlConfig)**
   - **设置redis配置信息(RedisConfig)**
   - 设置监听端口，注意端口之间不要产生冲突；
   - 设置注册服务配置(注册中心地址、服务名称、服务地址)
   - 设置etcd节点地址

   > **注意：每个成员使用单独的方法进行初始化。**
   >
6. `build`构建过程：

   - 完成各个实例的构造
   - 构建过程中要完成服务注册和发现。
   - RPC服务器使用`cpp-service-kit`框架搭建，参考`libSource/cpp-service-kit/source`中的封装结构。
   - MySQL、Redis、用户服务配置(端口、服务名称、地址)、etcd节点地址、监控服务名称都通过**main中gflags**参数配置 **（不要使用cmakelist+args参数或者环境变量的方式实现）**
7. 程序主流程中，主要是完成gflags参数解析、rpc服务器搭建、启动rpc服务器等，源文件为main.cpp，放置在chat2data-cpp/chat2data/svc_userService目录下。
8. 修改对应CMakeLists.txt文件，确保编译通过，同时cmakelist中的代码应当实现简单并配有必要的注释说明。

---

先阅读`mustRead`目录下的所有项目规则文件，再来进行实现。
