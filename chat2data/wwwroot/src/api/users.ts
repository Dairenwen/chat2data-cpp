// 用户接口：九条 HTTP 路由，字段以接口设计 1.2 节为准。
import { request } from "./client"

export type UserInfo = { userId: string; nickname: string; email: string }

function requireId(value: unknown, message: string) {
  if (typeof value !== "string" || !value) throw new Error(message)
  return value
}

export const users = {
  validateNickname: (nickname: string) => request("/api/user/valid/nickname", { auth: false, body: { nickname } }),
  validateEmail: (email: string) => request("/api/user/valid/email", { auth: false, body: { email } }),
  register: (nickname: string, email: string, password: string) =>
    request("/api/user/register", { auth: false, body: { nickname, email, password } }),
  async passwordLogin(nickname: string, password: string) {
    const data = await request<{ sessionId: string }>("/api/user/passwd/login", { auth: false, body: { nickname, password } })
    return requireId(data?.sessionId, "网关未返回登录会话。")
  },
  async getCode(email: string) {
    const data = await request<{ codeId: string }>("/api/user/code", { auth: false, body: { email } })
    return requireId(data?.codeId, "网关未返回验证码 ID。")
  },
  async codeLogin(email: string, codeId: string, verifyCode: string) {
    const data = await request<{ sessionId: string }>("/api/user/vcode/login", {
      auth: false,
      body: { email, codeId, verifyCode },
    })
    return requireId(data?.sessionId, "网关未返回登录会话。")
  },
  sessionLogin: (sessionId: string) => request("/api/user/session/login", { auth: false, body: { sessionId } }),
  logout: (sessionId: string) =>
    request("/api/user/logout", { auth: false, silentInvalid: true, body: { sessionId } }),
  async getInfo() {
    const data = await request<{ userInfo: UserInfo }>("/api/user/info", { method: "GET" })
    const user = data?.userInfo
    if (!user || typeof user.userId !== "string" || typeof user.nickname !== "string" || typeof user.email !== "string") {
      throw new Error("网关未返回用户资料。")
    }
    return user
  },
}
