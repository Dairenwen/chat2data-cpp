// 应用外壳：可折叠侧栏、五项导航、最近会话、用户菜单；移动端侧栏收成抽屉。
import * as DropdownMenu from "@radix-ui/react-dropdown-menu"
import { useQuery } from "@tanstack/react-query"
import {
  Database, FileSpreadsheet, FolderOpen, History, LogOut, Menu, Monitor, Moon,
  PanelLeftClose, PanelLeftOpen, Plus, Sun, UserRound, X,
} from "lucide-react"
import { type ReactNode, useMemo, useState } from "react"
import { ai } from "../api/ai"
import { type UserInfo } from "../api/users"
import { dateGroup, dateGroups } from "../lib/utils"
import { cn } from "../lib/utils"
import { type Route, navigate } from "../lib/router"
import { type Theme, useApp } from "../store/app"
import { Brand } from "./Brand"
import { Button } from "./ui"

const navItems: { route: Route; label: string; icon: typeof Database }[] = [
  { route: "excel", label: "智能 Excel 助手", icon: FileSpreadsheet },
  { route: "db", label: "智能 DB 助手", icon: Database },
  { route: "files", label: "我的文件", icon: FolderOpen },
  { route: "history", label: "历史会话", icon: History },
  { route: "profile", label: "个人中心", icon: UserRound },
]

const themeItems: { value: Theme; label: string; icon: typeof Sun }[] = [
  { value: "light", label: "浅色", icon: Sun },
  { value: "dark", label: "深色", icon: Moon },
  { value: "system", label: "跟随系统", icon: Monitor },
]

const menuItem = "flex cursor-pointer items-center gap-2 rounded-md px-2 py-1.5 text-sm outline-none data-[highlighted]:bg-accent"

export function Avatar({ nickname, className }: { nickname: string; className?: string }) {
  return (
    <span aria-hidden="true" className={cn("grid size-8 shrink-0 place-items-center rounded-full bg-secondary text-sm", className)}>
      {Array.from(nickname)[0]?.toUpperCase() || "U"}
    </span>
  )
}

function UserMenu({ user, collapsed, onLogout }: { user: UserInfo; collapsed: boolean; onLogout: () => void }) {
  const { theme, setTheme } = useApp()
  return (
    <DropdownMenu.Root>
      <DropdownMenu.Trigger asChild>
        <button
          aria-label="用户菜单"
          className="flex w-full items-center gap-2 rounded-md p-2 text-left transition-ui hover:bg-accent"
        >
          <Avatar nickname={user.nickname} />
          {!collapsed && (
            <span className="min-w-0 flex-1">
              <span className="block truncate text-sm" title={user.nickname}>{user.nickname}</span>
              <span className="block truncate text-xs text-muted-foreground" title={user.email}>{user.email}</span>
            </span>
          )}
        </button>
      </DropdownMenu.Trigger>
      <DropdownMenu.Portal>
        <DropdownMenu.Content side="top" align="start" sideOffset={6} className="z-50 w-52 rounded-md border bg-popover p-1 shadow-float">
          <DropdownMenu.Item className={menuItem} onSelect={() => navigate("profile")}>
            <UserRound className="size-4" strokeWidth={1.5} />个人中心
          </DropdownMenu.Item>
          <DropdownMenu.Separator className="my-1 h-px bg-border" />
          <DropdownMenu.Label className="px-2 py-1 text-xs text-muted-foreground">主题</DropdownMenu.Label>
          <DropdownMenu.RadioGroup value={theme} onValueChange={(value) => setTheme(value as Theme)}>
            {themeItems.map(({ value, label, icon: Icon }) => (
              <DropdownMenu.RadioItem key={value} value={value} className={menuItem}>
                <Icon className="size-4" strokeWidth={1.5} />{label}
                <DropdownMenu.ItemIndicator className="ml-auto text-xs text-muted-foreground">当前</DropdownMenu.ItemIndicator>
              </DropdownMenu.RadioItem>
            ))}
          </DropdownMenu.RadioGroup>
          <DropdownMenu.Separator className="my-1 h-px bg-border" />
          <DropdownMenu.Item className={menuItem} onSelect={onLogout}>
            <LogOut className="size-4" strokeWidth={1.5} />退出登录
          </DropdownMenu.Item>
        </DropdownMenu.Content>
      </DropdownMenu.Portal>
    </DropdownMenu.Root>
  )
}

