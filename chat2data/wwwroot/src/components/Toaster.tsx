import { X } from "lucide-react"
import { cn } from "../lib/utils"
import { useToast } from "../store/toast"

export function Toaster() {
  const { items, dismiss } = useToast()
  return (
    <div className="fixed right-4 bottom-4 z-[100] grid w-[min(360px,calc(100vw-2rem))] gap-2" role="region" aria-label="通知">
      {items.map((item) => (
        <div
          key={item.id}
          role={item.kind === "error" ? "alert" : "status"}
          className={cn("flex items-start gap-2 rounded-md border bg-popover p-3 text-sm shadow-float", item.kind === "error" && "border-destructive")}
        >
          <span className="min-w-0 flex-1 break-words">{item.text}</span>
          <button aria-label="关闭通知" className="text-muted-foreground hover:text-foreground" onClick={() => dismiss(item.id)}>
            <X className="size-4" />
          </button>
        </div>
      ))}
    </div>
  )
}
