import { cva, type VariantProps } from "class-variance-authority"
import { clsx } from "clsx"
import type { ComponentProps } from "react"
import { twMerge } from "tailwind-merge"

// shadcn/ui 的 Button 和 Input 使用项目的主题变量。
const buttonVariants = cva(
  "inline-flex shrink-0 items-center justify-center gap-2 rounded-lg text-sm font-semibold transition-colors " +
  "focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent disabled:opacity-50 " +
  "disabled:cursor-not-allowed [&_svg]:size-4",
  {
    variants: {
      variant: {
        default: "bg-accent text-white hover:bg-accent-hover",
        outline: "border border-line bg-surface text-ink hover:bg-muted",
        ghost: "text-ink hover:bg-muted",
      },
      size: { default: "h-11 px-4", icon: "size-10" },
    },
    defaultVariants: { variant: "default", size: "default" },
  },
)

export function Button({ className, variant, size, type = "button", ...props }:
  ComponentProps<"button"> & VariantProps<typeof buttonVariants>) {
  return <button type={type} className={twMerge(clsx(buttonVariants({ variant, size }), className))} {...props} />
}

export function Input({ className, ...props }: ComponentProps<"input">) {
  return (
    <input className={twMerge(clsx(
      "h-11 w-full rounded-lg border border-line bg-surface px-3 text-base text-ink",
      "placeholder:text-soft focus-visible:border-accent focus-visible:outline-1 focus-visible:outline-accent",
      className,
    ))} {...props} />
  )
}
