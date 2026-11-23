// 登录页右侧的静态产品示意：窗口栏 + 迷你对话 + 柱状图，全部用代码绘制。
const bars = [46, 72, 58, 88, 64]

export function AuthShowcase() {
  return (
    <div className="w-full max-w-md rounded-md border bg-card shadow-demo" aria-hidden="true">
      <div className="relative flex h-9 items-center border-b px-3">
        <div className="flex gap-1.5">
          {[0, 1, 2].map((i) => <span key={i} className="size-2.5 rounded-full bg-muted-foreground/40" />)}
        </div>
        <span className="absolute inset-x-0 text-center text-xs text-muted-foreground">智能 Excel 助手</span>
      </div>
      <div className="grid gap-4 p-5 text-sm">
        <div className="ml-auto max-w-[80%] rounded-md border bg-background px-3 py-2">各季度销售额对比如何？</div>
        <p className="leading-relaxed">第四季度销售额最高，较第一季度增长约 90%，华东区域贡献最大。</p>
        <div className="rounded-md border bg-background p-4">
          <div className="flex h-24 items-end gap-3">
            {bars.map((height, index) => (
              <span
                key={index}
                className="flex-1 rounded-t-xs"
                style={{ height: `${height}%`, background: index === 3 ? "var(--ember)" : "var(--chart-1)" }}
              />
            ))}
          </div>
          <div className="mt-2 flex justify-between text-xs text-muted-foreground">
            <span>Q1</span><span>Q2</span><span>Q3</span><span>Q4</span><span>Q5</span>
          </div>
        </div>
      </div>
    </div>
  )
}
