// 工作台布局：聊天 | 可折叠数据区（文件预览 / 结果分析）。移动端使用底部三标签“对话 / 数据 / 分析”。
import { BarChart3, MessageSquare, PanelRightClose, PanelRightOpen, Table2 } from "lucide-react"
import { type ReactNode, useState } from "react"
import { cn } from "../lib/utils"
import { Button } from "./ui"

type Tab = "data" | "analysis"
type MobileTab = "chat" | Tab

type Props = {
  title: ReactNode
  chat: ReactNode
  data: ReactNode
  analysis: ReactNode
  dataLabel?: string
  // 数据区顶部附加控件（表切换、类型标识等）。
  dataToolbar?: ReactNode
  // 外部请求切换到数据标签（例如上传成功后）。
  tab: Tab
  onTabChange: (tab: Tab) => void
}

const tabs: { value: Tab; label: string; icon: typeof Table2 }[] = [
  { value: "data", label: "文件预览", icon: Table2 },
  { value: "analysis", label: "结果分析", icon: BarChart3 },
]

export function AssistantWorkbench({ title, chat, data, analysis, dataLabel = "文件预览", dataToolbar, tab, onTabChange }: Props) {
  const [collapsed, setCollapsed] = useState(false)
  const [mobile, setMobile] = useState<MobileTab>("chat")

  const panel = (
    <>
      <div className="flex h-[52px] shrink-0 items-center gap-2 border-b px-3">
        <div role="tablist" aria-label="数据区" className="hidden gap-1 md:flex">
          {tabs.map(({ value, label, icon: Icon }) => (
            <button
              key={value}
              role="tab"
              aria-selected={tab === value}
              onClick={() => onTabChange(value)}
              className={cn("flex h-8 items-center gap-1.5 rounded-md px-3 text-sm transition-ui", tab === value ? "bg-secondary" : "text-muted-foreground hover:text-foreground")}
            >
              <Icon className="size-4" strokeWidth={1.5} />{value === "data" ? dataLabel : label}
            </button>
          ))}
        </div>
        <div className="min-w-0 flex-1">{tab === "data" && dataToolbar}</div>
        <Button variant="ghost" size="icon" aria-label="收起数据区" className="hidden md:inline-flex" onClick={() => setCollapsed(true)}>
          <PanelRightClose />
        </Button>
      </div>
      <div className="min-h-0 flex-1 overflow-auto">{tab === "data" ? data : analysis}</div>
    </>
  )

  return (
    <div className="flex h-full min-h-0 flex-col md:flex-row">
      {/* 桌面端：聊天 */}
      <section aria-label="对话" className={cn("min-h-0 min-w-0 flex-1 flex-col", mobile === "chat" ? "flex" : "hidden md:flex")}>
        <header className="flex h-[52px] shrink-0 items-center justify-between gap-2 px-4">
          <h1 className="truncate text-base">{title}</h1>
          {collapsed && (
            <Button variant="ghost" size="icon" aria-label="展开数据区" className="hidden md:inline-flex" onClick={() => setCollapsed(false)}>
              <PanelRightOpen />
            </Button>
          )}
        </header>
        {chat}
      </section>

      {/* 数据区：桌面端占 40–50% 宽度，可折叠；移动端按标签切换 */}
      {!collapsed && (
        <aside
          aria-label="数据区"
          className={cn("min-h-0 flex-1 flex-col border-l md:w-[45%] md:max-w-[50%] md:flex-none", mobile === "chat" ? "hidden md:flex" : "flex")}
        >
          {panel}
        </aside>
      )}

      <nav aria-label="移动端标签" className="grid shrink-0 grid-cols-3 border-t md:hidden">
        {([
          { value: "chat", label: "对话", icon: MessageSquare },
          { value: "data", label: "数据", icon: Table2 },
          { value: "analysis", label: "分析", icon: BarChart3 },
        ] as const).map(({ value, label, icon: Icon }) => (
          <button
            key={value}
            aria-current={mobile === value ? "page" : undefined}
            onClick={() => { setMobile(value); if (value !== "chat") onTabChange(value) }}
            className={cn("flex h-12 flex-col items-center justify-center gap-0.5 text-xs transition-ui", mobile === value ? "bg-secondary" : "text-muted-foreground")}
          >
            <Icon className="size-4" strokeWidth={1.5} />{label}
          </button>
        ))}
      </nav>
    </div>
  )
}
