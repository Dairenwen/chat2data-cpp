// 个人中心：单页分区卡片。后端没有修改资料和上传头像的接口，因此不提供编辑功能。
import { LogOut } from "lucide-react"
import { Avatar } from "../components/AppShell"
import { FileTable } from "../components/FileTable"
import { Button } from "../components/ui"
import { cn } from "../lib/utils"
import { type Theme, useApp } from "../store/app"

const themes: { value: Theme; label: string }[] = [
  { value: "light", label: "浅色" },
  { value: "dark", label: "深色" },
  { value: "system", label: "跟随系统" },
]

function Card({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section aria-label={title} className="grid gap-4 rounded-md border bg-card p-4 sm:p-6">
      <h2 className="text-lg">{title}</h2>
      {children}
    </section>
  )
}

export function ProfilePage({ onLogout }: { onLogout: () => void }) {
  const { user, theme, setTheme } = useApp()
  if (!user) return null
  return (
    <div className="h-full overflow-y-auto">
      <div className="mx-auto grid max-w-4xl gap-4 p-4 sm:p-6">
        <h1 className="text-2xl">个人中心</h1>
        <Card title="个人资料">
          <div className="flex items-center gap-4">
            <Avatar nickname={user.nickname} className="size-14 text-xl" />
            <dl className="grid min-w-0 gap-1 text-sm">
              <div className="flex gap-2"><dt className="text-muted-foreground">昵称</dt><dd className="truncate">{user.nickname}</dd></div>
              <div className="flex gap-2"><dt className="text-muted-foreground">邮箱</dt><dd className="truncate">{user.email}</dd></div>
              <div className="flex gap-2"><dt className="text-muted-foreground">状态</dt><dd>在线</dd></div>
            </dl>
          </div>
        </Card>
        <Card title="我的文件"><FileTable /></Card>
        <Card title="外观设置">
          <div role="group" aria-label="主题" className="flex w-fit gap-1 rounded-md border bg-background p-0.5">
            {themes.map((item) => (
              <button
                key={item.value}
                aria-pressed={theme === item.value}
                onClick={() => setTheme(item.value)}
                className={cn("h-8 rounded-md px-3 text-sm transition-ui", theme === item.value ? "bg-secondary" : "text-muted-foreground hover:text-foreground")}
              >
                {item.label}
              </button>
            ))}
          </div>
        </Card>
        <Card title="退出登录">
          <div><Button variant="outline" onClick={onLogout}><LogOut />退出登录</Button></div>
        </Card>
      </div>
    </div>
  )
}
