// 密码规则：至少 8 位；数字、小写字母、大写字母、特殊符号至少两类（接口设计 1.2.3）。
import { Check, Circle } from "lucide-react"
import { cn } from "../lib/utils"

function kindCount(value: string) {
  return [/[0-9]/, /[a-z]/, /[A-Z]/, /[^0-9a-zA-Z]/].filter((pattern) => pattern.test(value)).length
}

export const passwordRules = [
  { id: "length", label: "至少 8 位", test: (value: string) => Array.from(value).length >= 8 },
  { id: "kinds", label: "数字、小写字母、大写字母、特殊符号至少包含两类", test: (value: string) => kindCount(value) >= 2 },
]

export const isPasswordValid = (value: string) => passwordRules.every((rule) => rule.test(value))

// 实时打勾的规则清单；状态同时用图标和文字表达，不只靠颜色。
export function PasswordRules({ value }: { value: string }) {
  return (
    <ul className="grid gap-1 text-xs" aria-label="密码规则">
      {passwordRules.map((rule) => {
        const passed = rule.test(value)
        return (
          <li key={rule.id} className={cn("flex items-center gap-1.5", passed ? "text-foreground" : "text-muted-foreground")}>
            {passed ? <Check className="size-3.5 text-verdant" aria-hidden="true" /> : <Circle className="size-3.5" aria-hidden="true" />}
            {rule.label}
            <span className="sr-only">{passed ? "已满足" : "未满足"}</span>
          </li>
        )
      })}
    </ul>
  )
}
