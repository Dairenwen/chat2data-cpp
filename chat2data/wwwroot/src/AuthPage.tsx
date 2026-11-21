import { FileSpreadsheet, MessageSquare, Table2 } from "lucide-react"
import { type FormEvent, useEffect, useState } from "react"
import { ApiError, errorMessage, userError, users } from "./api/users"
import { Brand, ThemeButton } from "./components/Brand"
import { Button, Input } from "./components/ui"

const resendDelay = 60_000
const attemptLimit = 5

type Challenge = { codeId: string; resendAt: number; attempts: number }

type Props = {
  onLogin: (sessionId: string) => void
  dark: boolean
  toggleTheme: () => void
}

export function AuthPage({ onLogin, dark, toggleTheme }: Props) {
  const [registering, setRegistering] = useState(false)
  const [method, setMethod] = useState<"password" | "code">("password")
  const [account, setAccount] = useState("")
  const [nickname, setNickname] = useState("")
  const [email, setEmail] = useState("")
  const [password, setPassword] = useState("")
  const [confirmation, setConfirmation] = useState("")
  const [code, setCode] = useState("")
  const [challenges, setChallenges] = useState<Record<string, Challenge>>({})
  const [now, setNow] = useState(Date.now)
  const [pending, setPending] = useState(false)
  const [message, setMessage] = useState("")
  const [error, setError] = useState("")
  const [nicknameError, setNicknameError] = useState("")
  const [emailError, setEmailError] = useState("")
  const address = email.trim()
  const challenge = challenges[address]
  const countdown = challenge ? Math.max(0, Math.ceil((challenge.resendAt - now) / 1000)) : 0

  useEffect(() => {
    if (countdown === 0) return
    const timer = window.setInterval(() => setNow(Date.now()), 1000)
    return () => clearInterval(timer)
  }, [countdown === 0])

  function resetFeedback() {
    setMessage("")
    setError("")
    setNicknameError("")
    setEmailError("")
  }

  function switchPage() {
    setRegistering(!registering)
    setPassword("")
    setConfirmation("")
    setCode("")
    resetFeedback()
  }

  async function sendCode() {
    resetFeedback()
    if (!/^[^\s@]+@[^\s@]+$/.test(address)) {
      setEmailError("请输入注册时使用的邮箱。")
      return
    }
    if (challenge && Date.now() < challenge.resendAt) return
    setPending(true)
    try {
      const codeId = await users.getCode(address)
      const sentAt = Date.now()
      setChallenges((current) => ({
        ...current, [address]: { codeId, resendAt: sentAt + resendDelay, attempts: 0 },
      }))
      setNow(sentAt)
      setCode("")
      setMessage("验证码已发送，请查收邮件。")
    } catch (failure) {
      setError(errorMessage(failure))
    } finally {
      setPending(false)
    }
  }

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    resetFeedback()
    setPending(true)
    try {
      if (registering) {
        const name = nickname.trim()
        const classes = [/[a-z]/, /[A-Z]/, /[0-9]/, /[^a-zA-Z0-9\s]/]
        if (Array.from(password).length < 8 || classes.filter((pattern) => pattern.test(password)).length < 2) {
          setError("密码至少 8 位，且包含两类字符。")
          return
        }
        if (password !== confirmation) {
          setError("两次输入的密码不一致。")
          return
        }
        const [nameCheck, emailCheck] = await Promise.allSettled([
          users.validateNickname(name), users.validateEmail(address),
        ])
        if (nameCheck.status === "rejected") setNicknameError(errorMessage(nameCheck.reason))
        if (emailCheck.status === "rejected") setEmailError(errorMessage(emailCheck.reason))
        if (nameCheck.status === "rejected" || emailCheck.status === "rejected") return
        await users.register(name, address, password)
        setAccount(name)
        setPassword("")
        setConfirmation("")
        setRegistering(false)
        setMethod("password")
        setMessage("注册成功，请登录。")
        return
      }

      if (method === "password") {
        onLogin(await users.passwordLogin(account.trim(), password))
        return
      }

      if (!challenge?.codeId || challenge.attempts >= attemptLimit) {
        setError("请先获取新的验证码。")
        return
      }
      try {
        onLogin(await users.codeLogin(address, challenge.codeId, code))
      } catch (failure) {
        if (failure instanceof ApiError && failure.code === userError.codeMismatch) {
          const attempts = challenge.attempts + 1
          setChallenges((current) => ({ ...current, [address]: { ...challenge, attempts } }))
          if (attempts === attemptLimit) {
            setError("验证码输入错误次数已达上限，请重新获取。")
            return
          }
        }
        if (failure instanceof ApiError && failure.code === userError.codeMissing) {
          setChallenges((current) => ({ ...current, [address]: { ...challenge, codeId: "" } }))
        }
        throw failure
      }
    } catch (failure) {
      setError(errorMessage(failure))
    } finally {
      setPending(false)
    }
  }

  return (
    <main className="auth-layout">
      <aside className="auth-feature" aria-label="Chat2Data 智能 Excel 助手">
        <Brand />
        <div className="feature-main">
          <span className="feature-icon"><FileSpreadsheet size={29} /></span>
          <h2>用自然语言分析 Excel</h2>
          <p>上传工作簿、预览工作表，在同一个工作台追问数据。</p>
          <div className="feature-illustration" aria-hidden="true">
            <div className="illustration-table">
              <div><Table2 size={17} /> 工作表预览</div>
              <div className="illustration-grid"><i /><i /><i /><i /><i /><i /><i /><i /><i /></div>
            </div>
            <div className="illustration-chat"><MessageSquare size={18} /> 智能 Excel 助手</div>
          </div>
        </div>
      </aside>

      <section className="auth-content">
        <div className="auth-toolbar">
          <div className="mobile-brand"><Brand /></div>
          <ThemeButton dark={dark} toggle={toggleTheme} />
        </div>
        <div className="auth-card">
          <h1>{registering ? "创建账号" : "登录 Chat2Data"}</h1>
          <p className="auth-subtitle">{registering ? "注册后即可登录工作台。" : "登录后进入智能 Excel 工作台。"}</p>
          {!registering && (
            <div className="segmented" role="group" aria-label="登录方式">
              <button type="button" aria-pressed={method === "password"} disabled={pending}
                onClick={() => { setMethod("password"); resetFeedback() }}>密码登录</button>
              <button type="button" aria-pressed={method === "code"} disabled={pending}
                onClick={() => { setMethod("code"); resetFeedback() }}>邮箱验证码</button>
            </div>
          )}
          <form className="auth-form" onSubmit={submit}>
            <fieldset disabled={pending}>
              {registering && (
                <label>昵称
                  <Input required maxLength={32} autoComplete="username" value={nickname}
                    onChange={(event) => { setNickname(event.target.value); setNicknameError("") }} />
                  {nicknameError && <span role="alert" className="form-error">{nicknameError}</span>}
                </label>
              )}
              {(registering || method === "code") ? (
                <label>邮箱
                  <Input required type="email" maxLength={255} autoComplete="email" value={email}
                    onChange={(event) => { setEmail(event.target.value); setEmailError(""); setCode(""); setMessage("") }} />
                  {emailError && <span role="alert" className="form-error">{emailError}</span>}
                </label>
              ) : (
                <label>昵称或邮箱
                  <Input required autoComplete="username" value={account}
                    onChange={(event) => setAccount(event.target.value)} />
                </label>
              )}
              {(registering || method === "password") && (
                <label>密码
                  <Input type="password" required autoComplete={registering ? "new-password" : "current-password"}
                    value={password} onChange={(event) => setPassword(event.target.value)} />
                  {registering && <span className="field-note">至少 8 位，包含数字、字母或符号中的至少两类字符。</span>}
                </label>
              )}
              {registering && (
                <label>确认密码
                  <Input type="password" required autoComplete="new-password" value={confirmation}
                    onChange={(event) => setConfirmation(event.target.value)} />
                </label>
              )}
              {!registering && method === "code" && (
                <div className="code-field">
                  <label>验证码
                    <Input inputMode="numeric" autoComplete="one-time-code" pattern="[0-9]{6}" maxLength={6} required
                      value={code} onChange={(event) => setCode(event.target.value)} />
                  </label>
                  <Button variant="outline" onClick={sendCode} disabled={countdown > 0}>
                    {countdown > 0 ? `${countdown} 秒后重发` : "获取验证码"}
                  </Button>
                </div>
              )}
              {message && <p className="form-status" role="status">{message}</p>}
              {error && <p className="form-error" role="alert">{error}</p>}
              <Button type="submit" className="w-full">
                {pending ? "处理中…" : registering ? "注册" : "登录"}
              </Button>
            </fieldset>
          </form>
          <div className="auth-footer">
            {registering ? "已有账号？" : "还没有账号？"}
            <button type="button" onClick={switchPage} disabled={pending}>
              {registering ? "返回登录" : "创建账号"}
            </button>
          </div>
        </div>
      </section>
    </main>
  )
}
