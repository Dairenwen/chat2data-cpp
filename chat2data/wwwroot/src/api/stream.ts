// SSE 流式封装：sendStreamMessage 是 POST 请求，使用 fetch-event-source 解析。
import { fetchEventSource } from "@microsoft/fetch-event-source"
import { ApiError, getSessionId, isSessionInvalid, isUnavailableReply, newRequestId, reportSessionInvalid, unavailableError } from "./client"

const path = "/api/ai/sendStreamMessage"

export type StreamParams = {
  chatSessionId: string
  message: string
  fileId?: string
  connectionId?: string
  databaseName?: string
  tableName?: string
}

export type StreamHandlers = {
  onDelta: (text: string) => void
  onDone: () => void
  onError: (error: ApiError) => void
}

function businessError(code: number, message: string, status: number) {
  const invalid = isSessionInvalid(code, status)
  if (invalid) reportSessionInvalid()
  return new ApiError(message || "AI 服务返回错误。", invalid ? "session" : "business", code, status)
}

async function openError(response: Response) {
  const text = await response.text()
  if (isUnavailableReply(response, text)) return unavailableError(path, response.status)
  try {
    const reply = JSON.parse(text)
    if (Number.isInteger(reply.errorCode) && reply.errorCode !== 0) {
      return businessError(reply.errorCode, reply.errorMsg, response.status)
    }
  } catch {
    // 非 JSON 内容按格式错误处理。
  }
  if (response.status === 401) return businessError(0, "", 401)
  return new ApiError("网关响应格式有误，请稍后重试。", "format", 0, response.status)
}

// 发送消息并逐帧回调；返回的函数用于中断接收（后端不一定随之停止生成）。
// 断流或出错不自动重试，由用户手动触发重试。
export function sendStreamMessage(params: StreamParams, handlers: StreamHandlers) {
  const controller = new AbortController()
  let finished = false
  const finish = (error?: ApiError) => {
    if (finished) return
    finished = true
    if (error) handlers.onError(error)
    else handlers.onDone()
  }

  fetchEventSource(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ requestId: newRequestId(), sessionId: getSessionId() ?? "", ...params }),
    signal: controller.signal,
    openWhenHidden: true,
    async onopen(response) {
      const type = response.headers.get("content-type") ?? ""
      if (response.ok && type.includes("text/event-stream")) return
      throw await openError(response)
    },
    onmessage(event) {
      if (finished) return
      if (event.data === "[DONE]") {
        finish()
        return
      }
      let frame: { content?: string; errorCode?: number; errorMsg?: string }
      try {
        frame = JSON.parse(event.data)
      } catch {
        throw new ApiError("流式响应格式有误。", "format")
      }
      if (frame.errorCode) throw businessError(frame.errorCode, frame.errorMsg ?? "", 0)
      if (frame.content) handlers.onDelta(frame.content)
    },
    onclose() {
      if (!finished) throw new ApiError("连接意外中断，回答未完成。", "network")
    },
    onerror(error) {
      throw error
    },
  }).catch((error) => {
    if (controller.signal.aborted) {
      finished = true
      return
    }
    finish(error instanceof ApiError ? error : new ApiError("无法连接网关，回答未完成。", "network"))
  })

  return () => controller.abort()
}
