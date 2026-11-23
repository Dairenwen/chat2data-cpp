// 模型选择器：会话创建前可选，创建后锁定并仅展示所用模型。
import * as Select from "@radix-ui/react-select"
import { Check, ChevronDown, Lock } from "lucide-react"
import { errorMessage } from "../api/client"
import type { ModelItem } from "../api/ai"

type Props = {
  models: ModelItem[] | undefined
  loading: boolean
  error: unknown
  value: string
  onChange: (modelConfigId: string) => void
  // 会话已创建：锁定，展示会话所用的模型名称。
  lockedName?: string
}

export function ModelSelect({ models, loading, error, value, onChange, lockedName }: Props) {
  if (lockedName) {
    return (
      <span className="inline-flex items-center gap-1.5 text-xs text-muted-foreground" title="会话创建后模型已锁定">
        <Lock className="size-3.5" strokeWidth={1.5} aria-hidden="true" />{lockedName}
      </span>
    )
  }
  if (loading) return <span className="text-xs text-muted-foreground" role="status">模型加载中…</span>
  if (error || !models?.length) {
    return (
      <span className="text-xs text-muted-foreground" role="alert" title={error ? errorMessage(error) : undefined}>
        模型不可用：{error ? errorMessage(error) : "服务端未返回可用模型"}
      </span>
    )
  }
  return (
    <Select.Root value={value} onValueChange={onChange}>
      <Select.Trigger
        aria-label="选择模型"
        className="inline-flex h-8 max-w-full items-center gap-1.5 rounded-md border px-2 text-xs transition-ui hover:bg-accent"
      >
        <Select.Value placeholder="选择模型" />
        <Select.Icon><ChevronDown className="size-3.5" aria-hidden="true" /></Select.Icon>
      </Select.Trigger>
      <Select.Portal>
        <Select.Content position="popper" side="top" sideOffset={4} className="z-50 min-w-48 rounded-md border bg-popover p-1 shadow-float">
          <Select.Viewport>
            {models.map((model) => (
              <Select.Item
                key={model.modelConfigId}
                value={model.modelConfigId}
                className="flex cursor-pointer items-start gap-2 rounded-md px-2 py-1.5 text-sm outline-none data-[highlighted]:bg-accent"
              >
                <span className="min-w-0 flex-1">
                  <Select.ItemText>{model.modelName}</Select.ItemText>
                  {model.modelDesc && <span className="block text-xs text-muted-foreground">{model.modelDesc}</span>}
                </span>
                <Select.ItemIndicator><Check className="size-4" /></Select.ItemIndicator>
              </Select.Item>
            ))}
          </Select.Viewport>
        </Select.Content>
      </Select.Portal>
    </Select.Root>
  )
}
