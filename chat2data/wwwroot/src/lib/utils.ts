import { clsx, type ClassValue } from "clsx"
import { twMerge } from "tailwind-merge"

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs))
}

export function formatSize(bytes: number) {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

// 接口设计未统一文件时间单位（示例为 Unix 秒，ModelBridge 相关为 Unix 毫秒），按数值量级归一化为毫秒。
export function toMillis(time: number) {
  return time < 1e11 ? time * 1000 : time
}

export function formatDateTime(time: number) {
  return new Date(toMillis(time)).toLocaleString("zh-CN", { hour12: false })
}

export const dateGroups = ["今天", "昨天", "近 7 天", "更早"] as const
export type DateGroup = (typeof dateGroups)[number]

export function dateGroup(time: number, now = new Date()): DateGroup {
  const day = 86_400_000
  const startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime()
  const value = toMillis(time)
  if (value >= startOfToday) return "今天"
  if (value >= startOfToday - day) return "昨天"
  if (value >= startOfToday - 6 * day) return "近 7 天"
  return "更早"
}
