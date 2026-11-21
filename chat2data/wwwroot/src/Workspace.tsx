import {
  ChevronRight, FileSpreadsheet, LogOut, Menu, PanelRightClose, PanelRightOpen,
  Paperclip, Send, Table2, Upload, X,
} from "lucide-react"
import { useState } from "react"
import type { UserInfo } from "./api/users"
import { Brand, ThemeButton } from "./components/Brand"
import { Button } from "./components/ui"

type Props = {
  user: UserInfo
  dark: boolean
  toggleTheme: () => void
  onLogout: () => void
  pending: boolean
  error: string
}

export function Workspace({ user, dark, toggleTheme, onLogout, pending, error }: Props) {
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const [previewOpen, setPreviewOpen] = useState(() => window.matchMedia("(min-width: 1024px)").matches)
  const previewLabel = previewOpen ? "收起工作表预览" : "打开工作表预览"

  return (
    <div className="workspace">
      {sidebarOpen && (
        <button className="drawer-backdrop" aria-label="关闭导航" onClick={() => setSidebarOpen(false)} />
      )}
      <aside className={`workspace-sidebar ${sidebarOpen ? "sidebar-open" : ""}`}>
        <div className="sidebar-brand">
          <Brand />
          <Button className="mobile-only" size="icon" variant="ghost" aria-label="关闭导航"
            onClick={() => setSidebarOpen(false)}><X /></Button>
        </div>
        <nav aria-label="工作台导航">
          <div className="active-nav" aria-current="page">
            <FileSpreadsheet size={18} /> 智能 Excel <ChevronRight size={16} className="ml-auto" />
          </div>
        </nav>
        <div className="sidebar-bottom">
          <div className="profile">
            <span className="avatar" aria-hidden="true">{Array.from(user.nickname)[0]?.toUpperCase() || "U"}</span>
            <div className="min-w-0">
              <strong title={user.nickname}>{user.nickname}</strong>
              <span title={user.email}>{user.email}</span>
            </div>
          </div>
          <Button variant="outline" className="w-full" onClick={onLogout} disabled={pending}>
            <LogOut />{pending ? "正在退出…" : "退出登录"}
          </Button>
          {error && <p className="form-error" role="alert">{error}</p>}
        </div>
      </aside>

      <section className="workspace-main" aria-label="Excel 对话">
        <header className="workspace-header">
          <div className="flex min-w-0 items-center gap-3">
            <Button className="mobile-only" size="icon" variant="ghost" aria-label="打开导航"
              aria-expanded={sidebarOpen} onClick={() => setSidebarOpen(true)}><Menu /></Button>
            <h1>Excel 工作台</h1>
          </div>
          <div className="flex items-center gap-1">
            <ThemeButton dark={dark} toggle={toggleTheme} />
            <Button size="icon" variant="ghost" aria-label={previewLabel} title={previewLabel}
              aria-expanded={previewOpen} onClick={() => setPreviewOpen(!previewOpen)}>
              {previewOpen ? <PanelRightClose /> : <PanelRightOpen />}
            </Button>
          </div>
        </header>

        <div className="conversation">
          <div className="conversation-empty">
            <span className="conversation-icon"><FileSpreadsheet size={34} /></span>
            <h2>智能 Excel 助手</h2>
            <p>在这里上传工作簿、预览表格并向数据提问。</p>
            <div className="feature-unavailable"><span />文件与 AI 服务尚未接通</div>
          </div>
        </div>

        <div className="composer">
          <textarea aria-label="输入分析问题" placeholder="AI 服务接通后即可发送问题" disabled />
          <div className="composer-actions">
            <Button size="icon" variant="ghost" aria-label="上传 Excel" disabled><Paperclip /></Button>
            <Button size="icon" aria-label="发送消息" disabled><Send /></Button>
          </div>
        </div>
      </section>

      {previewOpen && (
        <aside className="preview-panel" aria-label="工作表预览">
          <header className="preview-header">
            <Table2 size={18} /><h2>工作表预览</h2>
            <Button className="ml-auto" size="icon" variant="ghost" aria-label="关闭工作表预览"
              onClick={() => setPreviewOpen(false)}><X /></Button>
          </header>
          <div className="preview-content">
            <div className="upload-placeholder">
              <span className="upload-icon"><Upload size={22} /></span>
              <h3>上传 Excel 工作簿</h3>
              <p>支持 .xlsx，最大 10 MB</p>
              <Button variant="outline" disabled>选择文件</Button>
            </div>
            <p className="preview-note">文件服务接通后，可在此查看工作表。</p>
          </div>
        </aside>
      )}
    </div>
  )
}
