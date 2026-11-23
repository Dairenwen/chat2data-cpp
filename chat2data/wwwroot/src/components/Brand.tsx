import { Sheet } from "lucide-react"
import { cn } from "../lib/utils"

// 品牌标识：墨色方块 + 字标，标题不加粗。
export function Brand({ className }: { className?: string }) {
  return (
    <div className={cn("flex items-center gap-2 text-lg", className)}>
      <span className="grid size-7 place-items-center rounded-md bg-primary text-primary-foreground">
        <Sheet className="size-4" strokeWidth={1.5} aria-hidden="true" />
      </span>
      Chat2Data
    </div>
  )
}
