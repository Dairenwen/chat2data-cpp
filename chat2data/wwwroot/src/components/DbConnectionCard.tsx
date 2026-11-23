// 连接信息卡片：已有连接时再次进入智能 DB 助手显示，不回显密码。
import { Database } from "lucide-react"
import { Badge, Button } from "./ui"
import type { ConnectedInfo } from "./DbConnectForm"

type Props = { info: ConnectedInfo; disconnecting: boolean; onUse: () => void; onDisconnect: () => void }

export function DbConnectionCard({ info, disconnecting, onUse, onDisconnect }: Props) {
  return (
    <div className="mx-auto grid w-full max-w-md gap-4 p-6">
      <div className="grid gap-3 rounded-md border bg-card p-6">
        <div className="flex items-center gap-2">
          <Database className="size-5 text-muted-foreground" strokeWidth={1.5} aria-hidden="true" />
          <span className="text-base">当前数据库连接</span>
          <Badge>{info.type}</Badge>
        </div>
        <p className="break-all text-sm text-muted-foreground">{info.label}</p>
        <div className="flex gap-2">
          <Button onClick={onUse} disabled={disconnecting}>使用此连接</Button>
          <Button variant="outline" onClick={onDisconnect} disabled={disconnecting}>{disconnecting ? "断开中…" : "断开连接"}</Button>
        </div>
      </div>
    </div>
  )
}
