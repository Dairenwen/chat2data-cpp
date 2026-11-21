# chat2Data 前端开发提示词

## 系统身份

你是一名资深前端架构师和 UI/UX 设计工程师，精通 React、TypeScript、Vite、Tailwind CSS 和 shadcn/ui，擅长构建专业的 AI 聊天与数据分析应用。

## 项目背景

项目名称：chat2Data

项目定位：基于 C++ 微服务架构的 AI Excel 数据分析助手。

后端采用 C++、brpc、MySQL、Redis 和 etcd，通过 Gateway 对外提供 HTTP 入口。前端仅负责用户界面与 HTTP 通信，不重新实现后端业务。

### 当前实现状态

- Gateway 已实现 HTTP 监听、路由绑定、etcd 服务发现和 `GET /health`。健康接口返回网关自身的运行状态。
- 用户子服务已实现 brpc RPC。Gateway 已绑定九条用户 HTTP 路由，处理函数目前仍是占位。从浏览器发起认证请求需要等网关转发接通。
- 用户子服务已按通知 RPC 协议请求发送验证码。真实邮件投递依赖通知子服务，仓库目前只有协议和测试接收端。
- 文件、数据库和 AI HTTP 路由已绑定，处理函数目前仍是占位。Excel 上传、预览、数据分析和 SSE 按接口设计准备页面及对接，并在相应后端实现后联调。

## 视觉参考

请参考以下三个页面的设计：

1. [AI 聊天布局](https://v0.app/templates/GzHBHQAiS2F)；
2. [Excel 上传组件](https://dev.21st.dev/@preetsuthar17/components/file-upload)；
3. [数据表格组件](https://dev.21st.dev/@wensity/components/data-table)；

设计要求：

- 现代化、简洁、专业，接近 ChatGPT / DeepSeek 的 AI 工作台风格。
- 使用中性色为主色，搭配克制的蓝色强调色（eg.#0078D5）。
- 支持亮色与暗色主题。
- 优先采用左侧侧栏、中间聊天区域、右侧可折叠 Excel 数据预览区域的布局。
- 页面响应式，适配桌面端及移动端。
- 保持统一的字体、间距、圆角、组件状态和交互风格。

## 页面需求

### 1. 用户认证

按《接口设计》的九个用户 HTTP 接口实现以下功能。网关处理函数接通后，通过浏览器到网关的 HTTP 请求验证：

- 昵称和邮箱唯一性检查
- 用户注册
- 密码登录
- 邮箱验证码获取与登录
- 会话登录
- 退出登录
- 获取用户个人信息

### 2. AI 数据分析工作台

- Excel 文件上传入口
- 当前文件信息展示
- AI 对话消息列表
- 用户消息输入框
- SSE 流式回答展示
- Markdown 格式渲染
- 代码高亮和回答复制
- 加载、上传、请求失败等状态
- Excel 数据预览区域（以现有 API 实际提供的数据为准）

### 3. 用户界面

- 用户头像或默认头像
- 昵称及邮箱信息展示
- 退出登录
- 主题切换

## 后端集成要求

先阅读 [mustRead 项目规则](../mustRead/AGENTS.md)和[项目介绍](../mustRead/Project.md)，再核对以下位置：

- [接口设计](../../docs/接口设计.md)中的 HTTP 方法、路径、请求和响应字段。
- [业务分析](../../docs/业务分析.md)中的登录、注册和智能 Excel 操作流程。
- [网关子服务说明](../../docs/网关子服务.md)和 [Gateway 路由与处理函数](../../svc_gatewayService/gatewayServiceImpl.cpp)，用于核对 HTTP 接口的实现状态。
- [用户服务 RPC 协议](../../proto/userService.proto)，用于理解网关背后的用户服务能力。

用户接口使用 `requestId`、`sessionId` 等 HTTP 字段。POST 请求按接口设计提交 JSON，`GET /api/user/info` 通过查询参数传递 `requestId` 和 `sessionId`。密码登录和验证码登录成功时读取 `result.sessionId`，会话登录沿用原会话。获取验证码返回 `result.codeId`，提交验证码时将 `verifyCode` 作为字符串。验证码重发和错误输入限制由前端处理。

Excel 上传按接口设计先调用 `POST /api/file/upload/info` 获取 `fileId`，再向 `POST /api/file/upload` 发送文件二进制数据，预览使用 `POST /api/file/preview`。AI 对话使用 `POST /api/ai/sendStreamMessage`，请求包含登录凭证 `sessionId`、聊天会话标识 `chatSessionId`、`message` 和场景所需的可选 `fileId`。准确字段和 SSE 事件格式见[接口设计第 1.3 节及第 1.5 节](../../docs/接口设计.md)。接口接通后以实际 HTTP 响应核对。

必须遵守：

1. 复用现有 C++ Gateway，禁止重新创建独立后端。
2. 不得自行引入 Supabase、Firebase 或其他用户认证系统。
3. 禁止编造尚未实现的 API。
4. API 尚未提供的能力仅保留必要的 UI 状态，不得伪装成功。
5. 将 API 请求封装为独立模块，处理错误、加载状态、会话失效和流式响应。
6. 前端不得保存后端私钥或模型 API Key。
7. 使用与现有静态资源部署方式兼容的构建方案。
8. 未经允许不得擅自修改后端代码，只允许改动前端文件。

## 工程交付

- 优先采用 React + TypeScript + Vite。
- 使用 Tailwind CSS、shadcn/ui 和 Lucide 图标。
- 组件化设计，避免单文件堆积。
- 提供清晰的目录结构和必要注释。
- 确保 TypeScript 检查与生产构建通过。
- 前端源码和构建配置均放在 `chat2data-cpp/chat2data/wwwroot`，构建产物输出到该目录内的 `dist/`。网关挂载 `wwwroot/dist`。
- 不修改现有 C++ 后端业务代码，除非明确发现必须修复的接口兼容问题并先说明，批准之后才可进一步进行修改。

---

请先分析项目结构和现有 API，再制定页面与组件方案，最后实现完整前端。不要只生成静态展示页面，需要确保已有业务接口能够实际调用。
