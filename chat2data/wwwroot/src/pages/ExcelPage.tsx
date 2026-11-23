// 智能 Excel 助手：聊天 + 文件预览 + 结果分析。
// 流程：选模型 → 新建会话（模型锁定）→ 上传 Excel 并关联会话 → 预览 → 提问。未上传文件时按普通聊天处理。
import { useQuery, useQueryClient } from "@tanstack/react-query"
import { useCallback, useEffect, useRef, useState } from "react"
import { ai, sessionTypes } from "../api/ai"
import { errorMessage } from "../api/client"
import { files } from "../api/files"
import type { ExcelPreview } from "../api/files"
import { AnalysisPanel } from "../components/AnalysisPanel"
import { AssistantWorkbench } from "../components/AssistantWorkbench"
import { Composer } from "../components/Composer"
import { MessageList } from "../components/MessageList"
import { ModelSelect } from "../components/ModelSelect"
import { SheetPreview } from "../components/SheetPreview"
import { UploadPanel } from "../components/UploadPanel"
import { Button, EmptyState, ErrorState, ListSkeleton } from "../components/ui"
import { type ChatMessage, useChat } from "../hooks/useChat"
import { useModels } from "../hooks/useModels"
import { type UploadResult, useUpload } from "../hooks/useUpload"
import { excelExt } from "../lib/constants"
import { useHashRoute } from "../lib/router"
import { formatSize } from "../lib/utils"
import { toast } from "../store/toast"

