// AI 接口：字段以接口设计 1.5 节为准；流式消息见 stream.ts。
import { request } from "./client"

// 接口设计仅给出 excel 示例，DB 场景取值待后端确认。
export const sessionTypes = { excel: "excel", db: "db" } as const
export type SessionType = (typeof sessionTypes)[keyof typeof sessionTypes]

export type ModelItem = { modelConfigId: string; modelName: string; modelDesc: string }
export type CreatedSession = { chatSessionId: string; modelConfigId: string; modelName: string }

export type ChatSession = {
  chatSessionId: string
  modelName: string
  title: string
  createdAt: number
  updatedAt: number
  messageCount: number
  firstUserMessageContent: string
}

export type HistoryMessage = { id: string; role: string; content: string; timestamp: number }
export type ChatHistory = {
  messageList: HistoryMessage[]
  fileId?: string
  sessionType?: string
  dbConnectionInfo?: string
}

export const ai = {
  async models() {
    const data = await request<{ modelList: ModelItem[] }>("/api/ai/models")
    return data?.modelList ?? []
  },
  createSession: (modelConfigId: string, sessionType: SessionType, title?: string) =>
    request<CreatedSession>("/api/ai/session/create", { body: { modelConfigId, sessionType, title } }),
  async sessions() {
    const data = await request<{ chatSessionLists: ChatSession[] }>("/api/ai/chatSessionLists")
    return data?.chatSessionLists ?? []
  },
  history: (chatSessionId: string) => request<ChatHistory>("/api/ai/history", { body: { chatSessionId } }),
  deleteSession: (chatSessionId: string) => request("/api/ai/delete", { body: { chatSessionId } }),
}
