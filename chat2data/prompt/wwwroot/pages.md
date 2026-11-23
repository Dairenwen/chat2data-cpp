# chat2Data 前端开发提示词

## 系统身份

你是一名资深前端架构师和 UI/UX 设计工程师，精通 React、TypeScript、Vite、Tailwind CSS 和 shadcn/ui，擅长构建专业的 AI 聊天与数据分析应用。

## 项目背景

项目名称：chat2Data

项目定位：基于 C++ 微服务架构的 AI Excel 数据分析助手，通过自然语言让用户查询、分析 Excel 文件和 MySQL / SQLite 数据库，并以流式对话和可视化图表展示结果。

后端采用 C++、brpc、MySQL、Redis 和 etcd，通过 Gateway 对外提供 HTTP 入口。前端仅负责用户界面与 HTTP 通信，不重新实现后端业务。

目标用户包括数据分析师、业务人员、开发人员和管理者。他们的共同点是不想写 SQL，希望用对话完成数据查询和分析，所以界面要做到“零学习成本”：核心操作不超过 3 步，对话首字响应要让人感到快（后端目标是 3 秒内响应）。

### 当前实现状态

- Gateway 已实现 HTTP 监听、路由绑定、etcd 服务发现和 `GET /health`。健康接口返回网关自身的运行状态。
- 用户子服务已实现 brpc RPC。Gateway 已绑定九条用户 HTTP 路由，处理函数目前仍是占位。从浏览器发起认证请求需要等网关转发接通。
- 用户子服务已按通知 RPC 协议请求发送验证码。真实邮件投递依赖通知子服务，仓库目前只有协议和测试接收端。
- 文件、数据库和 AI HTTP 路由已绑定，处理函数目前仍是占位。Excel 上传、预览、数据分析和 SSE 按接口设计准备页面及对接，并在相应后端实现后联调。

## 视觉参考


### 应用外壳与导航

导航项固定为五个：智能 Excel 助手、智能 DB 助手、我的文件、历史会话、个人中心。页面跳转由前端实现。

#### ★ 可折叠左侧栏

