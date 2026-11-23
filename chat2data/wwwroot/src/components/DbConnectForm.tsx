// 数据库连接界面：分段控件选择 MySQL 或 SQLite。
// 密码只保存在组件内存中，不写入 localStorage 或日志。
import { zodResolver } from "@hookform/resolvers/zod"
import { useRef, useState } from "react"
import { useForm } from "react-hook-form"
import { z } from "zod"
import { ApiError, errorMessage } from "../api/client"
import { db } from "../api/db"
import type { DatabaseConfig } from "../api/db"
import { files } from "../api/files"
import { sqliteExt } from "../lib/constants"
import { cn } from "../lib/utils"
import { PasswordInput } from "./PasswordInput"
import { Button, Input } from "./ui"

export type ConnectedInfo = { connectionId: string; type: "MySQL" | "SQLite"; label: string }
type Props = { onConnected: (info: ConnectedInfo) => void }

const mysqlSchema = z.object({
  host: z.string().trim().min(1, "请输入主机地址"),
  port: z.coerce.number({ message: "请输入端口号" }).int("端口号必须是整数").min(1, "端口号范围 1-65535").max(65535, "端口号范围 1-65535"),
  name: z.string().trim().min(1, "请输入数据库名称"),
  username: z.string().trim().min(1, "请输入用户名"),
  password: z.string(),
  charset: z.string().trim().min(1, "请输入字符集"),
})
type MySqlValues = z.infer<typeof mysqlSchema>

function Field({ label, error, children }: { label: string; error?: string; children: React.ReactNode }) {
  return (
    <label className="grid gap-1.5 text-sm">
      {label}
      {children}
      {error && <span role="alert" className="text-xs text-destructive">{error}</span>}
    </label>
  )
}

function MySqlForm({ onConnected }: Props) {
  const form = useForm({
    resolver: zodResolver(mysqlSchema),
    defaultValues: { host: "", port: 3306, name: "", username: "", password: "", charset: "utf8mb4" },
  })
  const [error, setError] = useState<unknown>(null)
  const { errors, isSubmitting } = form.formState

  const submit = form.handleSubmit(async (values: MySqlValues) => {
    setError(null)
    const database: DatabaseConfig = { type: "MySQL", MySQL: { ...values, host: values.host.trim(), name: values.name.trim(), username: values.username.trim() } }
    try {
      const connectionId = await db.connect(database)
      onConnected({ connectionId, type: "MySQL", label: `${values.host.trim()}:${values.port}/${values.name.trim()}` })
    } catch (failure) {
      setError(failure)
    }
  })

  return (
    <form onSubmit={submit} className="grid gap-4" noValidate>
      <Field label="主机地址" error={errors.host?.message}><Input autoComplete="off" {...form.register("host")} /></Field>
      <div className="grid grid-cols-2 gap-3">
        <Field label="端口号" error={errors.port?.message}><Input inputMode="numeric" {...form.register("port")} /></Field>
        <Field label="字符集" error={errors.charset?.message}><Input {...form.register("charset")} /></Field>
      </div>
      <Field label="数据库名称" error={errors.name?.message}><Input autoComplete="off" {...form.register("name")} /></Field>
      <Field label="用户名" error={errors.username?.message}><Input autoComplete="off" {...form.register("username")} /></Field>
      <Field label="密码" error={errors.password?.message}><PasswordInput autoComplete="off" {...form.register("password")} /></Field>
      {error != null && <p role="alert" className="text-sm text-destructive">{errorMessage(error)}</p>}
      <Button type="submit" disabled={isSubmitting}>{isSubmitting ? "连接中…" : "连接"}</Button>
    </form>
  )
}

// SQLite 三步引导：浏览选择 .db 文件 → 上传 → 连接。
function SqliteForm({ onConnected }: Props) {
  const input = useRef<HTMLInputElement>(null)
  const [file, setFile] = useState<File>()
  const [fileId, setFileId] = useState("")
  const [percent, setPercent] = useState(0)
  const [busy, setBusy] = useState<"" | "upload" | "connect">("")
  const [error, setError] = useState<unknown>(null)

  function choose(next: File | undefined) {
    if (!next) return
    setFileId("")
    setPercent(0)
    const ext = next.name.includes(".") ? next.name.split(".").pop()!.toLowerCase() : ""
    if (ext !== sqliteExt) {
      setFile(undefined)
      setError(new ApiError("仅支持 .db 格式的 SQLite 文件。", "business"))
      return
    }
    setError(null)
    setFile(next)
  }

  async function upload() {
    if (!file) return
    setBusy("upload")
    setError(null)
    try {
      setFileId(await files.uploadSqlite(file, setPercent))
    } catch (failure) {
      setError(failure)
    } finally {
      setBusy("")
    }
  }

  async function connect() {
    if (!file || !fileId) return
    setBusy("connect")
    setError(null)
    try {
      const connectionId = await db.connect({ type: "SQLite", SQLite: { fileId, readonly: false } })
      onConnected({ connectionId, type: "SQLite", label: file.name })
    } catch (failure) {
      setError(failure)
    } finally {
      setBusy("")
    }
  }

  return (
    <ol className="grid gap-4">
      <li className="grid gap-2 text-sm">
        <span>1. 浏览：选择 .db 文件</span>
        <div className="flex items-center gap-3">
          <Button variant="outline" disabled={!!busy} onClick={() => input.current?.click()}>浏览</Button>
          <span className="min-w-0 truncate text-muted-foreground" title={file?.name}>{file?.name ?? "未选择文件"}</span>
        </div>
        <input ref={input} type="file" accept=".db" hidden onChange={(event) => { choose(event.target.files?.[0]); event.target.value = "" }} />
      </li>
      <li className="grid gap-2 text-sm">
        <span>2. 上传</span>
        <div className="flex items-center gap-3">
          <Button variant="outline" disabled={!file || !!busy || !!fileId} onClick={() => void upload()}>
            {busy === "upload" ? `上传中 ${percent}%` : fileId ? "已上传" : "上传"}
          </Button>
        </div>
      </li>
      <li className="grid gap-2 text-sm">
        <span>3. 连接</span>
        <div><Button disabled={!fileId || !!busy} onClick={() => void connect()}>{busy === "connect" ? "连接中…" : "连接"}</Button></div>
      </li>
      {error != null && <li role="alert" className="text-sm text-destructive">{errorMessage(error)}</li>}
    </ol>
  )
}

export function DbConnectForm({ onConnected }: Props) {
  const [type, setType] = useState<"MySQL" | "SQLite">("MySQL")
  return (
    <div className="mx-auto grid w-full max-w-md gap-5 p-6">
      <div>
        <h2 className="text-xl">连接数据库</h2>
        <p className="mt-1 text-sm text-muted-foreground">目前仅支持 MySQL 和 SQLite。数据库密码只在内存中使用，不会被保存。</p>
      </div>
      <div role="tablist" aria-label="数据库类型" className="grid grid-cols-2 gap-1 rounded-md border bg-card p-1">
        {(["MySQL", "SQLite"] as const).map((item) => (
          <button
            key={item}
            role="tab"
            aria-selected={type === item}
            onClick={() => setType(item)}
            className={cn("h-8 rounded-md text-sm transition-ui", type === item ? "bg-secondary" : "text-muted-foreground hover:text-foreground")}
          >
            {item}
          </button>
        ))}
      </div>
      {type === "MySQL" ? <MySqlForm onConnected={onConnected} /> : <SqliteForm onConnected={onConnected} />}
    </div>
  )
}
