// 图表适配层：接口设计未定义可视化数据格式，仅识别 { type, data: [{ name, value }] }。
// 无法识别时退回原始文本；错误边界保证图表异常不会导致整页白屏。
import { Component, type ReactNode } from "react"
import { Bar, BarChart, CartesianGrid, Cell, Legend, Pie, PieChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts"
import { DataTable } from "./DataTable"

type Point = { name: string; value: number }
type Spec = { type: "bar" | "pie" | "table"; title?: string; data: Point[] }

const palette = ["var(--chart-1)", "var(--chart-2)", "var(--chart-3)", "var(--chart-4)", "var(--chart-5)"]
const overflowColors = ["#7a7974", "#cdcdc9"]
const colorAt = (i: number) => (i < palette.length ? palette[i] : overflowColors[(i - palette.length) % 2])

function normalize(raw: unknown): Spec | null {
  if (!raw || typeof raw !== "object") return null
  const { type, title, data } = raw as { type?: unknown; title?: unknown; data?: unknown }
  if ((type !== "bar" && type !== "pie" && type !== "table") || !Array.isArray(data) || !data.length) return null
  const points: Point[] = []
  for (const item of data) {
    const { name, value } = (item ?? {}) as { name?: unknown; value?: unknown }
    const number = Number(value)
    if (name === undefined || name === null || !Number.isFinite(number)) return null
    points.push({ name: String(name), value: number })
  }
  return { type, title: typeof title === "string" ? title : undefined, data: points }
}

function RawFallback({ raw }: { raw: unknown }) {
  const text = typeof raw === "string" ? raw : JSON.stringify(raw, null, 2)
  return (
    <pre className="max-h-64 overflow-auto rounded-md border bg-card p-3 font-mono text-xs">{text}</pre>
  )
}

class Boundary extends Component<{ raw: unknown; children: ReactNode }, { failed: boolean }> {
  state = { failed: false }
  static getDerivedStateFromError() {
    return { failed: true }
  }
  render() {
    return this.state.failed ? <RawFallback raw={this.props.raw} /> : this.props.children
  }
}

function Chart({ spec }: { spec: Spec }) {
  if (spec.type === "table") {
    return <DataTable columns={["名称", "数值"]} rows={spec.data.map((p) => [p.name, String(p.value)])} />
  }
  return (
    <div className="h-64 w-full">
      <ResponsiveContainer width="100%" height="100%">
        {spec.type === "bar" ? (
          <BarChart data={spec.data}>
            <CartesianGrid stroke="var(--border)" vertical={false} />
            <XAxis dataKey="name" tick={{ fill: "var(--muted-foreground)", fontSize: 12 }} />
            <YAxis tick={{ fill: "var(--muted-foreground)", fontSize: 12 }} />
            <Tooltip />
            <Bar dataKey="value" fill="var(--chart-1)" radius={[2, 2, 0, 0]} isAnimationActive={false} />
          </BarChart>
        ) : (
          <PieChart>
            <Tooltip />
            <Legend />
            <Pie data={spec.data} dataKey="value" nameKey="name" stroke="var(--background)" isAnimationActive={false}>
              {spec.data.map((_, i) => <Cell key={i} fill={colorAt(i)} />)}
            </Pie>
          </PieChart>
        )}
      </ResponsiveContainer>
    </div>
  )
}

export function ChartRenderer({ raw }: { raw: unknown }) {
  const spec = normalize(raw)
  return (
    <Boundary raw={raw}>
      {spec ? (
        <figure className="grid gap-2 rounded-md border bg-card p-6">
          {spec.title && <figcaption className="text-sm">{spec.title}</figcaption>}
          <Chart spec={spec} />
        </figure>
      ) : <RawFallback raw={raw} />}
    </Boundary>
  )
}
