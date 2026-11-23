// 输入框：Enter 发送，Shift+Enter 换行；输入法组合输入期间 Enter 不发送。
import { Paperclip, Send, Square } from "lucide-react"
import { type KeyboardEvent, type ReactNode, useLayoutEffect, useRef, useState } from "react"
import { Button } from "./ui"

const maxHeight = 200

type Props = {
  streaming: boolean
  // 上传或解析期间禁用发送。
  busy?: boolean
  disabled?: boolean
  placeholder?: string
  onSend: (text: string) => void
  onStop: () => void
  // 附件按钮：未提供时不显示；attachDisabledReason 非空时禁用并以 tooltip 说明。
  onAttach?: () => void
  attachDisabledReason?: string
  // 输入框下方的附加控件（模型选择器等）。
  footer?: ReactNode
}

export function Composer({ streaming, busy, disabled, placeholder, onSend, onStop, onAttach, attachDisabledReason, footer }: Props) {
  const [text, setText] = useState("")
  const area = useRef<HTMLTextAreaElement>(null)
  const canSend = !!text.trim() && !busy && !disabled && !streaming

  // 自动增高，超过上限后内部滚动。
  useLayoutEffect(() => {
    const el = area.current
    if (!el) return
    el.style.height = "auto"
    el.style.height = `${Math.min(el.scrollHeight, maxHeight)}px`
  }, [text])

  function submit() {
    if (!canSend) return
    onSend(text.trim())
    setText("")
  }

  function onKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key !== "Enter" || event.shiftKey || event.nativeEvent.isComposing) return
    event.preventDefault()
    submit()
  }

  return (
    <div className="mx-auto w-full max-w-3xl px-4 pb-4">
      <div className="rounded-md border bg-card p-2">
        <textarea
          ref={area}
          rows={1}
          value={text}
          disabled={disabled}
          placeholder={placeholder ?? "输入问题，Enter 发送，Shift+Enter 换行"}
          aria-label="输入消息"
          onChange={(event) => setText(event.target.value)}
          onKeyDown={onKeyDown}
          className="block max-h-[200px] w-full resize-none bg-transparent px-2 py-1.5 text-sm placeholder:text-muted-foreground focus-visible:outline-none disabled:opacity-50"
        />
        <div className="flex items-center gap-2">
          {onAttach && (
            <span title={attachDisabledReason}>
              <Button variant="ghost" size="icon" aria-label="上传文件" disabled={!!attachDisabledReason || busy} onClick={onAttach}>
                <Paperclip />
              </Button>
            </span>
          )}
          <div className="min-w-0 flex-1">{footer}</div>
          {streaming ? (
            <Button variant="outline" aria-label="停止生成" onClick={onStop}><Square />停止</Button>
          ) : (
            <Button size="icon" aria-label="发送消息" disabled={!canSend} onClick={submit}><Send /></Button>
          )}
        </div>
      </div>
    </div>
  )
}
