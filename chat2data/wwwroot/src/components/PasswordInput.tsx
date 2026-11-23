// 带显示 / 隐藏切换的密码框，可直接配合 react-hook-form 的 register 使用。
import { Eye, EyeOff } from "lucide-react"
import { type ComponentProps, useState } from "react"
import { Input } from "./ui"

export function PasswordInput(props: Omit<ComponentProps<"input">, "type">) {
  const [visible, setVisible] = useState(false)
  const label = visible ? "隐藏密码" : "显示密码"
  return (
    <div className="relative">
      <Input {...props} type={visible ? "text" : "password"} className="pr-10" />
      <button
        type="button"
        aria-label={label}
        title={label}
        aria-pressed={visible}
        onClick={() => setVisible(!visible)}
        className="absolute inset-y-0 right-0 grid w-10 place-items-center text-muted-foreground hover:text-foreground"
      >
        {visible ? <EyeOff className="size-4" strokeWidth={1.5} /> : <Eye className="size-4" strokeWidth={1.5} />}
      </button>
    </div>
  )
}
