// 只读数据表格：Excel 预览与数据库表预览共用。表头吸顶、行号列、空值明确显示、数字右对齐。
import { type ColumnDef, flexRender, getCoreRowModel, useReactTable } from "@tanstack/react-table"
import { useMemo } from "react"
import { cn } from "../lib/utils"

const numeric = /^-?\d+(\.\d+)?$/

type Props = { columns: string[]; rows: string[][]; startRow?: number; className?: string }

function CellValue({ value }: { value: string | null | undefined }) {
  if (value === null || value === undefined || value === "") {
    return <span className="text-muted-foreground">空</span>
  }
  return <span title={value}>{value}</span>
}

export function DataTable({ columns, rows, startRow = 1, className }: Props) {
  const defs = useMemo<ColumnDef<string[]>[]>(() => [
    { id: "__row", header: "#", cell: (ctx) => startRow + ctx.row.index },
    ...columns.map((name, i): ColumnDef<string[]> => ({
      id: `c${i}`,
      header: name || `列 ${i + 1}`,
      accessorFn: (row) => row[i],
      cell: (ctx) => <CellValue value={ctx.getValue() as string | undefined} />,
    })),
  ], [columns, startRow])

  const table = useReactTable({ data: rows, columns: defs, getCoreRowModel: getCoreRowModel() })

  return (
    <div className={cn("overflow-auto rounded-md border", className)}>
      <table className="w-max min-w-full border-collapse text-sm">
        <thead className="sticky top-0 z-10 bg-card">
          {table.getHeaderGroups().map((group) => (
            <tr key={group.id}>
              {group.headers.map((header) => (
                <th
                  key={header.id}
                  scope="col"
                  className={cn(
                    "max-w-[240px] truncate border-b px-3 py-2 text-left font-normal text-muted-foreground",
                    header.id === "__row" && "sticky left-0 w-12 bg-card text-right",
                  )}
                  title={typeof header.column.columnDef.header === "string" ? header.column.columnDef.header : undefined}
                >
                  {flexRender(header.column.columnDef.header, header.getContext())}
                </th>
              ))}
            </tr>
          ))}
        </thead>
        <tbody>
          {table.getRowModel().rows.map((row) => (
            <tr key={row.id} className="hover:bg-card">
              {row.getVisibleCells().map((cell) => {
                const isRow = cell.column.id === "__row"
                const raw = isRow ? "" : String(cell.getValue() ?? "")
                return (
                  <td
                    key={cell.id}
                    className={cn(
                      "max-w-[240px] truncate border-b px-3 py-1.5",
                      isRow && "sticky left-0 bg-background text-right text-xs text-muted-foreground tabular-nums",
                      numeric.test(raw) && "text-right font-mono tabular-nums",
                    )}
                  >
                    {flexRender(cell.column.columnDef.cell, cell.getContext())}
                  </td>
                )
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}
