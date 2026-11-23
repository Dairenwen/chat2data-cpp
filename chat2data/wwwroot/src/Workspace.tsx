// 页面分发：按路由渲染对应页面。
import { DbPage } from "./pages/DbPage"
import { ExcelPage } from "./pages/ExcelPage"
import { FilesPage } from "./pages/FilesPage"
import { HistoryPage } from "./pages/HistoryPage"
import { ProfilePage } from "./pages/ProfilePage"
import type { Route } from "./lib/router"

export function Workspace({ route, onLogout }: { route: Route; onLogout: () => void }) {
  switch (route) {
    case "excel": return <ExcelPage />
    case "db": return <DbPage />
    case "files": return <FilesPage />
    case "history": return <HistoryPage />
    case "profile": return <ProfilePage onLogout={onLogout} />
  }
}
