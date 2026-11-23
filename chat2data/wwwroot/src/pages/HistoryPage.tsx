// 历史会话：按今天、昨天、近 7 天、更早分组，带搜索；悬停显示删除，删除需二次确认。
// 会话列表接口不返回会话类型，点击时先读取历史消息，按 sessionType 决定打开 Excel 还是 DB 页。
import * as Dialog from "@radix-ui/react-dialog"
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query"
import { Search, Trash2 } from "lucide-react"
import { useEffect, useMemo, useState } from "react"
import { ai, sessionTypes } from "../api/ai"
import type { ChatSession } from "../api/ai"
import { errorMessage } from "../api/client"
import { Badge, Button, EmptyState, ErrorState, Input, ListSkeleton } from "../components/ui"
import { type Route, navigate, useHashRoute } from "../lib/router"
import { dateGroup, dateGroups, formatDateTime } from "../lib/utils"
import { toast } from "../store/toast"

const typeLabel: Record<string, string> = { excel: "Excel", db: "DB" }

export function HistoryPage() {
  const { query } = useHashRoute()
  const openId = query.get("open")
  const queryClient = useQueryClient()
  const [search, setSearch] = useState("")
  const [removing, setRemoving] = useState<ChatSession | null>(null)
  const [opening, setOpening] = useState("")
  // 已读取到的会话类型，用于徽标展示。
  const [types, setTypes] = useState<Record<string, string>>({})

  const list = useQuery({ queryKey: ["chatSessions"], queryFn: ai.sessions })

  const remove = useMutation({
    mutationFn: (chatSessionId: string) => ai.deleteSession(chatSessionId),
    onSuccess: () => {
      toast.success("会话已删除。")
      setRemoving(null)
      void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
    },
    onError: (failure) => toast.error(errorMessage(failure)),
  })

  async function open(chatSessionId: string) {
    setOpening(chatSessionId)
    try {
      const data = await ai.history(chatSessionId)
      const type = (data.sessionType ?? "").toLowerCase()
      setTypes((current) => ({ ...current, [chatSessionId]: type }))
      const target: Route = type === sessionTypes.db ? "db" : "excel"
      navigate(target, { open: chatSessionId })
    } catch (failure) {
      toast.error(errorMessage(failure))
    } finally {
      setOpening("")
    }
  }

  // 从侧栏“最近会话”进入：直接打开该会话。
  useEffect(() => {
    if (openId) void open(openId)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [openId])

  const groups = useMemo(() => {
    const keyword = search.trim().toLowerCase()
    const sorted = [...(list.data ?? [])]
      .filter((item) => !keyword || `${item.title} ${item.firstUserMessageContent}`.toLowerCase().includes(keyword))
      .sort((a, b) => b.updatedAt - a.updatedAt)
    return dateGroups
      .map((name) => ({ name, items: sorted.filter((item) => dateGroup(item.updatedAt) === name) }))
      .filter((group) => group.items.length)
  }, [list.data, search])

  return (
    <div className="h-full overflow-y-auto">
      <div className="mx-auto grid max-w-3xl gap-4 p-4 sm:p-6">
        <h1 className="text-2xl">历史会话</h1>
        <div className="relative">
          <Search className="pointer-events-none absolute top-2.5 left-2.5 size-4 text-muted-foreground" aria-hidden="true" />
          <Input aria-label="搜索会话" placeholder="搜索会话标题" className="pl-8" value={search} onChange={(event) => setSearch(event.target.value)} />
        </div>

        {list.isPending ? <ListSkeleton /> : list.isError ? (
          <ErrorState error={list.error} onRetry={() => void list.refetch()} />
        ) : !list.data.length ? (
          <EmptyState title="还没有历史会话" hint="在智能 Excel 助手或智能 DB 助手中新建会话后会显示在这里。" />
        ) : !groups.length ? (
          <EmptyState title="没有匹配的会话" hint="换一个关键词试试。" />
        ) : groups.map((group) => (
          <section key={group.name} aria-label={group.name} className="grid gap-1">
            <h2 className="px-1 text-xs text-muted-foreground">{group.name}</h2>
            {group.items.map((item) => {
              // 会话标题默认取第一条消息的前 20 个字。
              const title = (item.title || item.firstUserMessageContent || "新会话").slice(0, 20)
              const type = types[item.chatSessionId]
              return (
                <div key={item.chatSessionId} className="group relative rounded-md border transition-ui hover:bg-card">
                  <button
                    disabled={opening === item.chatSessionId}
                    onClick={() => void open(item.chatSessionId)}
                    className="grid w-full gap-1 px-3 py-2 pr-12 text-left"
                  >
                    <span className="flex items-center gap-2">
                      <span className="min-w-0 truncate text-sm" title={title}>{title}</span>
                      {type && typeLabel[type] && <Badge>{typeLabel[type]}</Badge>}
                    </span>
                    <span className="flex flex-wrap gap-x-3 text-xs text-muted-foreground">
                      <span>{formatDateTime(item.updatedAt)}</span>
                      <span>{item.messageCount} 条消息</span>
                      {item.modelName && <span>{item.modelName}</span>}
                    </span>
                  </button>
                  <Button
                    variant="ghost"
                    size="icon"
                    aria-label={`删除会话 ${title}`}
                    title="删除"
                    className="absolute top-1/2 right-1 -translate-y-1/2 opacity-0 focus-visible:opacity-100 group-hover:opacity-100"
                    onClick={() => setRemoving(item)}
                  >
                    <Trash2 />
                  </Button>
                </div>
              )
            })}
          </section>
        ))}
      </div>

      <Dialog.Root open={!!removing} onOpenChange={(value) => { if (!value && !remove.isPending) setRemoving(null) }}>
        <Dialog.Portal>
          <Dialog.Overlay className="fixed inset-0 z-50 bg-foreground/30" />
          <Dialog.Content className="fixed top-1/2 left-1/2 z-50 grid w-[min(420px,calc(100vw-2rem))] -translate-x-1/2 -translate-y-1/2 gap-4 rounded-dialog border bg-popover p-6 shadow-float">
            <Dialog.Title className="text-lg">删除会话</Dialog.Title>
            <Dialog.Description className="text-sm text-muted-foreground">确定删除这个会话吗？删除后无法恢复。</Dialog.Description>
            <div className="flex justify-end gap-2">
              <Button variant="outline" disabled={remove.isPending} onClick={() => setRemoving(null)}>取消</Button>
              <Button variant="destructive" disabled={remove.isPending} onClick={() => removing && remove.mutate(removing.chatSessionId)}>
                {remove.isPending ? "删除中…" : "确认删除"}
              </Button>
            </div>
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>
    </div>
  )
}
