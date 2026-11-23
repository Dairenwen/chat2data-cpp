// 预览区：底部工作表标签（一个 Worksheet 对应一张表），表格只读，沿用接口的分页参数。
import { useQuery } from "@tanstack/react-query"
import { ChevronLeft, ChevronRight } from "lucide-react"
import { useState } from "react"
import { files } from "../api/files"
import type { ExcelPreview } from "../api/files"
import { cn } from "../lib/utils"
import { DataTable } from "./DataTable"
import { Button, ErrorState, ListSkeleton } from "./ui"

const pageSize = 50

export function SheetPreview({ fileId, initial }: { fileId: string; initial?: ExcelPreview }) {
  const [sheet, setSheet] = useState<string>()
  const [page, setPage] = useState(1)

  // 不传 sheetName 时返回各工作表第一页，用于取得工作表列表。
  const overview = useQuery({
    queryKey: ["preview", fileId, "overview"],
    queryFn: () => files.preview(fileId),
    initialData: initial,
  })
  const names = overview.data?.excelData.sheets.map((item) => item.name) ?? []
  const current = sheet ?? names[0]

  const detail = useQuery({
    queryKey: ["preview", fileId, current, page],
    queryFn: () => files.preview(fileId, current, page, pageSize),
    enabled: !!current,
  })

  if (overview.isPending) return <ListSkeleton />
  if (overview.isError) return <ErrorState error={overview.error} onRetry={() => void overview.refetch()} />
  if (!names.length) return <p className="p-4 text-sm text-muted-foreground">文件中没有工作表。</p>

  const data = detail.data?.excelData.sheets.find((item) => item.name === current) ??
    overview.data?.excelData.sheets.find((item) => item.name === current)
  const totalPages = Math.max(1, data?.totalPages ?? 1)

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="min-h-0 flex-1 overflow-auto p-3">
        {detail.isError ? (
          <ErrorState error={detail.error} onRetry={() => void detail.refetch()} />
        ) : !data ? <ListSkeleton /> : data.data.length ? (
          <DataTable columns={data.columns} rows={data.data} startRow={(data.currentPage - 1) * data.pageSize + 1} />
        ) : <p className="p-4 text-sm text-muted-foreground">该工作表没有数据。</p>}
      </div>
      <div className="flex shrink-0 items-center gap-2 border-t bg-card px-2">
        <div role="tablist" aria-label="工作表" className="flex min-w-0 flex-1 gap-1 overflow-x-auto py-1">
          {names.map((name) => (
            <button
              key={name}
              role="tab"
              aria-selected={name === current}
              onClick={() => { setSheet(name); setPage(1) }}
              className={cn("max-w-40 shrink-0 truncate rounded-md px-3 py-1 text-sm transition-ui", name === current ? "bg-secondary" : "text-muted-foreground hover:text-foreground")}
              title={name}
            >
              {name}
            </button>
          ))}
        </div>
        {data && (
          <div className="flex shrink-0 items-center gap-1 text-xs text-muted-foreground">
            <span className="hidden sm:inline">{data.totalRows} 行 · {data.colCount} 列</span>
            <Button variant="ghost" size="icon" aria-label="上一页" disabled={page <= 1} onClick={() => setPage(page - 1)}><ChevronLeft /></Button>
            <span className="tabular-nums">{page} / {totalPages}</span>
            <Button variant="ghost" size="icon" aria-label="下一页" disabled={page >= totalPages} onClick={() => setPage(page + 1)}><ChevronRight /></Button>
          </div>
        )}
      </div>
    </div>
  )
}
