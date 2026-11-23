// 集中定义可调常量。
export const storageKey = { session: "chat2data.session", theme: "chat2data.theme" } as const

// 验证码重发冷却与连续输错上限（前端处理）。
export const resendCooldownMs = 60_000
export const codeAttemptLimit = 5

// 上传限制：Excel 仅 .xlsx 且不超过 10 MB，SQLite 仅 .db。
export const excelMaxBytes = 10 * 1024 * 1024
export const excelExt = "xlsx"
export const sqliteExt = "db"
