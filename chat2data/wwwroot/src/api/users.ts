export type UserInfo = { userId: string; nickname: string; email: string }

export const userError = {
  sessionNotFound: 107,
  sessionExpired: 108,
  codeMismatch: 109,
  codeMissing: 110,
}

export class ApiError extends Error {
  constructor(message: string, public code: number, public status: number) {
    super(message)
  }
}

export function sessionInvalid(error: unknown) {
  return error instanceof ApiError && (
    error.status === 401 || error.code === userError.sessionNotFound || error.code === userError.sessionExpired
  )
}

export function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : "请求失败，请重试。"
}

async function call<T>(path: string, fields: Record<string, string>, method = "POST"): Promise<T> {
  // getRandomValues 同样适用于开发环境的普通 HTTP 地址。
  const bytes = crypto.getRandomValues(new Uint8Array(16))
  const requestId = Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("")
  const params = { requestId, ...fields }
  const isGet = method === "GET"
  const url = isGet ? `${path}?${new URLSearchParams(params)}` : path
  let response: Response
  try {
    response = await fetch(url, {
      method,
      headers: isGet ? undefined : { "Content-Type": "application/json" },
      body: isGet ? undefined : JSON.stringify(params),
      cache: "no-store",
      signal: AbortSignal.timeout(45_000),
    })
  } catch {
    throw new Error("无法连接网关或请求超时，请重试。")
  }

  const reply = await response.json().catch(() => null)
  if (reply?.requestId !== requestId || !Number.isInteger(reply.errorCode) ||
      typeof reply.errorMsg !== "string") {
    throw new Error("网关响应格式有误，请重试。")
  }
  if (!response.ok || reply.errorCode !== 0) {
    throw new ApiError(reply.errorMsg || "用户请求失败。", reply.errorCode, response.status)
  }
  return reply.result
}

function requireId(value: unknown, message: string) {
  if (typeof value !== "string" || !value) throw new Error(message)
  return value
}

export const users = {
  validateNickname: (nickname: string) => call<void>("/api/user/valid/nickname", { nickname }),
  validateEmail: (email: string) => call<void>("/api/user/valid/email", { email }),
  register: (nickname: string, email: string, password: string) =>
    call<void>("/api/user/register", { nickname, email, password }),
  async passwordLogin(nickname: string, password: string) {
    const data = await call<{ sessionId: string }>("/api/user/passwd/login", { nickname, password })
    return requireId(data?.sessionId, "网关未返回登录会话。")
  },
  async getCode(email: string) {
    const data = await call<{ codeId: string }>("/api/user/code", { email })
    return requireId(data?.codeId, "网关未返回验证码 ID。")
  },
  async codeLogin(email: string, codeId: string, verifyCode: string) {
    const data = await call<{ sessionId: string }>("/api/user/vcode/login", { email, codeId, verifyCode })
    return requireId(data?.sessionId, "网关未返回登录会话。")
  },
  sessionLogin: (sessionId: string) => call<void>("/api/user/session/login", { sessionId }),
  logout: (sessionId: string) => call<void>("/api/user/logout", { sessionId }),
  async getInfo(sessionId: string) {
    const data = await call<{ userInfo: UserInfo }>("/api/user/info", { sessionId }, "GET")
    const user = data?.userInfo
    if (!user || typeof user.userId !== "string" || typeof user.nickname !== "string" ||
        typeof user.email !== "string") {
      throw new Error("网关未返回用户资料。")
    }
    return user
  },
}
