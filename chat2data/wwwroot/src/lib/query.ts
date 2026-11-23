// 服务端数据（文件列表、历史会话、模型列表）统一由 TanStack Query 管理。
import { QueryClient } from "@tanstack/react-query"

// 请求失败由用户手动重试，不自动重试，避免重复提交。
export const queryClient = new QueryClient({
  defaultOptions: { queries: { retry: false, refetchOnWindowFocus: false, staleTime: 30_000 } },
})