// 从“我的文件”中选择已有 Excel，关联到当前聊天会话。
function FilePicker({ onPick, onCancel }: { onPick: (fileId: string) => void; onCancel: () => void }) {
  const query = useQuery({ queryKey: ["files"], queryFn: files.list })
  const excel = (query.data ?? []).filter((item) => item.fileName.toLowerCase().endsWith(`.${excelExt}`))
  return (
    <div className="grid gap-3 p-4">
      <div className="flex items-center justify-between">
        <p className="text-base">从我的文件中选择</p>
        <Button variant="ghost" size="sm" onClick={onCancel}>取消</Button>
      </div>
      {query.isPending ? <ListSkeleton rows={4} /> : query.isError ? (
        <ErrorState error={query.error} onRetry={() => void query.refetch()} />
      ) : !excel.length ? (
        <EmptyState title="还没有 Excel 文件" hint="请先上传一个 .xlsx 文件。" />
      ) : (
        <ul className="grid gap-1">
          {excel.map((item) => (
            <li key={item.fileId}>
              <button
                onClick={() => onPick(item.fileId)}
                className="flex w-full items-center justify-between gap-3 rounded-md border px-3 py-2 text-left text-sm transition-ui hover:bg-accent"
              >
                <span className="min-w-0 truncate" title={item.fileName}>{item.fileName}</span>
                <span className="shrink-0 text-xs text-muted-foreground">{formatSize(item.fileSize)}</span>
              </button>
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}

export function ExcelPage() {
  const { query } = useHashRoute()
  const newKey = query.get("new")
  const openId = query.get("open")
  const fileParam = query.get("file")
  const queryClient = useQueryClient()
  const models = useModels()
  const chat = useChat()
  const picker = useRef<HTMLInputElement>(null)

  const [chatSessionId, setChatSessionId] = useState("")
  const [lockedName, setLockedName] = useState("")
  const [fileId, setFileId] = useState("")
  const [preview, setPreview] = useState<ExcelPreview>()
  const [tab, setTab] = useState<"data" | "analysis">("data")
  const [creating, setCreating] = useState(false)
  const [replacing, setReplacing] = useState(false)
  const [picking, setPicking] = useState(false)
  const [history, setHistory] = useState<{ loading: boolean; error: unknown }>({ loading: false, error: null })
  const [historyTry, setHistoryTry] = useState(0)

  // 上传回调需要保持稳定引用，通过 ref 读取最新的聊天会话。
  const sessionRef = useRef("")
  sessionRef.current = chatSessionId
  const onReady = useCallback(({ fileId: id, preview: data }: UploadResult) => {
    setFileId(id)
    setPreview(data)
    setReplacing(false)
    setTab("data")
    files.mapChat(id, sessionRef.current).catch((failure) => toast.error(errorMessage(failure)))
  }, [])
  const upload = useUpload(onReady)

  const sessions = useQuery({ queryKey: ["chatSessions"], queryFn: ai.sessions })
  const modelName = lockedName || sessions.data?.find((item) => item.chatSessionId === chatSessionId)?.modelName || ""

  // 新建会话入口（侧栏）：清空消息区、结果分析区和当前文件。
  useEffect(() => {
    if (!newKey) return
    chat.reset()
    upload.reset()
    setChatSessionId("")
    setLockedName("")
    setFileId("")
    setPreview(undefined)
    setReplacing(false)
    setPicking(false)
    setTab("data")
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [newKey])

  // 从我的文件打开：展示该文件数据。
  useEffect(() => {
    if (!fileParam) return
    setFileId(fileParam)
    setPreview(undefined)
  }, [fileParam])

  // 从历史会话打开：加载历史消息和关联文件。
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
      setFileId(data.fileId ?? "")
      setPreview(undefined)
      setHistory({ loading: false, error: null })
    }).catch((failure) => {
      if (!cancelled) setHistory({ loading: false, error: failure })
    })
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [openId, historyTry])

  async function createSession() {
    if (!models.modelId) return
    setCreating(true)
    try {
      const created = await ai.createSession(models.modelId, sessionTypes.excel)
      setChatSessionId(created.chatSessionId)
      setLockedName(created.modelName)
      void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
      if (fileId) await files.mapChat(fileId, created.chatSessionId)
    } catch (failure) {
      toast.error(errorMessage(failure))
    } finally {
      setCreating(false)
    }
  }

  async function pickExisting(id: string) {
    try {
      await files.mapChat(id, chatSessionId)
      setFileId(id)
      setPreview(undefined)
      setPicking(false)
      setReplacing(false)
    } catch (failure) {
      toast.error(errorMessage(failure))
    }
  }

  function send(text: string) {
    // 未上传文件时不带 fileId，按普通模型聊天处理。
    chat.send(text, { chatSessionId, fileId: fileId || undefined }, modelName)
    void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
  }

  const noSession = chatSessionId ? undefined : "请先新建会话"
  const overview = useQuery({
    queryKey: ["preview", fileId, "overview"],
    queryFn: () => files.preview(fileId),
    enabled: !!fileId,
    initialData: preview?.fileId === fileId ? preview : undefined,
  })
  const info = overview.data

  const showUpload = !fileId || replacing || upload.active || upload.stage === "error"
  const data = picking ? (
    <FilePicker onPick={(id) => void pickExisting(id)} onCancel={() => setPicking(false)} />
  ) : showUpload ? (
    <div>
      <UploadPanel
        disabledReason={noSession}
        stage={upload.stage}
        percent={upload.percent}
        active={upload.active}
        error={upload.error}
        accept=".xlsx"
        hint="仅支持 .xlsx，不超过 10 MB"
        onFile={(file) => void upload.start(file)}
        onCancel={upload.cancel}
        onRetry={upload.retry}
        onPickExisting={() => setPicking(true)}
      />
      {replacing && !upload.active && (
        <div className="px-4 pb-4"><Button variant="ghost" size="sm" onClick={() => { setReplacing(false); upload.reset() }}>取消更换</Button></div>
      )}
    </div>
  ) : (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 items-center gap-3 border-b px-3 py-2 text-sm">
        <span className="min-w-0 flex-1 truncate" title={info?.fileName}>{info?.fileName ?? "当前文件"}</span>
        {info && (
          <span className="shrink-0 text-xs text-muted-foreground">
            {formatSize(info.fileSize)} · {info.excelData.sheets.length} 个工作表
          </span>
        )}
        <Button variant="outline" size="sm" disabled={!chatSessionId} title={noSession} onClick={() => setReplacing(true)}>更换文件</Button>
      </div>
      <div className="min-h-0 flex-1"><SheetPreview fileId={fileId} initial={info} /></div>
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
          <p className="text-2xl">智能 Excel 助手</p>
          <p className="text-sm text-muted-foreground">先选择模型并新建会话，再上传 Excel 提问；也可以直接和模型聊天。</p>
          <Button disabled={creating || !models.modelId} onClick={() => void createSession()}>
            {creating ? "创建中…" : "新建会话"}
          </Button>
        </div>
      ) : (
        <>
          {!chat.messages.length && (
            <p className="px-4 pt-4 text-center text-sm text-muted-foreground">会话已创建。上传 Excel 后针对文件分析，不上传则按普通聊天处理。</p>
          )}
          <MessageList messages={chat.messages} streaming={chat.streaming} onRetry={chat.retry} />
        </>
      )}
      <Composer
        streaming={chat.streaming}
        busy={upload.active}
        disabled={!chatSessionId}
        placeholder={chatSessionId ? undefined : "请先新建会话"}
        onSend={send}
        onStop={chat.stop}
        onAttach={() => picker.current?.click()}
        attachDisabledReason={noSession}
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
      <input
        ref={picker}
        type="file"
        accept=".xlsx"
        hidden
        onChange={(event) => {
          const file = event.target.files?.[0]
          event.target.value = ""
          if (!file) return
          setReplacing(true)
          setTab("data")
          void upload.start(file)
        }}
      />
    </div>
  )

  return (
    <AssistantWorkbench
      title="智能 Excel 助手"
      chat={chatArea}
      data={data}
      analysis={<AnalysisPanel />}
      tab={tab}
      onTabChange={setTab}
    />
  )
}
