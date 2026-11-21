import { Moon, Sheet, Sun } from "lucide-react"
import { Button } from "./ui"

export function Brand() {
  return <div className="brand"><span className="brand-mark"><Sheet size={19} /></span>Chat2Data</div>
}

export function ThemeButton({ dark, toggle }: { dark: boolean; toggle: () => void }) {
  const label = dark ? "切换到浅色主题" : "切换到深色主题"
  return (
    <Button variant="ghost" size="icon" aria-label={label} title={label} onClick={toggle}>
      {dark ? <Sun /> : <Moon />}
    </Button>
  )
}
