// 认证页：密码登录、验证码登录、注册。不提供“忘记密码”和第三方登录。
import { zodResolver } from "@hookform/resolvers/zod"
import { useEffect, useRef, useState } from "react"
import { Controller, useForm } from "react-hook-form"
import { z } from "zod"
import { ApiError, errorMessage } from "./api/client"
import { userError } from "./api/client"
import { users } from "./api/users"
import { AuthShowcase } from "./components/AuthShowcase"
import { Brand } from "./components/Brand"
import { OtpInput } from "./components/OtpInput"
import { PasswordInput } from "./components/PasswordInput"
import { PasswordRules, isPasswordValid } from "./components/PasswordRules"
import { Button, Input } from "./components/ui"
import { codeAttemptLimit, resendCooldownMs } from "./lib/constants"
import { cn } from "./lib/utils"

type Props = { onLogin: (sessionId: string) => void; notice?: string }
type Mode = "password" | "code" | "register"
type Challenge = { codeId: string; resendAt: number; attempts: number }

const emailPattern = /^[^\s@]+@[^\s@]+\.[^\s@]+$/

function FieldError({ text }: { text?: string }) {
  return text ? <p role="alert" className="text-xs text-destructive">{text}</p> : null
}

function Field({ label, error, children }: { label: string; error?: string; children: React.ReactNode }) {
  return (
    <label className="grid gap-1.5 text-sm">
      {label}
      {children}
      <FieldError text={error} />
    </label>
  )
}

