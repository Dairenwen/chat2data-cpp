// 结果分析区：分析任务、关键发现、分析总结、数据可视化。
// 接口设计 1.5.6 的 SSE 只返回 content / errorCode / errorMsg，没有任务、发现、总结和图表字段。
// 不假设后端提供这些字段，没有的区块不渲染；后端补充事件格式后在此处对接。
import { ChartRenderer } from "./ChartRenderer"
import { EmptyState } from "./ui"

export type AnalysisResult = {
  // 预留：仅当后端按接口设计提供对应字段时才会有值。
  charts?: unknown[]
}

export function AnalysisPanel({ result }: { result?: AnalysisResult }) {
  if (!result?.charts?.length) {
    return (
      <EmptyState
        title="暂无分析结果"
        hint="当前接口设计的流式响应只包含回答文本，分析任务、关键发现、总结和图表待后端提供对应字段后展示。"
      />
    )
  }
  return (
    <div className="grid gap-4 p-4">
      {result.charts.map((chart, i) => <ChartRenderer key={i} raw={chart} />)}
    </div>
  )
}
