// 消息列表：助手回答平铺不加气泡，用户消息 Bone 底 + 细线。
// 自动滚到底部；用户上滑后不再强制拉回，出现“回到底部”按钮。
import { ArrowDown, Check, Copy, RefreshCw } from "lucide-react"
import { useEffect, useRef, useState } from "react"
import { isUnavailable } from "../api/client"
import type { ChatMessage } from "../hooks/useChat"
import { toast } from "../store/toast"
import { Markdown } from "./Markdown"
import { Button } from "./ui"

function CopyButton({ text }: { text: string }) {
  const [done, setDone] = useState(false)
  async function copy() {
    try {
      await navigator.clipboard.writeText(text)
      setDone(true)
      setTimeout(() => setDone(false), 1500)
    } catch {
      toast.error("复制失败，请手动选择文本复制。")
    }
  }
  return (
    <Button variant="ghost" size="sm" aria-label="复制回答" onClick={() => void copy()}>
      {done ? <Check /> : <Copy />}{done ? "已复制" : "复制"}
    </Button>
  )
}

type Props = { messages: ChatMessage[]; streaming: boolean; onRetry: () => void }

export function MessageList({ messages, streaming, onRetry }: Props) {
  const box = useRef<HTMLDivElement>(null)
  const pinned = useRef(true)
  const [showJump, setShowJump] = useState(false)

  function onScroll() {
    const el = box.current
    if (!el) return
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 48
    pinned.current = atBottom
    setShowJump(!atBottom)
  }

  function jump() {
    box.current?.scrollTo({ top: box.current.scrollHeight })
    pinned.current = true
    setShowJump(false)
  }

  useEffect(() => {
    if (pinned.current) box.current?.scrollTo({ top: box.current.scrollHeight })
  }, [messages])

  const last = messages.length - 1
  return (
    <div className="relative min-h-0 flex-1">
      <div ref={box} onScroll={onScroll} className="h-full overflow-y-auto px-4 py-6" role="log" aria-live="polite" aria-label="对话消息">
        <div className="mx-auto grid max-w-3xl gap-6">
          {messages.map((message, i) =>
            message.role === "user" ? (
              <div key={message.id} className="ml-auto max-w-[85%] whitespace-pre-wrap break-words rounded-md border bg-card px-3 py-2 text-sm">
                {message.content}
              </div>
            ) : (
              <article key={message.id} className="grid gap-2">
                {message.modelName && <p className="text-xs text-muted-foreground">{message.modelName}</p>}
                {message.content ? <Markdown>{message.content}</Markdown> : (
                  streaming && i === last && !message.error && <p className="text-sm text-muted-foreground" role="status">正在生成…</p>
                )}
                {message.incomplete && (
                  <p className="text-xs text-muted-foreground">回答未完成。前端停止接收不代表后端已停止生成。</p>
                )}
                {message.error && (
                  <p role="alert" className="text-sm text-destructive">
                    {isUnavailable(message.error) ? `后端接口暂未接通：${message.error.message.replace(/^后端接口暂未接通：/, "")}` : message.error.message}
                  </p>
                )}
                <div className="flex items-center gap-1">
                  {message.content && !(streaming && i === last) && <CopyButton text={message.content} />}
                  {i === last && message.incomplete && !streaming && (
                    <Button variant="ghost" size="sm" onClick={onRetry}><RefreshCw />重试</Button>
                  )}
                </div>
              </article>
            ),
          )}
        </div>
      </div>
      {showJump && (
        <Button variant="outline" size="sm" className="absolute bottom-3 left-1/2 -translate-x-1/2" onClick={jump}>
          <ArrowDown />回到底部
        </Button>
      )}
    </div>
  )
}
