// 客户端全局状态：登录会话、用户资料、主题、侧栏折叠。
import { create } from "zustand"
import type { UserInfo } from "../api/users"
import { storageKey } from "../lib/constants"

export type Theme = "light" | "dark" | "system"

function readTheme(): Theme {
  const value = localStorage.getItem(storageKey.theme)
  return value === "light" || value === "dark" ? value : "system"
}

export function applyTheme(theme: Theme) {
  const dark = theme === "dark" || (theme === "system" && matchMedia("(prefers-color-scheme: dark)").matches)
  document.documentElement.classList.toggle("dark", dark)
}

type AppState = {
  sessionId: string | null
  user: UserInfo | null
  theme: Theme
  sidebarCollapsed: boolean
  setSession: (sessionId: string) => void
  setUser: (user: UserInfo) => void
  clearAuth: () => void
  setTheme: (theme: Theme) => void
  toggleSidebar: () => void
}

export const useApp = create<AppState>((set) => ({
  sessionId: localStorage.getItem(storageKey.session),
  user: null,
  theme: readTheme(),
  sidebarCollapsed: false,
  setSession: (sessionId) => {
    localStorage.setItem(storageKey.session, sessionId)
    set({ sessionId })
  },
  setUser: (user) => set({ user }),
  clearAuth: () => {
    localStorage.removeItem(storageKey.session)
    set({ sessionId: null, user: null })
  },
  setTheme: (theme) => {
    if (theme === "system") localStorage.removeItem(storageKey.theme)
    else localStorage.setItem(storageKey.theme, theme)
    applyTheme(theme)
    set({ theme })
  },
  toggleSidebar: () => set((state) => ({ sidebarCollapsed: !state.sidebarCollapsed })),
}))
