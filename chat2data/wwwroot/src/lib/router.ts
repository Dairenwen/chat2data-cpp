// Hash 路由：网关没有 SPA 回退，使用 #/route?key=value 形式，刷新和直接访问均可加载。
import { useSyncExternalStore } from "react"

export const routes = ["excel", "db", "files", "history", "profile"] as const
export type Route = (typeof routes)[number]

function subscribe(callback: () => void) {
  addEventListener("hashchange", callback)
  return () => removeEventListener("hashchange", callback)
}

function parse(hash: string) {
  const [path, query = ""] = hash.replace(/^#\/?/, "").split("?")
  const route = (routes as readonly string[]).includes(path) ? (path as Route) : "excel"
  return { route, query: new URLSearchParams(query) }
}

export function useHashRoute() {
  const hash = useSyncExternalStore(subscribe, () => location.hash)
  return parse(hash)
}

export function navigate(route: Route, query?: Record<string, string>) {
  const search = query ? `?${new URLSearchParams(query)}` : ""
  location.hash = `/${route}${search}`
}