// 登录失败直接展示服务端返回的具体原因。
function FormMessage({ error, status }: { error: string; status?: string }) {
  return (
    <>
      {status && <p role="status" className="text-sm text-verdant">{status}</p>}
      {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
    </>
  )
}

const passwordLoginSchema = z.object({
  account: z.string().trim().min(1, "请输入昵称或邮箱"),
  password: z.string().min(1, "请输入密码"),
})

function PasswordLogin({ onLogin, onDone }: { onLogin: Props["onLogin"]; onDone: () => void }) {
  const form = useForm<z.infer<typeof passwordLoginSchema>>({ resolver: zodResolver(passwordLoginSchema), defaultValues: { account: "", password: "" } })
  const [error, setError] = useState("")
  const { errors, isSubmitting } = form.formState

  const submit = form.handleSubmit(async (values) => {
    setError("")
    try {
      onLogin(await users.passwordLogin(values.account, values.password))
    } catch (failure) {
      setError(errorMessage(failure))
      onDone()
    }
  })

  return (
    <form onSubmit={submit} className="grid gap-4" noValidate>
      <Field label="昵称或邮箱" error={errors.account?.message}>
        <Input autoComplete="username" aria-invalid={!!errors.account} {...form.register("account")} />
      </Field>
      <Field label="密码" error={errors.password?.message}>
        <PasswordInput autoComplete="current-password" aria-invalid={!!errors.password} {...form.register("password")} />
      </Field>
      <FormMessage error={error} />
      <Button type="submit" disabled={isSubmitting}>{isSubmitting ? "登录中…" : "登录"}</Button>
    </form>
  )
}

const codeLoginSchema = z.object({
  email: z.string().trim().regex(emailPattern, "请输入注册时使用的邮箱"),
  code: z.string().regex(/^\d{6}$/, "请输入 6 位数字验证码"),
})

function CodeLogin({ onLogin }: { onLogin: Props["onLogin"] }) {
  const form = useForm<z.infer<typeof codeLoginSchema>>({ resolver: zodResolver(codeLoginSchema), defaultValues: { email: "", code: "" } })
  const [challenges, setChallenges] = useState<Record<string, Challenge>>({})
  const [now, setNow] = useState(Date.now)
  const [sending, setSending] = useState(false)
  const [error, setError] = useState("")
  const [status, setStatus] = useState("")
  const { errors, isSubmitting } = form.formState
  const email = form.watch("email").trim()
  const challenge = challenges[email]
  const countdown = challenge ? Math.max(0, Math.ceil((challenge.resendAt - now) / 1000)) : 0

  useEffect(() => {
    if (countdown === 0) return
    const timer = window.setInterval(() => setNow(Date.now()), 1000)
    return () => clearInterval(timer)
  }, [countdown === 0])

  async function sendCode() {
    setError("")
    setStatus("")
    if (!(await form.trigger("email"))) return
    setSending(true)
    try {
      const codeId = await users.getCode(email)
      const sentAt = Date.now()
      setChallenges((current) => ({ ...current, [email]: { codeId, resendAt: sentAt + resendCooldownMs, attempts: 0 } }))
      setNow(sentAt)
      form.setValue("code", "")
      setStatus("验证码已发送，5 分钟内有效，使用一次后失效。")
    } catch (failure) {
      setError(errorMessage(failure))
    } finally {
      setSending(false)
    }
  }

  const submit = form.handleSubmit(async (values) => {
    setError("")
    setStatus("")
    if (!challenge?.codeId || challenge.attempts >= codeAttemptLimit) {
      setError("请先获取新的验证码。")
      return
    }
    try {
      onLogin(await users.codeLogin(email, challenge.codeId, values.code))
    } catch (failure) {
      if (failure instanceof ApiError && failure.code === userError.codeMismatch) {
        const attempts = challenge.attempts + 1
        setChallenges((current) => ({ ...current, [email]: { ...challenge, attempts } }))
        if (attempts >= codeAttemptLimit) {
          setError("验证码输入错误次数已达上限，请重新获取。")
          return
        }
      }
      if (failure instanceof ApiError && failure.code === userError.codeMissing) {
        setChallenges((current) => ({ ...current, [email]: { ...challenge, codeId: "" } }))
      }
      setError(errorMessage(failure))
    }
  })

  return (
    <form onSubmit={submit} className="grid gap-4" noValidate>
      <Field label="邮箱" error={errors.email?.message}>
        <Input type="email" autoComplete="email" aria-invalid={!!errors.email} {...form.register("email")} />
      </Field>
      <div className="grid gap-1.5 text-sm">
        验证码
        <div className="flex flex-wrap items-center gap-3">
          <Controller
            control={form.control}
            name="code"
            render={({ field }) => <OtpInput value={field.value} onChange={field.onChange} invalid={!!errors.code} />}
          />
          <Button variant="outline" onClick={sendCode} disabled={sending || countdown > 0}>
            {sending ? "发送中…" : countdown > 0 ? `${countdown} 秒后重发` : "获取验证码"}
          </Button>
        </div>
        <FieldError text={errors.code?.message} />
        <p className="text-xs text-muted-foreground">验证码 5 分钟内有效，使用一次后失效。</p>
      </div>
      <FormMessage error={error} status={status} />
      <Button type="submit" disabled={isSubmitting}>{isSubmitting ? "登录中…" : "登录"}</Button>
    </form>
  )
}

const registerSchema = z.object({
  nickname: z.string().trim().min(1, "请输入昵称").max(32, "昵称不超过 32 个字符"),
  email: z.string().trim().regex(emailPattern, "请输入有效的邮箱").max(255, "邮箱过长"),
  password: z.string().refine(isPasswordValid, "密码不符合规则"),
  confirm: z.string(),
}).refine((value) => value.password === value.confirm, { path: ["confirm"], message: "两次输入的密码不一致" })

const debounceMs = 500

function Register({ onRegistered }: { onRegistered: (nickname: string) => void }) {
  const form = useForm<z.infer<typeof registerSchema>>({
    resolver: zodResolver(registerSchema),
    mode: "onTouched",
    defaultValues: { nickname: "", email: "", password: "", confirm: "" },
  })
  const [error, setError] = useState("")
  const timers = useRef<Record<string, number>>({})
  const { errors, isSubmitting } = form.formState
  const password = form.watch("password")

  // 失焦后防抖调用唯一性检查；仅在格式本身有效时请求，结果过期则丢弃。
  function checkUnique(field: "nickname" | "email") {
    window.clearTimeout(timers.current[field])
    timers.current[field] = window.setTimeout(async () => {
      if (!(await form.trigger(field))) return
      const value = form.getValues(field).trim()
      try {
        await (field === "nickname" ? users.validateNickname(value) : users.validateEmail(value))
      } catch (failure) {
        if (form.getValues(field).trim() === value) form.setError(field, { message: errorMessage(failure) })
      }
    }, debounceMs)
  }

  useEffect(() => () => Object.values(timers.current).forEach((id) => window.clearTimeout(id)), [])

  const nickname = form.register("nickname")
  const email = form.register("email")

  const submit = form.handleSubmit(async (values) => {
    setError("")
    const name = values.nickname.trim()
    const address = values.email.trim()
    const [nameCheck, emailCheck] = await Promise.allSettled([users.validateNickname(name), users.validateEmail(address)])
    if (nameCheck.status === "rejected") form.setError("nickname", { message: errorMessage(nameCheck.reason) })
    if (emailCheck.status === "rejected") form.setError("email", { message: errorMessage(emailCheck.reason) })
    if (nameCheck.status === "rejected" || emailCheck.status === "rejected") return
    try {
      await users.register(name, address, values.password)
      onRegistered(name)
    } catch (failure) {
      setError(errorMessage(failure))
    }
  })

  return (
    <form onSubmit={submit} className="grid gap-4" noValidate>
      <Field label="昵称" error={errors.nickname?.message}>
        <Input
          autoComplete="username"
          aria-invalid={!!errors.nickname}
          {...nickname}
          onBlur={(event) => { void nickname.onBlur(event); checkUnique("nickname") }}
        />
      </Field>
      <Field label="邮箱" error={errors.email?.message}>
        <Input
          type="email"
          autoComplete="email"
          aria-invalid={!!errors.email}
          {...email}
          onBlur={(event) => { void email.onBlur(event); checkUnique("email") }}
        />
      </Field>
      <Field label="密码" error={errors.password?.message}>
        <PasswordInput autoComplete="new-password" aria-invalid={!!errors.password} {...form.register("password")} />
      </Field>
      <PasswordRules value={password} />
      <Field label="确认密码" error={errors.confirm?.message}>
        <PasswordInput autoComplete="new-password" aria-invalid={!!errors.confirm} {...form.register("confirm")} />
      </Field>
      <FormMessage error={error} />
      <Button type="submit" disabled={isSubmitting}>{isSubmitting ? "注册中…" : "注册"}</Button>
    </form>
  )
}

export function AuthPage({ onLogin, notice }: Props) {
  const [mode, setMode] = useState<Mode>("password")
  const [registered, setRegistered] = useState("")
  const registering = mode === "register"

  return (
    <main className="grid min-h-dvh lg:grid-cols-2">
      <section className="flex flex-col px-6 py-8 sm:px-12">
        <Brand />
        <div className="mx-auto my-auto w-full max-w-sm py-10">
          <h1 className="text-3xl tracking-tight">{registering ? "创建账号" : "登录 Chat2Data"}</h1>
          <p className="mt-2 mb-6 text-sm text-muted-foreground">
            {registering ? "注册后即可登录工作台。" : "用自然语言查询和分析 Excel 与数据库。"}
          </p>
          {notice && !registering && <p role="alert" className="mb-4 rounded-md border px-3 py-2 text-sm">{notice}</p>}
          {registered && !registering && <p role="status" className="mb-4 text-sm text-verdant">注册成功，请使用「{registered}」登录。</p>}
          {!registering && (
            <div role="tablist" aria-label="登录方式" className="mb-5 grid grid-cols-2 gap-1 rounded-md border bg-card p-1">
              {(["password", "code"] as const).map((item) => (
                <button
                  key={item}
                  role="tab"
                  type="button"
                  aria-selected={mode === item}
                  onClick={() => setMode(item)}
                  className={cn("h-8 rounded-md text-sm transition-ui", mode === item ? "bg-secondary text-foreground" : "text-muted-foreground hover:text-foreground")}
                >
                  {item === "password" ? "密码登录" : "验证码登录"}
                </button>
              ))}
            </div>
          )}
          {mode === "password" && <PasswordLogin onLogin={onLogin} onDone={() => setRegistered("")} />}
          {mode === "code" && <CodeLogin onLogin={onLogin} />}
          {registering && <Register onRegistered={(name) => { setRegistered(name); setMode("password") }} />}
          <p className="mt-6 text-center text-sm text-muted-foreground">
            {registering ? "已有账号？" : "还没有账号？"}
            <button
              type="button"
              className="ml-1 text-ember-text underline underline-offset-4"
              onClick={() => setMode(registering ? "password" : "register")}
            >
              {registering ? "点击登录" : "点击注册"}
            </button>
          </p>
        </div>
      </section>
      <aside className="hidden items-center justify-center border-l bg-card p-12 lg:flex">
        <AuthShowcase />
      </aside>
    </main>
  )
}
