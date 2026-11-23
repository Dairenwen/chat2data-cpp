// 聊天状态：由自有 SSE 协议驱动，不使用 Vercel AI SDK 的 useChat。
import { useCallback, useRef, useState } from "react"
import type { ApiError } from "../api/client"
import { type StreamParams, sendStreamMessage } from "../api/stream"

export type ChatMessage = {
  id: string
  role: "user" | "assistant"
  content: string
  // 仅助手消息：所用模型名称。
  modelName?: string
  // 流式失败或被中断，回答未完成。
  incomplete?: boolean
  error?: ApiError
}

type Context = Omit<StreamParams, "message">

let counter = 0
const nextId = () => `m${Date.now()}-${counter++}`

export function useChat() {
  const [messages, setMessages] = useState<ChatMessage[]>([])
  const [streaming, setStreaming] = useState(false)
  const abortRef = useRef<(() => void) | null>(null)
  const lastRef = useRef<{ text: string; context: Context; modelName?: string } | null>(null)

  const patchLast = useCallback((patch: (message: ChatMessage) => ChatMessage) => {
    setMessages((list) => list.map((message, i) => (i === list.length - 1 ? patch(message) : message)))
  }, [])

  const run = useCallback((text: string, context: Context, modelName?: string) => {
    lastRef.current = { text, context, modelName }
    setStreaming(true)
    abortRef.current = sendStreamMessage({ ...context, message: text }, {
      onDelta: (delta) => patchLast((message) => ({ ...message, content: message.content + delta })),
      onDone: () => {
        abortRef.current = null
        setStreaming(false)
      },
      onError: (error) => {
        abortRef.current = null
        setStreaming(false)
        patchLast((message) => ({ ...message, incomplete: true, error }))
      },
    })
  }, [patchLast])

  const send = useCallback((text: string, context: Context, modelName?: string) => {
    setMessages((list) => [
      ...list,
      { id: nextId(), role: "user", content: text },
      { id: nextId(), role: "assistant", content: "", modelName },
    ])
    run(text, context, modelName)
  }, [run])

  // 手动重试：丢弃未完成的助手消息，重新提交同一问题。
  const retry = useCallback(() => {
    const last = lastRef.current
    if (!last || streaming) return
    setMessages((list) => [...list.slice(0, -1), { id: nextId(), role: "assistant", content: "", modelName: last.modelName }])
    run(last.text, last.context, last.modelName)
  }, [run, streaming])

  // 仅停止接收；后端不一定随之停止生成。
  const stop = useCallback(() => {
    abortRef.current?.()
    abortRef.current = null
    setStreaming(false)
    patchLast((message) => ({ ...message, incomplete: true }))
  }, [patchLast])

  const reset = useCallback((next: ChatMessage[] = []) => {
    abortRef.current?.()
    abortRef.current = null
    lastRef.current = null
    setStreaming(false)
    setMessages(next)
  }, [])

  return { messages, streaming, send, retry, stop, reset }
}
