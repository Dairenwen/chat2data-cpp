
**系统身份**: 你是一个资深的全栈工程师，精通 HTML5、CSS3 和 JavaScript，擅长构建现代化的web页面

**任务**: 帮我实现一个基于 AI 的 Excel 数据分析助手的前端页面，提供文件上传和智能对话功能

**后端接口**

```
GET /api/health
响应体
{
    "status": "healthy"
}

POST /api/uploadFile?fileName=xxx
请求头: Content-Type: application/octet-stream
请求体: 文件二进制内容
响应:
{
    "success": true,
    "result": {
        "fileId": "1711234567"
    }
}

POST /api/sendMessage
请求头: Content-Type: application/json
请求体:
{
    "fileId": "file_Id",
    "message": "分析这个Excel文件"
}

响应格式:
data: 正文

data: 正文

data: 正文

data: [DONE]
```

**服务器配置**

- 服务器地址: `192.168.112.128`
- 服务器端口号：`9000`

**前端web页面描述**
结合以上内容帮我生成一个前端页面，类似于 [mimo.xiaomi.com](https://mimo.xiaomi.com/) 的网页聊天助手，页面要求如下:

1. 前端页面的html、JavaScript、css等代码分开存放
2. 前端页面最上方是`Chat2Excel`标题，标题下是`"基于AI的Excel数据分析助手,上传您的Excel文件,即可获得智能化的数据分析和建议"`功能介绍，均居中显示。
3. 功能介绍下方`"选择文件"`按钮，用户点击`"选择文件"`按钮之后，弹出文件选择窗口，用户选择`Excel`文件后，点击确定按钮，前端需要将Excel文件上传到服务器，服务器返回文件ID
4. `选择文件`按钮右侧是一个只读编辑框，用来显示用户选择的Excel文件路径。注意文件仅支持`.xlsx`格式的Excel文件
5. 选择按钮下方是消息展示区域，占主页面85%高度，消息展示区域左侧展示模型消息，右侧展示用户消息，每条消息下方紧跟消息收发时间，可滚动。用户消息和模型消息背景用不同颜色区分。
6. 消息显示区域下方为用户输入问题的编辑框，在未输入文本时，框内提示`"请输入您的消息...(Enter发送，Shift+Enter换行)"`，用户输入文本后提示消失，消息编辑框右侧为发送按钮，按钮下显示用户消息文本总字数/输入文本最大限制`(最大限制2000字)`，用户点击发送按钮后，编辑框中输入的问题会被发送到服务器
7. 发送消息成功后，服务器返回的是标准SSE格式，比如：

```
data: 文本数据\n\n

data: 文本数据\n\n

data: [DONE]
```

对服务器返回的流式数据以`markdown`格式解析，代码放到代码块中语法高亮显示，代码支持一键复制

注意：服务器返回的流式数据可能会较多，避免数据过长而导致界面变形或者乱码

9. 要求整个页面协调美观，配色和字体严格参考 [mimo.xiaomi.com](https://mimo.xiaomi.com/)

**输出要求**:

- 前端代码放在`src/rootwww`目录下，包含HTML、 CSS、 JavaScript等文件
