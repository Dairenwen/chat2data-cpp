# chat2Data 前端

React 19 + TypeScript + Vite + Tailwind CSS 4。前端只负责界面与 HTTP 通信，复用现有 C++ Gateway，不使用 Mock。

## 开发

```bash
cd chat2data/wwwroot
npm install
GATEWAY_URL=http://127.0.0.1:9000 npm run dev
```

`GATEWAY_URL` 为网关地址，默认 `http://127.0.0.1:9000`。Vite dev server 将 `/api` 和 `/health` 转发到该地址。

## 构建

```bash
npm run build
```

先执行 `tsc --noEmit`，再输出到 `dist/`。网关挂载 `wwwroot/dist`，`base` 为相对路径。

网关没有 SPA 回退，前端使用 Hash 路由（如 `/#/excel`），刷新和直接访问均可加载。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/api` | 请求封装（`client.ts`）、SSE（`stream.ts`）、用户 / 文件 / 数据库 / AI 接口 |
| `src/store` | zustand：登录会话、主题、Toast、数据库连接 |
| `src/hooks` | 聊天流式、Excel 上传、模型列表 |
| `src/components` | 通用组件与工作台组件 |
| `src/pages` | 智能 Excel、智能 DB、我的文件、历史会话、个人中心 |

## 联调说明

- 网关占位路由返回 `501`，前端显示“后端接口暂未接通：<接口路径>”。前端兼容旧网关的 `200` 空响应体和 `404`。
- `sessionId` 保存在 `localStorage`；数据库密码只在内存中使用。
- 接口设计未定义分析任务、关键发现、总结和可视化的 SSE 字段，结果分析区暂显示空状态，`ChartRenderer` 仅识别 `{ type, data: [{ name, value }] }`。
- DB 场景的 `sessionType` 取值（当前使用 `db`）待后端确认。

## 当前未接通的接口

用户接口 9 条已由网关转发，其余为占位。

| 分类 | 接口 |
| --- | --- |
| 文件 | `POST /api/file/upload/info`、`GET /api/file/info`、`POST /api/file/upload`、`GET /api/file/download`、`DELETE /api/file/{fileId}`、`POST /api/file/preview`、`POST /api/file/list`、`POST /api/file/chat/map`、`POST /api/file/sqlite/upload` |
| 数据库 | `POST /api/db/connect`、`GET /api/db/tables`、`POST /api/db/disconnect`、`POST /api/db/table/data`、`POST /api/db/connection/status` |
| AI | `POST /api/ai/models`、`POST /api/ai/session/create`、`POST /api/ai/chatSessionLists`、`POST /api/ai/history`、`POST /api/ai/delete`、`POST /api/ai/sendStreamMessage` |

接口接通后以实际 HTTP 响应核对字段。
