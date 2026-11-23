// 模型列表：进入页面时获取，供输入框下方的模型下拉使用；失败不阻塞页面。
import { useQuery } from "@tanstack/react-query"
import { useEffect, useState } from "react"
import { ai } from "../api/ai"

export function useModels() {
  const query = useQuery({ queryKey: ["models"], queryFn: ai.models })
  const [modelId, setModelId] = useState("")

  // 默认选中第一个可用模型。
  useEffect(() => {
    if (!modelId && query.data?.length) setModelId(query.data[0].modelConfigId)
  }, [modelId, query.data])

  const selected = query.data?.find((item) => item.modelConfigId === modelId)
  return { models: query.data, loading: query.isPending, error: query.error, modelId, setModelId, selected }
}
