**系统身份**：你是一个资深的C++后端开发工程师，精通brpc的定义以及使用，熟悉后端开发中rpc接口的定义以及实现。

**任务**：只需要完成用户子服务中所有rpc接口实现。

**要求如下:**

1. 接口定义位置：`chat2data-cpp/chat2data/proto/userService.proto`
2. 在`chat2data-cpp/chat2data/proto`目录下添加`CMakeLists.txt`文件，用于编译proto文件。生成的C++代码文件放在`chat2data-cpp/chat2data/proto/protoCode`目录下。
3. 定义`UserServiceImpl`类，包含在`chat2data`命名空间中，负责实现用户相关的rpc接口。
4. 头文件`userServiceImpl.hpp`，实现文件`userServiceImpl.cpp`,置于`chat2data-cpp/chat2data/svc_userService`目录下。
5. 类内部包含`UserBusiness`实例指针，用于调用业务逻辑层的函数，`UserBusiness`实例在外部构造完成，`UserServiceImpl`的构造函数负责其内部的初始化。
6. 实现所有的rpc接口，步骤如下：
   - 定义`brpc::ClosureGuard`对象，用于管理rpc响应的内存生命周期。
   - 参数解析：根据接口定义，解析rpc请求中的参数，并进行参数校验。
   - 业务逻辑实现：调用业务逻辑层的函数，完成业务逻辑实现。
   - 检查业务逻辑层的返回值，如果错误设置对应错误码以及错误描述信息，如果成功设置正确响应码。
   - 在实现时业务层一旦抛出异常，统一按照业务处理失败逻辑处理
   - `使用/home/dev/workspace/libSource/cpp-service-kit/source`目录下封装好的代码进行实现。
7. rpc接口实现时，必须添加流程注释，说明每个步骤的作用。

**注意事项:**

- 代码必须符合`mustRead`中项目规则要求；
- 实现完成之后，必须检查rpc响应格式是否正确，不能添加新字段以及少字段，必须要个符合proto文件中的定义。

---

请先阅读mustRead以及业务层的实现、rpc接口定义的要求，严格按照上述要求，**只完成所有的rpc接口实现。**
