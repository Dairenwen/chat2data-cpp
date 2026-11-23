// 数据区的上传面板：大拖拽区 + 整体状态（校验 → 上传信息 → 上传文件 → 服务端解析 → 预览就绪）。
// 未创建聊天会话前上传入口禁用，并以 tooltip 说明。
import { FolderOpen, Upload, X } from "lucide-react"
import { type DragEvent, useRef, useState } from "react"
import { ErrorState } from "./ui"
import { type UploadStage, stageText } from "../hooks/useUpload"
import { cn } from "../lib/utils"
import { Button } from "./ui"

type Props = {
  disabledReason?: string
  stage: UploadStage
  percent: number
  active: boolean
  error: unknown
  accept: string
  hint: string
  onFile: (file: File) => void
  onCancel: () => void
  onRetry: () => void
  // 从“我的文件”中选择已有文件（仅 Excel 场景提供）。
  onPickExisting?: () => void
}

const order: UploadStage[] = ["validating", "info", "uploading", "parsing", "ready"]

export function UploadPanel({ disabledReason, stage, percent, active, error, accept, hint, onFile, onCancel, onRetry, onPickExisting }: Props) {
  const input = useRef<HTMLInputElement>(null)
  const [over, setOver] = useState(false)
  const disabled = !!disabledReason || active

  function onDrop(event: DragEvent) {
    event.preventDefault()
    setOver(false)
    const file = event.dataTransfer.files[0]
    if (file && !disabled) onFile(file)
  }

  return (
    <div className="grid gap-4 p-4">
      <div
        onDragOver={(event) => { event.preventDefault(); if (!disabled) setOver(true) }}
        onDragLeave={() => setOver(false)}
        onDrop={onDrop}
        title={disabledReason}
        className={cn(
          "grid justify-items-center gap-3 rounded-md border border-dashed px-6 py-12 text-center transition-ui",
          over && "bg-accent",
          disabled && "opacity-60",
        )}
      >
        <Upload className="size-8 text-muted-foreground" strokeWidth={1.5} aria-hidden="true" />
        <p className="text-base">拖拽文件到此处，或点击选择</p>
        <p className="text-sm text-muted-foreground">{disabledReason ?? hint}</p>
        <input
          ref={input}
          type="file"
          accept={accept}
          hidden
          onChange={(event) => {
            const file = event.target.files?.[0]
            event.target.value = ""
            if (file) onFile(file)
          }}
        />
        <div className="flex flex-wrap justify-center gap-2">
          <span title={disabledReason}>
            <Button variant="outline" disabled={disabled} onClick={() => input.current?.click()}>选择文件</Button>
          </span>
          {onPickExisting && (
            <span title={disabledReason}>
              <Button variant="ghost" disabled={!!disabledReason || active} onClick={onPickExisting}><FolderOpen />从我的文件中选择</Button>
            </span>
          )}
        </div>
      </div>

      {stage !== "idle" && (
        <div className="grid gap-2 rounded-md border bg-card p-4" role="status" aria-live="polite">
          <ol className="flex flex-wrap gap-x-3 gap-y-1 text-xs">
            {order.map((item) => {
              const reached = order.indexOf(stage) >= order.indexOf(item)
              return (
                <li key={item} className={reached && stage !== "error" ? "text-foreground" : "text-muted-foreground"}>
                  {stageText[item]}
                </li>
              )
            })}
          </ol>
          {stage === "uploading" && (
            <div className="flex items-center gap-2">
              <progress className="h-1.5 flex-1" max={100} value={percent} aria-label="上传进度" />
              <span className="w-10 text-right text-xs tabular-nums">{percent}%</span>
            </div>
          )}
          {active && (
            <div><Button variant="ghost" size="sm" onClick={onCancel}><X />取消上传</Button></div>
          )}
        </div>
      )}
      {stage === "error" && <ErrorState error={error} onRetry={onRetry} />}
    </div>
  )
}