// 最近会话：按今天、昨天、近 7 天、更早分组；接口未接通时给出明确提示。
function RecentSessions({ onPick }: { onPick: () => void }) {
  const query = useQuery({ queryKey: ["chatSessions"], queryFn: ai.sessions })
  const groups = useMemo(() => {
    const sorted = [...(query.data ?? [])].sort((a, b) => b.updatedAt - a.updatedAt).slice(0, 30)
    return dateGroups
      .map((name) => ({ name, items: sorted.filter((item) => dateGroup(item.updatedAt) === name) }))
      .filter((group) => group.items.length)
  }, [query.data])

  if (query.isPending) return <p className="px-2 text-xs text-muted-foreground" role="status">加载中…</p>
  if (query.isError) {
    return (
      <p className="px-2 text-xs text-muted-foreground" role="alert">
        暂无法获取最近会话。
        <button className="ml-1 text-ember-text underline" onClick={() => void query.refetch()}>重试</button>
      </p>
    )
  }
  if (!groups.length) return <p className="px-2 text-xs text-muted-foreground">还没有会话</p>
  return (
    <div className="grid gap-3">
      {groups.map((group) => (
        <div key={group.name}>
          <p className="px-2 pb-1 text-xs text-muted-foreground">{group.name}</p>
          {group.items.map((item) => {
            const title = (item.title || item.firstUserMessageContent || "新会话").slice(0, 20)
            return (
              <button
                key={item.chatSessionId}
                title={title}
                onClick={() => { navigate("history", { open: item.chatSessionId }); onPick() }}
                className="block w-full truncate rounded-md px-2 py-1.5 text-left text-sm transition-ui hover:bg-accent"
              >
                {title}
              </button>
            )
          })}
        </div>
      ))}
    </div>
  )
}

type Props = { route: Route; user: UserInfo; onLogout: () => void; children: ReactNode }

export function AppShell({ route, user, onLogout, children }: Props) {
  const { sidebarCollapsed, toggleSidebar } = useApp()
  const [drawerOpen, setDrawerOpen] = useState(false)

  // 抽屉始终展开显示；桌面端按折叠状态显示图标态。
  const sidebar = (collapsed: boolean, closeDrawer: () => void) => (
    <div className="flex h-full flex-col gap-3 p-3">
      <div className={cn("flex items-center", collapsed ? "justify-center" : "justify-between")}>
        {!collapsed && <Brand />}
        <Button
          variant="ghost"
          size="icon"
          aria-label={collapsed ? "展开侧栏" : "收起侧栏"}
          className="hidden md:inline-flex"
          onClick={toggleSidebar}
        >
          {collapsed ? <PanelLeftOpen /> : <PanelLeftClose />}
        </Button>
        <Button variant="ghost" size="icon" aria-label="关闭导航" className="md:hidden" onClick={closeDrawer}>
          <X />
        </Button>
      </div>
      <Button
        variant="outline"
        aria-label="新建会话"
        title="新建会话"
        className={collapsed ? "px-0" : "justify-start"}
        onClick={() => { navigate(route === "db" ? "db" : "excel", { new: String(Date.now()) }); closeDrawer() }}
      >
        <Plus />{!collapsed && "新建会话"}
      </Button>
      <nav aria-label="主导航" className="grid gap-0.5">
        {navItems.map(({ route: target, label, icon: Icon }) => {
          const active = route === target
          return (
            <button
              key={target}
              aria-current={active ? "page" : undefined}
              aria-label={label}
              title={label}
              onClick={() => { navigate(target); closeDrawer() }}
              className={cn(
                "relative flex h-9 items-center gap-2 rounded-md px-2.5 text-sm transition-ui hover:bg-accent",
                collapsed && "justify-center px-0",
                active && "bg-accent",
              )}
            >
              {active && <span aria-hidden="true" className="absolute inset-y-1.5 left-0 w-0.5 bg-forest" />}
              <Icon className="size-4 shrink-0 text-muted-foreground" strokeWidth={1.5} />
              {!collapsed && label}
            </button>
          )
        })}
      </nav>
      {!collapsed && (
        <div className="min-h-0 flex-1 overflow-y-auto">
          <p className="px-2 pb-2 text-xs text-muted-foreground">最近会话</p>
          <RecentSessions onPick={closeDrawer} />
        </div>
      )}
      {collapsed && <div className="flex-1" />}
      <UserMenu user={user} collapsed={collapsed} onLogout={onLogout} />
    </div>
  )

  return (
    <div className="flex h-dvh overflow-hidden bg-background">
      <aside
        aria-label="侧栏"
        className={cn("hidden shrink-0 border-r border-sidebar-border bg-sidebar md:block", sidebarCollapsed ? "w-14" : "w-64")}
      >
        {sidebar(sidebarCollapsed, () => {})}
      </aside>

      {drawerOpen && (
        <div className="fixed inset-0 z-40 md:hidden">
          <button aria-label="关闭导航" className="absolute inset-0 bg-foreground/30" onClick={() => setDrawerOpen(false)} />
          <aside aria-label="侧栏" className="absolute inset-y-0 left-0 w-72 max-w-[85vw] border-r border-sidebar-border bg-sidebar">
            {sidebar(false, () => setDrawerOpen(false))}
          </aside>
        </div>
      )}

      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex h-[52px] shrink-0 items-center gap-2 border-b px-3 md:hidden">
          <Button variant="ghost" size="icon" aria-label="打开导航" onClick={() => setDrawerOpen(true)}><Menu /></Button>
          <Brand />
        </header>
        <main className="min-h-0 flex-1 overflow-hidden">{children}</main>
      </div>
    </div>
  )
}
