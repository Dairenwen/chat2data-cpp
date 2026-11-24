// 统一请求封装：生成 requestId、注入 sessionId、归一化错误、拦截会话失效。
export const userError = { sessionNotFound: 107, sessionExpired: 108, codeMismatch: 109, codeMissing: 110 }

export type ErrorKind = "network" | "timeout" | "aborted" | "format" | "session" | "unavailable" | "business"

export class ApiError extends Error {
  constructor(message: string, public kind: ErrorKind, public code = 0, public status = 0) {
    super(message)
  }
}

type Scalar = string | number | boolean | undefined
type Options = {
  method?: "GET" | "POST" | "DELETE"
  // false 时不注入当前登录会话，由调用方在 body 中自行提供 sessionId。
  auth?: boolean
  query?: Record<string, Scalar>
  body?: Record<string, unknown>
  signal?: AbortSignal
  timeoutMs?: number
  // 退出登录场景：会话失效时不触发全局跳转。
  silentInvalid?: boolean
  // 个别接口（获取连接临时表列表）的数据位于响应顶层而不在 result 内。
  whole?: boolean
}

const hooks = { getSession: (): string | null => null, onSessionInvalid: () => {} }

export function configureClient(next: Partial<typeof hooks>) {
  Object.assign(hooks, next)
}
export const getSessionId = () => hooks.getSession()
export const reportSessionInvalid = () => hooks.onSessionInvalid()

export function newRequestId() {
  // getRandomValues 同样适用于开发环境的普通 HTTP 地址。
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("")
}

export const isSessionInvalid = (code: number, status: number) =>
  status === 401 || code === userError.sessionNotFound || code === userError.sessionExpired

export const isUnavailable = (error: unknown) => error instanceof ApiError && error.kind === "unavailable"
export const errorMessage = (error: unknown) => (error instanceof Error ? error.message : "请求失败，请重试。")

export function unavailableError(path: string, status = 0) {
  return new ApiError(`后端接口暂未接通：${path}`, "unavailable", 0, status)
}

function toQuery(fields: Record<string, Scalar>) {
  const params = new URLSearchParams()
  for (const [key, value] of Object.entries(fields)) if (value !== undefined) params.set(key, String(value))
  return params.toString()
}

// 网络层异常归一化为 ApiError，区分取消、超时和断网。
export function transportError(error: unknown) {
  const name = error instanceof DOMException ? error.name : ""
  if (name === "AbortError") return new ApiError("请求已取消。", "aborted")
  if (name === "TimeoutError") return new ApiError("请求超时，请稍后重试。", "timeout")
  return new ApiError("无法连接网关，请检查网络后重试。", "network")
}

// 兼容返回空响应的旧网关和返回 501 的占位路由。
export function isUnavailableReply(response: Response, text: string) {
  return response.status === 501 || response.status === 404 || (response.ok && !text.trim())
}

export async function parseReply<T>(path: string, response: Response, requestId: string, silentInvalid = false, whole = false): Promise<T> {
  const text = await response.text()
  if (isUnavailableReply(response, text)) throw unavailableError(path, response.status)
  let reply: { requestId?: string; errorCode?: unknown; errorMsg?: unknown; result?: T } | null = null
  try {
    reply = JSON.parse(text)
  } catch {
    reply = null
  }
  if (!reply || !Number.isInteger(reply.errorCode) || typeof reply.errorMsg !== "string") {
    throw new ApiError("网关响应格式有误，请稍后重试。", "format", 0, response.status)
  }
  const code = reply.errorCode as number
  if (response.ok && code === 0) {
    if (reply.requestId !== requestId) throw new ApiError("网关响应格式有误，请稍后重试。", "format", 0, response.status)
    return (whole ? reply : reply.result) as unknown as T
  }
  if (isSessionInvalid(code, response.status)) {
    if (!silentInvalid) hooks.onSessionInvalid()
    throw new ApiError(reply.errorMsg || "登录已失效，请重新登录。", "session", code, response.status)
  }
  throw new ApiError(reply.errorMsg || "请求失败。", "business", code, response.status)
}

export async function request<T = undefined>(path: string, options: Options = {}): Promise<T> {
  const { method = "POST", auth = true, query = {}, body = {}, signal, timeoutMs = 45_000, silentInvalid, whole } = options
  const requestId = newRequestId()
  const common = { requestId, ...(auth ? { sessionId: hooks.getSession() ?? "" } : {}) }
  const timeout = AbortSignal.timeout(timeoutMs)
  const init: RequestInit = { method, cache: "no-store", signal: signal ? AbortSignal.any([signal, timeout]) : timeout }
  let url = path
  if (method === "POST") {
    init.headers = { "Content-Type": "application/json" }
    init.body = JSON.stringify({ ...common, ...body })
  } else {
    url = `${path}?${toQuery({ ...common, ...query })}`
  }
  let response: Response
  try {
    response = await fetch(url, init)
  } catch (error) {
    throw transportError(error)
  }
  return parseReply<T>(path, response, requestId, silentInvalid, whole)
}

// 下载二进制文件；失败时网关返回 JSON 错误体。
export async function requestBlob(path: string, query: Record<string, Scalar>) {
  const requestId = newRequestId()
  const url = `${path}?${toQuery({ requestId, sessionId: hooks.getSession() ?? "", ...query })}`
  let response: Response
  try {
    response = await fetch(url, { cache: "no-store", signal: AbortSignal.timeout(120_000) })
  } catch (error) {
    throw transportError(error)
  }
  const type = response.headers.get("content-type") ?? ""
  if (response.ok && !type.includes("json")) {
    const blob = await response.blob()
    if (!blob.size) throw unavailableError(path, response.status)
    return blob
  }
  await parseReply(path, response, requestId)
  throw new ApiError("网关响应格式有误，请稍后重试。", "format", 0, response.status)
}
