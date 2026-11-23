// 智能 DB 助手：连接 → 表预览（原表 / 备份表）→ 聊天分析。聊天和结果分析区与 Excel 助手共用组件。
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useEffect, useRef, useState } from "react"
import { ai, sessionTypes } from "../api/ai"
import { errorMessage } from "../api/client"
import { db } from "../api/db"
import { AnalysisPanel } from "../components/AnalysisPanel"
import { AssistantWorkbench } from "../components/AssistantWorkbench"
import { Composer } from "../components/Composer"
import { DbConnectForm } from "../components/DbConnectForm"
import { DbConnectionCard } from "../components/DbConnectionCard"
import { DataTable } from "../components/DataTable"
import { MessageList } from "../components/MessageList"
import { ModelSelect } from "../components/ModelSelect"
import { Badge, Button, EmptyState, ErrorState, ListSkeleton } from "../components/ui"
import { type ChatMessage, useChat } from "../hooks/useChat"
import { useModels } from "../hooks/useModels"
import { useHashRoute } from "../lib/router"
import { cn } from "../lib/utils"
import { useDb } from "../store/db"
import { toast } from "../store/toast"

export function DbPage() {
  const { query } = useHashRoute()
  const newKey = query.get("new")
  const openId = query.get("open")
  const fileParam = query.get("file")
  const fileName = query.get("name") ?? "SQLite 文件"
  const queryClient = useQueryClient()
  const { connection, setConnection } = useDb()
  const models = useModels()
  const chat = useChat()

  // 已有连接时再次进入先显示连接信息卡片，选择“使用此连接”后进入交互页。
  const [entered, setEntered] = useState(false)
  const [disconnecting, setDisconnecting] = useState(false)
  const [connecting, setConnecting] = useState<unknown>(null)
  const [chatSessionId, setChatSessionId] = useState("")
  const [lockedName, setLockedName] = useState("")
  const [creating, setCreating] = useState(false)
  const [table, setTable] = useState("")
  const [view, setView] = useState<"backup" | "original">("backup")
  const [tab, setTab] = useState<"data" | "analysis">("data")
  const [history, setHistory] = useState<{ loading: boolean; error: unknown }>({ loading: false, error: null })
  const [historyTry, setHistoryTry] = useState(0)
  const connectionId = connection?.connectionId ?? ""

  const sessions = useQuery({ queryKey: ["chatSessions"], queryFn: ai.sessions })
  const modelName = lockedName || sessions.data?.find((item) => item.chatSessionId === chatSessionId)?.modelName || ""

  // 从“我的文件”打开 SQLite：直接建立对应连接。
  useEffect(() => {
    if (!fileParam) return
    let cancelled = false
    setConnecting("pending")
    db.connect({ type: "SQLite", SQLite: { fileId: fileParam, readonly: false } }).then((id) => {
      if (cancelled) return
      setConnection({ connectionId: id, type: "SQLite", label: fileName })
      setEntered(true)
      setConnecting(null)
    }).catch((failure) => { if (!cancelled) setConnecting(failure) })
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fileParam])

  // 新建会话入口：清空消息区，保留当前连接。
  useEffect(() => {
    if (!newKey) return
    chat.reset()
    setChatSessionId("")
    setLockedName("")
    setTab("data")
    if (useDb.getState().connection) setEntered(true)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [newKey])

  // 从历史会话打开：加载历史消息；数据库连接需在当前连接下继续使用。
  useEffect(() => {
    if (!openId) return
    let cancelled = false
    setHistory({ loading: true, error: null })
    ai.history(openId).then((data) => {
      if (cancelled) return
      const list: ChatMessage[] = (data.messageList ?? [])
        .map((item) => ({ ...item, role: item.role.toLowerCase() }))
        .filter((item) => item.role === "user" || item.role === "assistant")
        .map((item) => ({ id: item.id, role: item.role as "user" | "assistant", content: item.content }))
      chat.reset(list)
      setChatSessionId(openId)
      setLockedName("")
      setHistory({ loading: false, error: null })
      if (useDb.getState().connection) setEntered(true)
    }).catch((failure) => { if (!cancelled) setHistory({ loading: false, error: failure }) })
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [openId, historyTry])

  const tables = useQuery({
    queryKey: ["dbTables", connectionId],
    queryFn: () => db.tables(connectionId),
    enabled: !!connectionId && entered,
  })
  const current = table && tables.data?.includes(table) ? table : tables.data?.[0] ?? ""

  const status = useQuery({
    queryKey: ["dbStatus", connectionId],
    queryFn: () => db.connectionStatus(connectionId),
    enabled: !!connectionId && entered,
  })
  const modified = !!status.data?.hasModifications && !!status.data.tempTables?.includes(current)

  const data = useQuery({
    queryKey: ["dbTable", connectionId, current, view === "original"],
    queryFn: () => db.tableData(connectionId, current, view === "original"),
    enabled: !!connectionId && !!current,
  })

  // 一轮回答结束后刷新备份表状态和表数据（修改类 SQL 只作用于备份表）。
  const wasStreaming = useRef(false)
  useEffect(() => {
    if (wasStreaming.current && !chat.streaming && connectionId) {
      void status.refetch()
      void data.refetch()
    }
    wasStreaming.current = chat.streaming
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [chat.streaming])

  async function disconnect() {
    setDisconnecting(true)
    try {
      await db.disconnect(connectionId)
      setConnection(null)
      setEntered(false)
      chat.reset()
      setChatSessionId("")
      setLockedName("")
      queryClient.removeQueries({ queryKey: ["dbTable"] })
    } catch (failure) {
      toast.error(errorMessage(failure))
    } finally {
      setDisconnecting(false)
    }
  }

  async function createSession() {
    if (!models.modelId) return
    setCreating(true)
    try {
      const created = await ai.createSession(models.modelId, sessionTypes.db)
      setChatSessionId(created.chatSessionId)
      setLockedName(created.modelName)
      void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
    } catch (failure) {
      toast.error(errorMessage(failure))
    } finally {
      setCreating(false)
    }
  }

  function send(text: string) {
    chat.send(text, { chatSessionId, connectionId, tableName: current || undefined }, modelName)
    void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
  }

  if (connecting === "pending") {
    return <p role="status" className="p-6 text-sm text-muted-foreground">正在建立数据库连接…</p>
  }
  if (connecting) {
    return <ErrorState error={connecting} onRetry={() => { setConnecting(null); setEntered(false) }} />
  }
  if (!connection) {
    return (
      <div className="h-full overflow-y-auto">
        <DbConnectForm onConnected={(info) => { setConnection(info); setEntered(true) }} />
      </div>
    )
  }
  if (!entered) {
    return (
      <div className="h-full overflow-y-auto">
        <DbConnectionCard info={connection} disconnecting={disconnecting} onUse={() => setEntered(true)} onDisconnect={() => void disconnect()} />
      </div>
    )
  }

  const toolbar = (
    <div className="flex min-w-0 flex-wrap items-center gap-2">
      <Badge>{connection.type}</Badge>
      <select
        aria-label="切换数据表"
        value={current}
        disabled={!tables.data?.length}
        onChange={(event) => setTable(event.target.value)}
        className="h-8 max-w-40 rounded-md border bg-card px-2 text-sm"
      >
        {(tables.data ?? []).map((name) => <option key={name} value={name}>{name}</option>)}
      </select>
      {modified && (
        <div role="group" aria-label="数据来源" className="flex gap-1 rounded-md border bg-card p-0.5">
          {([["original", "原表"], ["backup", "备份表"]] as const).map(([value, label]) => (
            <button
              key={value}
              aria-pressed={view === value}
              onClick={() => setView(value)}
              className={cn("h-7 rounded-md px-2 text-xs transition-ui", view === value ? "bg-secondary" : "text-muted-foreground")}
            >
              {label}
            </button>
          ))}
        </div>
      )}
      <Button variant="ghost" size="sm" disabled={disconnecting} onClick={() => void disconnect()}>断开连接</Button>
    </div>
  )

  const preview = (
    <div className="flex h-full min-h-0 flex-col">
      {modified && view === "backup" && (
        <p className="shrink-0 border-b px-3 py-2 text-xs text-muted-foreground">修改只作用于备份表，不影响原表。</p>
      )}
      <div className="min-h-0 flex-1 overflow-auto p-3">
        {tables.isPending ? <ListSkeleton /> : tables.isError ? (
          <ErrorState error={tables.error} onRetry={() => void tables.refetch()} />
        ) : !tables.data.length ? (
          <EmptyState title="数据库中没有数据表" />
        ) : data.isPending ? <ListSkeleton /> : data.isError ? (
          <ErrorState error={data.error} onRetry={() => void data.refetch()} />
        ) : (
          <DataTable
            columns={data.data.tableSchema.columnInfo.map((column) => column.name)}
            rows={data.data.tableData.rows.map((row) => row.cells)}
          />
        )}
      </div>
    </div>
  )

  const chatArea = (
    <div className="flex min-h-0 flex-1 flex-col">
      {history.loading ? (
        <div className="p-4"><ListSkeleton rows={4} /></div>
      ) : history.error ? (
        <ErrorState error={history.error} onRetry={() => setHistoryTry(historyTry + 1)} />
      ) : !chatSessionId ? (
        <div className="grid flex-1 place-content-center justify-items-center gap-3 p-6 text-center">
          <p className="text-2xl">智能 DB 助手</p>
          <p className="text-sm text-muted-foreground">先选择模型并新建会话，再用自然语言查询和分析数据库。</p>
          <Button disabled={creating || !models.modelId} onClick={() => void createSession()}>
            {creating ? "创建中…" : "新建会话"}
          </Button>
        </div>
      ) : (
        <MessageList messages={chat.messages} streaming={chat.streaming} onRetry={chat.retry} />
      )}
      <Composer
        streaming={chat.streaming}
        disabled={!chatSessionId}
        placeholder={chatSessionId ? undefined : "请先新建会话"}
        onSend={send}
        onStop={chat.stop}
        footer={
          <ModelSelect
            models={models.models}
            loading={models.loading}
            error={models.error}
            value={models.modelId}
            onChange={models.setModelId}
            lockedName={chatSessionId ? modelName || "已锁定" : undefined}
          />
        }
      />
    </div>
  )

  return (
    <AssistantWorkbench
      title="智能 DB 助手"
      chat={chatArea}
      data={preview}
      dataLabel="表预览"
      dataToolbar={toolbar}
      analysis={<AnalysisPanel />}
      tab={tab}
      onTabChange={setTab}
    />
  )
}
