// 当前数据库连接：仅保存在内存，不含密码，不写入 localStorage。
import { create } from "zustand"
import type { ConnectedInfo } from "../components/DbConnectForm"

type DbState = {
  connection: ConnectedInfo | null
  setConnection: (connection: ConnectedInfo | null) => void
}

export const useDb = create<DbState>((set) => ({
  connection: null,
  setConnection: (connection) => set({ connection }),
}))