- 结构：顶部品牌和“新建会话”，中部五项导航，下部“最近会话”（按今天、昨天、近 7 天、更早分组），底部用户头像菜单（主题切换、退出登录）。
- 参考：[shadcn/ui Sidebar 组件](https://ui.shadcn.com/docs/components/sidebar)及 [Blocks 中的侧栏模板](https://ui.shadcn.com/blocks)；[Vercel Chatbot](https://github.com/vercel/chatbot)的会话分组侧栏；[Open WebUI](https://github.com/open-webui/open-webui)。
- 借鉴点：折叠成图标态，移动端用 Sheet 抽屉，会话分组和悬停操作。
- 最贴近 ChatGPT / DeepSeek 的使用习惯。

---

### 智能 Excel / 智能 DB 工作台布局

两个助手页共用同一套工作台组件。无论选哪种，数据区顶部都有“文件预览 / 结果分析”两个可切换标签。

#### ★ 三栏：导航 | 聊天 | 可折叠数据区

- 结构：中间是聊天，右侧是数据区（约占 40–50% 宽度，可折叠成窄条）。数据区里切换“文件预览”和“结果分析”。
- 移动端：底部三个标签“对话 / 数据 / 分析”。
- 参考：ChatGPT / Claude 的右侧面板交互（闭源，仅作交互思路）；[Vercel Chatbot](https://github.com/vercel/chatbot) 的侧边文档面板。
- 聊天阅读舒适，数据区可随时收起。

---

### 聊天界面组件来源

只取 UI 展示组件。后端 SSE 是自有协议，数据流由项目自己的 hook 驱动，**不使用 Vercel AI SDK 的 `useChat`**，除非另外写适配层并先说明。

#### ★ assistant-ui

- 地址：[GitHub](https://github.com/assistant-ui/assistant-ui)。
- 内容：`Thread`、`Composer`、`ThreadList` 等原语，自动滚动、无障碍完善，可接自定义 runtime 或 data-stream。
- 取舍：抽象最重，需要为自有 SSE 写自定义 runtime，学习成本高；适合以后要做复杂的“工具调用渲染成组件”。


#### 聊天体验通用要求

- 消息列表：用户消息和助手消息区分明显；助手消息头部显示所用模型名称；自动滚到底部，但用户手动上滑后不再强制拉回，并出现“回到底部”按钮。
- 输入框：Enter 发送，Shift+Enter 换行；**输入法组合输入（`isComposing`）期间按 Enter 不发送**；自动增高有上限；发送中禁用发送按钮，显示“停止生成”。
- 流式失败时**不自动重试** POST（避免重复发送），显示“重试”按钮由用户手动触发。
- 每条助手消息提供“复制”操作。

---

### Markdown、代码高亮与流式渲染

通用要求：SQL 必须有语法高亮；代码块带复制按钮；表格可横向滚动；**禁止用 `dangerouslySetInnerHTML` 渲染模型输出**。

#### ★ Streamdown

- 地址：[GitHub](https://github.com/vercel/streamdown)。
- 内容：react-markdown 的 drop-in 替代，专为流式设计，能处理没写完的 Markdown 块；GFM 表格、Shiki 代码高亮（亮暗双主题）、KaTeX 公式、Mermaid，带安全加固。

#### react-markdown + remark-gfm + Shiki

- 地址：[react-markdown](https://github.com/remarkjs/react-markdown)、[Shiki](https://shiki.style)。
- 流式输出时未闭合的代码块和表格会短暂渲染异常，需要自己做更新节流或补全。

---

### AI 分析结果展示

需求要求结果分析区展示四类内容：分析任务（任务拆解与各步骤状态）、关键发现、分析总结、数据可视化。**这些内容的字段和 SSE 事件格式一律以接口设计为准，不得假设后端一定提供某个字段**；没有的就不渲染对应区块。

#### ★ 步骤时间线 + 结论卡

- 结构：分析任务用垂直时间线，每步有“等待 / 进行中 / 完成 / 失败”状态；之后依次是关键发现（要点卡）、分析总结、图表。
- 参考：AI Elements 的 [`Task` / `ChainOfThought`](https://elements.ai-sdk.dev)、prompt-kit 的 `Steps` / `Reasoning`。

---

### 图表渲染库

需求要求至少能显示柱状图、饼状图和表格。无论选哪个，都要做成独立的 `ChartRenderer` 适配层：可视化数据的格式以接口设计为准；遇到无法识别的数据时退回表格或原始文本，并用错误边界保护，**不能因为图表数据异常导致整页白屏**。

#### ★ shadcn/ui Charts（基于 Recharts）

- 地址：[ui.shadcn.com/charts](https://ui.shadcn.com/charts)。

#### Apache ECharts + echarts-for-react

- 地址：[ECharts](https://echarts.apache.org)、[echarts-for-react](https://github.com/hustcc/echarts-for-react)。


---

### 数据表格与 Excel / 数据库表预览

Excel 预览和数据库表预览共用同一个只读表格组件。一个 Worksheet 对应一张表，所以要有类似 Excel 的底部工作表标签。

通用要求：表头吸顶，行号列，空值有明确显示，数字右对齐并使用等宽数字，长文本截断加 tooltip，横向滚动，亮暗主题都清晰；只读，不做编辑；**展示内容以预览接口实际返回的表结构和数据为准**。

#### ★ TanStack Table + shadcn/ui Data Table

- 地址：[shadcn Data Table](https://ui.shadcn.com/docs/components/data-table)、[TanStack Table](https://tanstack.com/table)；增强写法参考 [tablecn](https://github.com/sadmann7/tablecn)（列固定、筛选、虚拟滚动）

#### Glide Data Grid

- 地址：[GitHub](https://github.com/glideapps/glide-data-grid)。

#### react-data-grid

- 地址：[GitHub](https://github.com/adazzle/react-data-grid)。


---

### 文件上传（Excel 与 SQLite）

通用要求：

- Excel 只接受 `.xlsx`，不超过 10 MB；SQLite 只接受 `.db`。前端先校验格式和大小，不通过时给出明确原因。
- **未创建聊天会话前，Excel 上传入口处于禁用状态**，并用 tooltip 说明“请先新建会话”。
- 上传分两步（先调用 `POST /api/file/upload/info` 获取 `fileId`，再调用 `POST /api/file/upload` 发送二进制），界面显示整体状态：校验 → 上传信息 → 上传文件 → 服务端解析 → 预览就绪。
- `fetch` 没有上传进度事件，二进制上传需要进度时使用 `XMLHttpRequest`（或 axios 的 `onUploadProgress`）；支持取消和失败重试。
- 上传或解析期间禁用发送。当前文件信息条显示文件名、大小和工作表数量，并提供“更换文件”。
- 同一个 Excel 可以关联多个会话：新建会话后，除了上传新文件，还要提供“从我的文件中选择”入口（调用“关联文件和聊天会话”接口）。

#### ★ Kibo UI Dropzone + 输入框附件按钮

- 数据区空状态显示大拖拽区；输入框左侧有附件按钮；两处共用同一套上传逻辑。
- 组件：[Kibo UI Dropzone](https://www.kibo-ui.com)（MIT，shadcn 注册表，可直接 `npx shadcn add`，Fable 能自行安装）。
- Fable 可以自动拉取源码，最稳妥。

---

### 登录与注册页

交互通用要求：

- 登录页有两个标签“密码登录 / 验证码登录”；注册页和登录页之间用“点击注册”“点击登录”互相跳转。
- 验证码输入使用 6 位分格输入框（[shadcn Input OTP](https://ui.shadcn.com/docs/components/input-otp)）；“获取验证码”带倒计时；提示“验证码 5 分钟内有效，使用一次后失效”。
- 表单使用 react-hook-form + zod；密码框带显示 / 隐藏；Enter 提交；登录失败直接显示服务端返回的具体原因。
- 注册：昵称和邮箱在失焦时（带防抖）调用唯一性检查接口；密码规则（至少 8 位；数字、小写字母、大写字母、特殊符号至少两类）以实时打勾的规则清单展示；确认密码必须一致。
- 不添加需求里没有的功能，例如“忘记密码”“第三方登录”。

#### V9-A ★ 两栏：左表单，右品牌视觉

- 参考：[shadcn/ui login 与 signup Blocks](https://ui.shadcn.com/blocks/login)（login-04、signup-02 这一类）。
- 右侧用一个静态的“对话 + 图表”产品示意（用代码画，不依赖图片素材）。


---

### 文件、历史会话、个人中心的列表样式

#### 我的文件

- **★ 表格视图**：名称、类型图标、大小、上传时间、操作；带类型筛选和搜索。参考 V7-A 的表格方案和 [shadcn-admin](https://github.com/satnaing/shadcn-admin) 的数据表。

#### 历史会话

- **★ 按日期分组的列表**（今天、昨天、近 7 天、更早）+ 搜索；每行显示标题、最后活跃时间、消息数量、模型名称、会话类型徽标，悬停显示删除。参考 ChatGPT / [LobeChat](https://github.com/lobehub/lobe-chat) 的会话列表。

#### 个人中心

- ** ★ 单页分区卡片**：资料卡（头像、昵称、邮箱、在线状态）、我的文件（与“我的文件”页复用同一组件）、外观设置（主题）、退出登录。

---

### 视觉风格与主题

整体风格参考 [Refero 上的 Cursor 设计系统](https://styles.refero.design/style/4e3b4717-84c8-4599-baaf-a343c3d619b6)（页面提供 DESIGN.md、Tailwind v4 CSS 变量和设计令牌，可直接抓取）。一句话概括：**暖奶油纸面、墨色文字、一个橙色强调、4px 小圆角、细线边框、标题不加粗**，像一本编辑精良的技术杂志，而不是常见的 SaaS 仪表盘。

#### 色彩（亮色主题，来源）

| 角色 | 名称 | 色值 | 用在哪里 |
| --- | --- | --- | --- |
| 页面底色 | Parchment | `#f7f7f4` | 整个应用背景 |
| 卡片 / 悬浮层表面 | Bone | `#f2f1ed` | 卡片、输入框、用户消息、表头 |
| 次级填充 | Linen | `#e6e5e0` | 次按钮、选中行、悬停背景 |
| 细线 | Stone | `#cdcdc9` | 分割线、浮层边框 |
| 主文字 / 主按钮 | Ink | `#26251e` | 正文、标题、主按钮底色 |
| 弱化图标 | Ash | `#7a7974` | 图标、占位符 |
| 强调 | Ember | `#f54e00` | **只用于**链接、标签和短语强调 |
| 辅助 | Forest `#34785c` / Verdant `#1f8a65` / Amber `#c08532` / Crimson `#cf2d56` | | 见下文用法 |

硬性规则（来源）：

- **不使用纯白 `#ffffff` 和纯黑 `#000000`**，全部用暖色近似。
- Ember 橙色**不用作按钮底色、大面积底色或图标填充**。
- 不使用渐变、发光、色彩晕染；层次只靠 1px 细线边框，其次才是柔和的暖灰阴影。
- 不使用偏蓝的冷色阴影。浏览器文字选区高亮 `#8BC4F8` 是整套系统里唯一的冷色，仅用于 `::selection`。

对比度修正（派生）：设计系统里的 Ember、Ash、Driftwood 用在小字号文字上达不到 4.5:1（Ember 在奶油底上约 3.3:1）。因此：

- 小字号（≤16px）文字链接和强调用 `--ember-text: #c43d00`；`#f54e00` 只用于大字号、加粗强调和图标。
- 次要文字统一用 `--muted-foreground: #66655f`；Ash、Driftwood、Mist 只用于图标、分割装饰和禁用态。

#### 暗色主题（派生，设计系统只提供了亮色）

保持“暖”：底色是暖黑而不是冷灰黑，文字是暖奶油而不是纯白，其余规则不变（4px 圆角、细线、无渐变）。

#### 设计令牌（映射到 shadcn/ui 变量，Tailwind v4）

```css
:root {
  --radius: 4px;                       /* 来源：按钮、卡片、输入框、标签统一 4px */
  --background: #f7f7f4;
  --foreground: #26251e;
  --card: #f2f1ed;
  --card-foreground: #26251e;
  --popover: #f2f1ed;
  --popover-foreground: #26251e;
  --primary: #26251e;                  /* 主按钮：墨色底 + 奶油字 */
  --primary-foreground: #f7f7f4;
  --secondary: #e6e5e0;                /* 次按钮：Linen 底 + 墨色字 */
  --secondary-foreground: #26251e;
  --muted: #f2f1ed;
  --muted-foreground: #66655f;         /* 派生：满足 4.5:1 */
  --accent: #e6e5e0;                   /* shadcn 的 accent 指悬停/选中底色，不是品牌强调色 */
  --accent-foreground: #26251e;
  --destructive: #cf2d56;
  --border: color-mix(in oklab, #26251e 10%, transparent);   /* 来源：5–10% 墨色细线 */
  --input: color-mix(in oklab, #26251e 10%, transparent);
  --ring: #26251e;
  --ember: #f54e00;
  --ember-text: #c43d00;               /* 派生 */
  --chart-1: #26251e;                  /* 派生：图表色板，见下文 */
  --chart-2: #34785c;
  --chart-3: #c08532;
  --chart-4: #f54e00;
  --chart-5: #cf2d56;
  --sidebar: #f7f7f4;
  --sidebar-border: color-mix(in oklab, #26251e 10%, transparent);
}

.dark {                                 /* 全部为派生值 */
  --background: #1a1915;
  --foreground: #efede6;
  --card: #22211c;
  --card-foreground: #efede6;
  --popover: #262520;
  --popover-foreground: #efede6;
  --primary: #efede6;                  /* 暗色下主按钮反转：奶油底 + 暖黑字 */
  --primary-foreground: #1a1915;
  --secondary: #2d2c26;
  --secondary-foreground: #efede6;
  --muted: #22211c;
  --muted-foreground: #a1a19f;
  --accent: #2d2c26;
  --accent-foreground: #efede6;
  --destructive: #e0486d;
  --border: color-mix(in oklab, #efede6 12%, transparent);
  --input: color-mix(in oklab, #efede6 12%, transparent);
  --ring: #efede6;
  --ember: #f54e00;
  --ember-text: #ff6a2b;
  --chart-1: #efede6;
  --chart-2: #3f9272;
  --chart-3: #d49a45;
  --chart-4: #ff6a2b;
  --chart-5: #e0486d;
  --sidebar: #1a1915;
  --sidebar-border: color-mix(in oklab, #efede6 12%, transparent);
}
```

必须覆盖 shadcn/ui 的默认值：`Button`、`Input`、`Select`、`Badge`、`Tabs`、`Card` 全部使用 `--radius` 的 4px，**不使用胶囊形（`rounded-full`）按钮、卡片和徽标**（头像可以是圆形，开关控件保持其本身的形状）；`Dialog` 和 `Sheet` 圆角 8px。


#### 形状、间距与层次

- 基准间距 4px；元素间距 8px；卡片内边距桌面 24px、移动端 16px；密度偏紧凑。
- 卡片：`Bone` 底、1px 细线、4px 圆角，默认**无阴影**；弹窗（Dialog）8px 圆角。
- 阴影只用于悬浮层和登录页的产品示意卡，且必须是暖色的：悬浮层 `0 0 1rem #00000005, 0 0 0.5rem #00000002`，示意卡 `0 28px 70px rgba(0,0,0,0.14), 0 14px 32px rgba(0,0,0,0.1)`。
- 过渡 150ms，缓动 `cubic-bezier(0.4, 0, 0.2, 1)`，只做颜色和背景的过渡，同时遵守 `prefers-reduced-motion`。
- 聚焦态（派生）：`outline: 2px solid` 墨色 60%，偏移 2px；不使用蓝色聚焦环。

#### 组件落地约定（派生，基于上述令牌）

- **按钮**：主按钮是墨色底 + 奶油字；次按钮是 Linen 底 + 墨色字；幽灵按钮无底色，文字 60% 墨色，悬停出现下划线。同一个操作组里**不放两个同等分量的填充按钮**（例如“发送”和“停止生成”：生成中只显示“停止”）。
- **应用外壳**：侧栏与页面同底色（Parchment），用 1px 右侧细线分隔；选中的导航项用 Linen 底，左侧加一条 2px 的 Forest 指示条；顶栏高 52px，透明背景，无阴影。
- **聊天**：助手回答**不加气泡**，直接平铺在纸面上；用户消息用 `Bone` 底 + 1px 细线 + 4px 圆角。输入区是 `Bone` 底、1px 细线、4px 圆角的输入框，右侧是墨色方形发送按钮。
- **表格**：表头 `Bone` 底，行与行之间只用细线分隔，不做斑马纹；悬停行 `Bone`，选中行 `Linen`；数字右对齐。
- **数据区和分析区的卡片**（关键发现、图表容器）：`Bone` 底 + 细线 + 24px 内边距。
- **状态指示**：分析任务的“进行中 / 完成 / 失败”用 8px 小圆点（Amber / Verdant / Crimson）加文字说明，**不能只靠颜色表达状态**。这是对设计系统“绿色不作状态色”的有意折中，因为分析任务确实需要状态。
- **图表色板**：按 `--chart-1` 到 `--chart-5` 的顺序取色（墨色、Forest、Amber、Ember、Crimson），超过 5 个系列后循环使用 Ash 和 Stone。单系列图表默认用墨色；**Ember 只给需要强调的那一个系列或数据点**。网格线用 Stone 半透明，柱形顶部圆角 2px，不使用渐变填充。
- **图标**：细线条（Lucide，`strokeWidth` 1.5），颜色 Ash；不使用彩色图标和插画。
- **登录页的产品示意**：做成设计系统里的“窗口示意框”——`Bone` 底卡片 + 三个灰色圆点和居中标题的窗口栏，里面用代码画一个迷你的“对话 + 柱状图”，加上示意卡阴影。不使用照片、插画和渐变背景。

---

### 字体

后端静态资源通过 Gateway 挂载，用户可能处在内网或访问 Google Fonts 不稳定的网络，所以**不使用任何运行时外链字体 CDN**，字体要么走系统字体，要么自托管。

- **★ 系统字体栈**：`system-ui, -apple-system, "PingFang SC", "Microsoft YaHei", "Noto Sans SC", sans-serif`，等宽用 `ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`。零依赖、离线可用。


### 设计要求

- 现代化、简洁、专业，对话体验接近 ChatGPT / DeepSeek 的 AI 工作台。
- 支持亮色与暗色主题：默认跟随系统，可手动切换，选择持久化到 localStorage。
- 页面响应式，适配桌面端（≥1024px）、平板（768–1023px）和移动端（<768px）。移动端规则：侧栏收成抽屉，数据区与对话区用底部标签切换，表格横向滚动，不允许整页横向滚动。
- 保持统一的字体、间距、圆角、组件状态（默认、悬停、聚焦、禁用、加载、错误）和交互风格。
- 动效克制：过渡不超过 200ms，并尊重 `prefers-reduced-motion`。
- 可访问性：全部交互元素可键盘操作，有清晰的聚焦环，图标按钮带 `aria-label`，文字对比度不低于 4.5:1。
- 界面文案全部使用简体中文。

---

## 页面需求

各页面使用的接口、路径和字段**一律以《接口设计》为准**；业务流程以《业务分析》和《需求文档》为准。接口设计中没有的能力，不做假设，不虚构。

### 实现范围

- **★ 实现全部五个业务页面**：认证、智能 Excel 助手、智能 DB 助手、我的文件、历史会话、个人中心。后端仍是占位的接口，对应页面先完成 UI 与 API 封装，并在运行时显示明确的“后端接口暂未接通”状态，接通后联调即可。

### 1. 用户认证

按《接口设计》的九个用户 HTTP 接口实现以下功能。网关处理函数接通后，通过浏览器到网关的 HTTP 请求验证：

- 昵称和邮箱唯一性检查
- 用户注册
- 密码登录（昵称或邮箱 + 密码）
- 邮箱验证码获取与登录
- 会话登录
- 退出登录
- 获取用户个人信息

流程要求：

- **启动流程**：应用加载时先读取本地缓存的 `sessionId` 尝试会话登录；成功则进入主页，失败则进入登录页。会话有效期由服务端决定（自创建起三天），前端不自行判断过期，一切以服务端响应为准。
- **登录成功后**：缓存 `result.sessionId`，随后并行请求“当前用户信息”和“支持的模型列表”，再进入主页；默认进入智能 Excel 助手，右上角显示昵称。模型列表请求失败不阻塞进入主页，模型选择器显示为不可用状态并给出原因。
- **多设备**：每次密码或验证码登录产生独立会话；退出登录只结束当前会话，清除本地 `sessionId` 并跳转登录页。
- **验证码**：六位数字，5 分钟内有效，使用一次后失效；获取验证码返回 `result.codeId`，提交时携带 `verifyCode`（字符串）。重发冷却和错误输入次数限制由前端处理，建议默认值为重发冷却 60 秒、连续输错 5 次后要求重新获取验证码，并集中定义为可调常量。
- **会话失效**：任何接口返回会话失效，统一清除本地凭证、跳转登录页并提示“登录已失效，请重新登录”。
- 登录和注册失败时，向用户展示服务端返回的具体失败原因。

### 2. 应用外壳与导航

- 实现五项导航，路由守卫：未登录一律跳转登录页。
- 右上角（或侧栏底部）显示用户头像和昵称，菜单包含：个人中心、主题切换、退出登录。
- 全局提供统一的加载骨架、空状态、错误提示和 Toast。

### 3. 智能 Excel 助手（AI 数据分析工作台）

- Excel 文件上传入口（见 V8）
- 当前文件信息展示
- AI 对话消息列表
- 用户消息输入框
- SSE 流式回答展示
- Markdown 格式渲染
- 代码高亮和回答复制
- 加载、上传、请求失败等状态
- Excel 数据预览区域（以现有 API 实际提供的数据为准）

业务流程（来自需求文档，必须遵守）：

1. 进入页面时获取支持的模型列表，供输入框下方的模型下拉使用。
2. 用户点击“新建会话”创建聊天会话；**模型在创建会话前选定，会话创建后模型选择器锁定，仅展示所用模型**。新建会话同时清空结果分析区和消息区。
3. 会话创建后才能上传 Excel；上传成功后立即发起预览，在文件预览区展示各 Worksheet（底部工作表标签切换）的表结构和数据。
4. **未上传文件直接发送消息，按普通模型聊天处理（请求不带 `fileId`）；上传文件后发送，针对该文件分析（带 `fileId`）。**
5. 回答流式显示在聊天区；分析总结、关键发现、可视化显示在“结果分析”标签（见 V5、V6）。
6. 用户可以在“文件预览”和“结果分析”之间切换，分别查看原始数据与分析结果。

### 4. 智能 DB 助手

- **首次进入且没有连接**：显示数据库连接界面，用分段控件选择 MySQL 或 SQLite。
- **MySQL 表单**：主机地址、端口号（默认 3306）、数据库名称、用户名、密码、字符集（默认 utf8mb4），点击“连接”。
- **SQLite**：三步引导——点击“浏览”选择 `.db` 文件 → “上传” → “连接”。
- **连接成功后**：接口返回所有表名、第一张表的表结构和数据。跳转到交互页，预览区默认展示第一张表，用下拉框切换其他表，并显示数据库类型标识（`MySQL` / `SQLite`）。
- **连接管理**：已有连接时再次点击“智能 DB 助手”，显示连接信息卡片，提供“使用此连接”和“断开连接”。
- **修改类 SQL 的安全说明**：执行修改类 SQL 前后端会先备份原表，再在备份表中执行。预览区提供“原表 / 备份表”切换，备份表（临时表）列表来自“获取连接中临时表列表”接口，并明确提示“修改只作用于备份表，不影响原表”。
- 聊天和结果分析区与智能 Excel 助手共用同一套组件。
- **安全**：数据库密码只在内存中使用，不写入 localStorage 或日志；连接信息卡片不回显密码。

### 5. 我的文件

- 展示当前用户上传过的所有文件（Excel 与 SQLite）
- **预览**：Excel 跳转到智能 Excel 页并展示数据；SQLite 跳转到智能 DB 页，建立对应连接并展示数据。
- **下载**：从服务器取回文件保存到本地。
- **删除**：二次确认，并明确告知“将同时删除该文件关联的聊天会话”。

### 6. 历史会话


- 展示当前用户的会话列表，会话标题默认取第一条消息的前 20 个字。
- 点击会话：切换到对应页面，加载该会话的历史消息，并显示关联的文件或数据库表信息。
- 删除会话：二次确认。

### 7. 用户界面与个人中心

- 用户头像或默认头像（昵称首字）
- 昵称及邮箱信息展示，以及在线状态（来自个人信息接口的字段）
- 退出登录
- 主题切换
- 个人中心同时展示用户上传的文件列表，操作与“我的文件”完全一致，复用同一个组件（仅当选择 S1-A 时）。
- 后端没有修改资料和上传头像的接口，所以不提供编辑功能。

### 8. 通用状态规则

- 每个会请求接口的区域都要有四种状态：加载中（骨架）、空数据（带引导）、失败（带重试）、成功。
- 对网关尚未接通的路由，不要伪装成功；显示“后端接口暂未接通”，并说明对应的接口名称，便于联调时判断。
- 会话失效、网络断开、请求超时要有区别明确的提示文案。

## 后端集成要求

先阅读 [mustRead 项目规则](../mustRead/AGENTS.md)和[项目介绍](../mustRead/Project.md)，再核对以下位置：

- [接口设计](../../docs/接口设计.md)中的 HTTP 方法、路径、请求和响应字段。
- [需求文档](../../docs/需求文档.md)和[业务分析](../../docs/业务分析.md)中的登录、注册、智能 Excel、智能 DB、文件管理和历史会话流程。
- [网关子服务说明](../../docs/网关子服务.md)和 [Gateway 路由与处理函数](../../svc_gatewayService/gatewayServiceImpl.cpp)，用于核对 HTTP 接口的实现状态，以及网关挂载静态资源的方式（是否支持 SPA 回退，见 E1）。
- [用户服务 RPC 协议](../../proto/userService.proto)，用于理解网关背后的用户服务能力。

用户接口使用 `requestId`、`sessionId` 等 HTTP 字段。POST 请求按接口设计提交 JSON，`GET /api/user/info` 通过查询参数传递 `requestId` 和 `sessionId`。密码登录和验证码登录成功时读取 `result.sessionId`，会话登录沿用原会话。获取验证码返回 `result.codeId`，提交验证码时将 `verifyCode` 作为字符串。验证码重发和错误输入限制由前端处理。

Excel 上传按接口设计先调用 `POST /api/file/upload/info` 获取 `fileId`，再向 `POST /api/file/upload` 发送文件二进制数据，预览使用 `POST /api/file/preview`。AI 对话使用 `POST /api/ai/sendStreamMessage`，请求包含登录凭证 `sessionId`、聊天会话标识 `chatSessionId`、`message` 和场景所需的可选 `fileId`。准确字段和 SSE 事件格式见[接口设计第 1.3 节及第 1.5 节](../../docs/接口设计.md)。接口接通后以实际 HTTP 响应核对。

流式响应实现要点：

- `sendStreamMessage` 是 POST 请求，浏览器原生 `EventSource` 只支持 GET 且不能带请求体，所以要用 [`@microsoft/fetch-event-source`](https://github.com/Azure/fetch-event-source) 或基于 `fetch` + `ReadableStream` 自行解析 SSE，二选一，封装在 `src/api/stream.ts`。
- 支持用 `AbortController` 中断读取（“停止生成”）。注意前端中断只停止接收，不代表后端停止生成，界面文案不要暗示后端已停止。
- 断流或出错不自动重试；已收到的内容保留，并标记“回答未完成”。

必须遵守：

1. 复用现有 C++ Gateway，禁止重新创建独立后端。
2. 不得自行引入 Supabase、Firebase 或其他用户认证系统。
3. 禁止编造尚未实现的 API。
4. API 尚未提供的能力仅保留必要的 UI 状态，不得伪装成功。
5. 将 API 请求封装为独立模块，处理错误、加载状态、会话失效和流式响应。
6. 前端不得保存后端私钥或模型 API Key。
7. 使用与现有静态资源部署方式兼容的构建方案。
8. 未经允许不得擅自修改后端代码，只允许改动前端文件。
9. 业务约束以文档为准：Excel 仅 `.xlsx` 且不超过 10 MB，SQLite 仅 `.db`，数据库仅 MySQL 和 SQLite；`sessionId` 保存在浏览器本地。

## 工程交付

- 优先采用 React + TypeScript + Vite（所选组件库对 React、Tailwind 版本有要求时，以其要求为准，例如 Streamdown 和 AI Elements 需要 React 19）。
- 使用 Tailwind CSS、shadcn/ui 和 Lucide 图标。
- 参考项目多为 Next.js，只借鉴 UI、交互和组件源码，不引入 Next.js、Server Actions、Auth.js 或 Vercel AI SDK 的服务端能力。
- 组件化设计，避免单文件堆积。
- 客户端状态（当前会话、主题、侧栏、当前文件）建议用 Zustand；服务端数据（文件列表、历史会话、模型列表）建议用 TanStack Query。请求封装统一生成 `requestId`、注入 `sessionId`、归一化错误、拦截会话失效。
- 提供清晰的目录结构和必要注释。
- 确保 TypeScript 检查与生产构建通过。
- 前端源码和构建配置均放在 `chat2data-cpp/chat2data/wwwroot`，构建产物输出到该目录内的 `dist/`。网关挂载 `wwwroot/dist`。Vite 的 `base` 使用相对路径，保证从网关挂载点下可正常加载资源。
- 开发时通过 Vite dev server 的 proxy 把 `/api` 和 `/health` 转发到网关，网关地址用环境变量配置；生产环境同源访问，不需要跨域配置。
- 提供简短的 README：开发、构建、联调方法，以及“哪些接口当前未接通”的清单。
- 不修改现有 C++ 后端业务代码，除非明确发现必须修复的接口兼容问题并先说明，批准之后才可进一步进行修改。


### 开发期数据策略

当前文件、数据库和 AI 的网关处理函数还是占位，没有 Mock 时这些页面在联调前看不到真实效果。

- **★ 不使用 Mock**：严格对接真实网关；接口占位时显示“后端接口暂未接通”。

---

请先分析项目结构和现有 API，再制定页面与组件方案，最后实现完整前端。不要只生成静态展示页面，需要确保已有业务接口能够实际调用。
