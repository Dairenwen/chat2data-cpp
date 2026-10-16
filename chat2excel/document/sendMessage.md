
**系统身份**: 你是一个资深C++后端开发工程师

**任务**: 帮我在Chat2ExcelServer类中实现消息发送接口，用于将用户问题发送给大模型

**接口定义**

```bash
POST /api/sendMessage
请求头: Content-Type: application/json
请求体:
{
    "fileId": "file_id",
    "message": "分析这个Excel文件"
}
```

**sendMessage方法流程**

1. 将请求体中的请求参数反序列化为`JSON`对象
2. 从`Json`对象中提取`fileId`和`message`字段，确保其不能为空
3. 使用文件Id作为参数，解析Excel文件内容
4. 构建提示词
5. 设置HTTP响应头
   - `Content-Type`: `text/event-stream`
   - `Cache-Control`: `no-cache`
   - `Connection`: `keep-alive`
   - 使用分块传输编码（Chunked Transfer Encoding）
   - 实现 SSE格式:
     - 消息内容为 JSON 字符串
     - 每条消息以两个换行符结束
     - 最后发送 `data: [DONE]` 表示结束
   - 实时推送 AI 生成的文本片段

- **技术要求**
- 代码逻辑清晰，结构合理
- 注释完整:
  - 类注释：说明类的用途
  - 方法注释：说明方法功能、参数、返回值、可能抛出的异常
  - 关键代码注释：说明复杂逻辑
- 命名规范
  - 类名使用大驼峰命名法
  - 方法名、参数名、变量等使用小驼峰命名法
- 无语法错误

**注意**

1. 使用`ChatSDK`给模型发送消息时，你可以参考`libSource/ChatServer`中发送消息的实现
2. 在构造方法中初始化ChatSDK，设置模型参数等，参考libSource/ChatServer中构造函数的实现，模型只需要注册`deepseek-chat`即可
3. 绑定路由，将`sendMessage`方法注册到"`/api/sendMessage`"路径上
4. 日志输出使用`bite_scaffold`中的日志库
