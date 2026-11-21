import { useCallback, useEffect, useState } from "react"
import { createRoot } from "react-dom/client"
import { AuthPage } from "./AuthPage"
import { type UserInfo, errorMessage, sessionInvalid, users } from "./api/users"
import { Brand } from "./components/Brand"
import { Button } from "./components/ui"
import { Workspace } from "./Workspace"
import "./styles.css"

const sessionKey = "chat2data.session"
const themeKey = "chat2data.theme"

function App() {
  const [savedSession] = useState(() => localStorage.getItem(sessionKey))
  const [session, setSession] = useState(savedSession)
  const [user, setUser] = useState<UserInfo>()
  const [pending, setPending] = useState(Boolean(savedSession))
  const [error, setError] = useState("")
  const [dark, setDark] = useState(() => {
    const theme = localStorage.getItem(themeKey)
    return theme ? theme === "dark" : window.matchMedia("(prefers-color-scheme: dark)").matches
  })

  useEffect(() => {
    document.documentElement.dataset.theme = dark ? "dark" : "light"
    localStorage.setItem(themeKey, dark ? "dark" : "light")
  }, [dark])

  const clearSession = useCallback(() => {
    localStorage.removeItem(sessionKey)
    setSession(null)
    setUser(undefined)
    setError("")
  }, [])

  const loadProfile = useCallback(async (sessionId: string, restore: boolean) => {
    setPending(true)
    setError("")
    try {
      // 从浏览器恢复的会话需要先完成会话登录，再读取用户资料。
      if (restore) await users.sessionLogin(sessionId)
      setUser(await users.getInfo(sessionId))
    } catch (failure) {
      if (sessionInvalid(failure)) clearSession()
      else setError(errorMessage(failure))
    } finally {
      setPending(false)
    }
  }, [clearSession])

  useEffect(() => {
    if (savedSession) void loadProfile(savedSession, true)
  }, [savedSession, loadProfile])

  function login(sessionId: string) {
    localStorage.setItem(sessionKey, sessionId)
    setSession(sessionId)
    void loadProfile(sessionId, false)
  }

  async function logout() {
    if (!session) return
    setPending(true)
    setError("")
    try {
      await users.logout(session)
      clearSession()
    } catch (failure) {
      if (sessionInvalid(failure)) clearSession()
      else setError(errorMessage(failure))
    } finally {
      setPending(false)
    }
  }

  const theme = { dark, toggleTheme: () => setDark(!dark) }
  if (!session) return <AuthPage onLogin={login} {...theme} />
  if (user) return <Workspace user={user} onLogout={logout} pending={pending} error={error} {...theme} />

  return (
    <main className="session-screen">
      <section>
        <Brand />
        {pending ? <p role="status">正在验证登录会话…</p> : (
          <>
            <p role="alert" className="form-error">{error}</p>
            <Button onClick={() => loadProfile(session, true)}>重试</Button>
          </>
        )}
      </section>
    </main>
  )
}

createRoot(document.getElementById("root")!).render(<App />)
