**系统身份：** 你是一个资深的C++后端开发工程师，熟悉网关服务的实现。

**任务：** 按照要求只完成网关子服务中涉及到**有关用户子服务的所有HTTP接口**的实现。

**要求如下:**

1. 网关子服务中API接口定义文档位置： `chat2data-tech/chat2Data/svc_gatewayService/API.md`
2. 用户子服务的RPC接口定义文档位置： `chat2data-cpp/chat2data/proto/userService.proto`
3. 所有的HTTP接口在`chat2data-tech/chat2Data/svc_gatewayService/gatewayServiceImpl.h、gatewayServiceImpl.cc` 中声明和实现
4. 用户子服务涉及的 9 个网关HTTP接口包括：

   - **检测用户昵称是否唯一**：`handleValidNickname`
   - **检测用户邮箱是否唯一**：`handleValidEmail`
   - **用户注册**：`handleUserRegister`
   - **密码登录**：`handlePasswordLogin`
   - **获取验证码**：`handleGetVerifyCode`
   - **验证码登录**：`handleVerifyCodeLogin`
   - **会话登录**：`handleSessionLogin`
   - **退出登录**：`handleLogout`
   - **获取用户信息**：`handleGetUserInfo`

   > 注意：只实现用户子服务的9个网关HTTP接口，不要实现其他的子服务接口！
   >
5. 每个接口可以参考如下流程实现：

   - 解析HTTP请求参数
   - 调用用户子服务的对应RPC接口
     - 获取用户服务信道
     - 创建用户子服务RPC客户端
     - 构建RPC请求
     - 调用用户子服务RPC接口
     - 检测RPC响应是否成功
   - 解析RPC响应
   - 构建HTTP响应体

   > 注意：Rpc通信方式使用Brpc方式。
   >
6. 关键的必要操作除了按照正常流程处理之外，还需要先进行鉴权，再调用用户子服务的RPC接口完成后续操作，如果鉴权失败，session无效则不能继续进行。
7. 在绑定路由前，设置静态资源路径，前端页面严格参考`chat2data-cpp/chat2data/prompt/wwwroot/pages.md`进行实现。
8. 网关的所有HTTP请求和响应在进行JSON序列化和反序列化或可使用已经封装好的工具时，使用`libSource/cpp-service-kit/source`/`chat2data-cpp/chat2data/common`
9. 异常处理以及错误码返回参考`chat2data-cpp/chat2data/common`下相关文件，允许增加错误码，注意写好错误描述。
10. 代码书写规则和注释规范参考`mustRead`目录，不允许出现复杂难懂、设计冗余、没有注释的代码。

---

先阅读`mustRead`目录下的所有文件，再严格按照上述要求，只完成网关中涉及的用户子服务的HTTP接口的实现。实现完成之后，同步修改CMakeLists.txt文件，确保网关子服务代码能编译成功，**暂且不需要增加测试或修改任何测试文件。**
