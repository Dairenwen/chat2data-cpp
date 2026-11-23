// 全局 Toast 状态，4 秒后自动消失。
import { create } from "zustand"

export type ToastKind = "info" | "success" | "error"
export type ToastItem = { id: number; kind: ToastKind; text: string }

type ToastState = {
  items: ToastItem[]
  push: (kind: ToastKind, text: string) => void
  dismiss: (id: number) => void
}

let nextId = 1

export const useToast = create<ToastState>((set) => ({
  items: [],
  push: (kind, text) => {
    const id = nextId++
    set((state) => ({ items: [...state.items, { id, kind, text }] }))
    setTimeout(() => set((state) => ({ items: state.items.filter((item) => item.id !== id) })), 4000)
  },
  dismiss: (id) => set((state) => ({ items: state.items.filter((item) => item.id !== id) })),
}))

export const toast = {
  info: (text: string) => useToast.getState().push("info", text),
  success: (text: string) => useToast.getState().push("success", text),
  error: (text: string) => useToast.getState().push("error", text),
}
