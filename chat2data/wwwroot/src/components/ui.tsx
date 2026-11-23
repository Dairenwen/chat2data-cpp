// 基础组件：全部使用 4px 圆角和细线边框，不使用胶囊形。
import { cva, type VariantProps } from "class-variance-authority"
import { AlertTriangle, Inbox, RefreshCw } from "lucide-react"
import type { ComponentProps, ReactNode } from "react"
import { errorMessage, isUnavailable } from "../api/client"
import { cn } from "../lib/utils"

const buttonVariants = cva(
  "inline-flex shrink-0 items-center justify-center gap-2 rounded-md text-sm transition-ui " +
    "disabled:pointer-events-none disabled:opacity-50 [&_svg]:size-4 [&_svg]:shrink-0",
  {
    variants: {
      variant: {
        default: "bg-primary text-primary-foreground hover:bg-primary/90",
        secondary: "bg-secondary text-secondary-foreground hover:bg-secondary/70",
        outline: "border bg-card text-foreground hover:bg-accent",
        ghost: "text-foreground/60 hover:text-foreground hover:underline",
        destructive: "bg-destructive text-[#f7f7f4] hover:bg-destructive/90",
      },
      size: { default: "h-9 px-4", sm: "h-8 px-3", icon: "size-9" },
    },
    defaultVariants: { variant: "default", size: "default" },
  },
)

export function Button({ className, variant, size, type = "button", ...props }:
  ComponentProps<"button"> & VariantProps<typeof buttonVariants>) {
  return <button type={type} className={cn(buttonVariants({ variant, size }), className)} {...props} />
}

export function Input({ className, ...props }: ComponentProps<"input">) {
  return (
    <input
      className={cn(
        "h-9 w-full rounded-md border bg-card px-3 text-sm text-foreground transition-ui",
        "placeholder:text-muted-foreground disabled:cursor-not-allowed disabled:opacity-50",
        "aria-invalid:border-destructive",
        className,
      )}
      {...props}
    />
  )
}

export function Badge({ className, ...props }: ComponentProps<"span">) {
  return (
    <span
      className={cn("inline-flex items-center rounded-md border bg-secondary px-1.5 py-0.5 text-xs text-secondary-foreground", className)}
      {...props}
    />
  )
}

export function Skeleton({ className }: { className?: string }) {
  return <div aria-hidden="true" className={cn("animate-pulse rounded-md bg-secondary", className)} />
}

// 加载中以骨架行表示。
export function ListSkeleton({ rows = 5 }: { rows?: number }) {
  return (
    <div role="status" aria-label="加载中" className="grid gap-2">
      {Array.from({ length: rows }, (_, i) => <Skeleton key={i} className="h-10 w-full" />)}
    </div>
  )
}

// 空数据状态，带引导。
export function EmptyState({ title, hint, action }: { title: string; hint?: string; action?: ReactNode }) {
  return (
    <div className="flex flex-col items-center gap-2 px-4 py-12 text-center">
      <Inbox className="size-8 text-muted-foreground" strokeWidth={1.5} aria-hidden="true" />
      <p className="text-base">{title}</p>
      {hint && <p className="text-sm text-muted-foreground">{hint}</p>}
      {action}
    </div>
  )
}

// 失败状态，带重试；网关占位路由显示“后端接口暂未接通”及接口名称。
export function ErrorState({ error, onRetry }: { error: unknown; onRetry?: () => void }) {
  const unavailable = isUnavailable(error)
  return (
    <div role="alert" className="flex flex-col items-center gap-2 px-4 py-12 text-center">
      <AlertTriangle className="size-8 text-muted-foreground" strokeWidth={1.5} aria-hidden="true" />
      <p className="text-base">{unavailable ? "后端接口暂未接通" : "加载失败"}</p>
      <p className="max-w-md break-words text-sm text-muted-foreground">{errorMessage(error)}</p>
      {onRetry && <Button variant="outline" size="sm" onClick={onRetry}><RefreshCw />重试</Button>}
    </div>
  )
}
