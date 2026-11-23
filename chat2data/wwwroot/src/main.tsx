// 入口：启动流程、路由守卫、会话失效统一处理。
import { QueryClientProvider } from "@tanstack/react-query"
import { useCallback, useEffect, useState } from "react"
import { createRoot } from "react-dom/client"
import { ai } from "./api/ai"
import { configureClient, errorMessage } from "./api/client"
import { users } from "./api/users"
import { AuthPage } from "./AuthPage"
import { AppShell } from "./components/AppShell"
import { Brand } from "./components/Brand"
import { Toaster } from "./components/Toaster"
import { Button } from "./components/ui"
import { queryClient } from "./lib/query"
import { useHashRoute } from "./lib/router"
import { applyTheme, useApp } from "./store/app"
import { toast } from "./store/toast"
import { Workspace } from "./Workspace"
import "./styles.css"

const sessionExpiredText = "登录已失效，请重新登录"

configureClient({
  getSession: () => useApp.getState().sessionId,
  // 任何接口返回会话失效：清除本地凭证、跳转登录页并提示。
  onSessionInvalid: () => {
    if (!useApp.getState().sessionId) return
    useApp.getState().clearAuth()
    queryClient.clear()
    toast.error(sessionExpiredText)
  },
})
applyTheme(useApp.getState().theme)

function App() {
  const { sessionId, user, setSession, setUser, clearAuth } = useApp()
  const { route } = useHashRoute()
  const [booting, setBooting] = useState(Boolean(sessionId))
  const [bootError, setBootError] = useState("")

  // 启动：先会话登录，再并行请求用户信息和模型列表；模型列表失败不阻塞进入主页。
  const enter = useCallback(async (id: string, restore: boolean) => {
    setBooting(true)
    setBootError("")
    try {
      if (restore) await users.sessionLogin(id)
      const [info] = await Promise.all([
        users.getInfo(),
        queryClient.fetchQuery({ queryKey: ["models"], queryFn: ai.models }).catch(() => undefined),
      ])
      setUser(info)
    } catch (failure) {
      // 会话失效已由请求层统一处理；其余错误允许用户重试。
      if (useApp.getState().sessionId) setBootError(errorMessage(failure))
    } finally {
      setBooting(false)
    }
  }, [setUser])

  useEffect(() => {
    const saved = useApp.getState().sessionId
    if (saved) void enter(saved, true)
  }, [enter])

  function login(id: string) {
    setSession(id)
    void enter(id, false)
  }

  async function logout() {
    const id = useApp.getState().sessionId
    try {
      if (id) await users.logout(id)
    } catch {
      // 服务端退出失败时仍清除本地凭证，避免用户被卡在已登录界面。
    }
    clearAuth()
    queryClient.clear()
  }

  if (!sessionId) return <AuthPage onLogin={login} />
  if (!user) {
    return (
      <main className="grid min-h-dvh place-items-center p-6">
        <div className="grid justify-items-center gap-4 text-center">
          <Brand />
          {booting ? <p role="status" className="text-sm text-muted-foreground">正在验证登录会话…</p> : (
            <>
              <p role="alert" className="text-sm text-destructive">{bootError}</p>
              <Button onClick={() => void enter(sessionId, true)}>重试</Button>
            </>
          )}
        </div>
      </main>
    )
  }
  return (
    <AppShell route={route} user={user} onLogout={() => void logout()}>
      <Workspace route={route} onLogout={() => void logout()} />
    </AppShell>
  )
}

createRoot(document.getElementById("root")!).render(
  <QueryClientProvider client={queryClient}>
    <App />
    <Toaster />
  </QueryClientProvider>,
)
