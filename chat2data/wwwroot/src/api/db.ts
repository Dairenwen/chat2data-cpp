// 数据库接口：字段以接口设计 1.4 节为准，仅支持 MySQL 和 SQLite。
import { request } from "./client"

export type MySqlConfig = {
  host: string
  port: number
  name: string
  username: string
  password: string
  charset: string
}

export type DatabaseConfig =
  | { type: "MySQL"; MySQL: MySqlConfig }
  | { type: "SQLite"; SQLite: { fileId: string; readonly: boolean } }

export type TableData = {
  tableSchema: { columnInfo: { name: string; type: string }[] }
  tableData: { rows: { cells: string[] }[] }
}

export type ConnectionStatus = { tempTables?: string[]; hasModifications?: boolean }

function requireId(value: unknown, message: string) {
  if (typeof value !== "string" || !value) throw new Error(message)
  return value
}

export const db = {
  async connect(database: DatabaseConfig) {
    const data = await request<{ connectionId: string }>("/api/db/connect", { body: { database } })
    return requireId(data?.connectionId, "网关未返回数据库连接 ID。")
  },
  async tables(connectionId: string) {
    const data = await request<{ tableList: string[] }>("/api/db/tables", { method: "GET", query: { connectionId } })
    return data?.tableList ?? []
  },
  disconnect: (connectionId: string) => request("/api/db/disconnect", { body: { connectionId } }),
  // forceOriginal 为 true 时读取原表，否则读取备份表（若存在）。
  tableData: (connectionId: string, tableName: string, forceOriginal: boolean) =>
    request<TableData>("/api/db/table/data", { body: { connectionId, tableName, forceOriginal } }),
  // tempTables 与 hasModifications 位于响应顶层。
  connectionStatus: (connectionId: string) =>
    request<ConnectionStatus>("/api/db/connection/status", { body: { connectionId }, whole: true }),
}
