// 文件表格：我的文件页与个人中心复用同一组件。
// 名称、类型图标、大小、上传时间、操作；带类型筛选和搜索。
import * as Dialog from "@radix-ui/react-dialog"
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query"
import { Database, Download, Eye, FileSpreadsheet, Search, Trash2 } from "lucide-react"
import { useMemo, useState } from "react"
import { errorMessage } from "../api/client"
import { type FileItem, files } from "../api/files"
import { excelExt, sqliteExt } from "../lib/constants"
import { navigate } from "../lib/router"
import { cn, formatDateTime, formatSize } from "../lib/utils"
import { toast } from "../store/toast"
import { Button, EmptyState, ErrorState, Input, ListSkeleton } from "./ui"

type Kind = "all" | "excel" | "sqlite"

const extOf = (name: string) => (name.includes(".") ? name.split(".").pop()!.toLowerCase() : "")
const isExcel = (name: string) => extOf(name) === excelExt
const isSqlite = (name: string) => extOf(name) === sqliteExt

const filters: { value: Kind; label: string }[] = [
  { value: "all", label: "全部" },
  { value: "excel", label: "Excel" },
  { value: "sqlite", label: "SQLite" },
]

function saveBlob(blob: Blob, name: string) {
  const url = URL.createObjectURL(blob)
  const link = document.createElement("a")
  link.href = url
  link.download = name
  link.click()
  URL.revokeObjectURL(url)
}

export function FileTable() {
  const queryClient = useQueryClient()
  const [kind, setKind] = useState<Kind>("all")
  const [search, setSearch] = useState("")
  const [removing, setRemoving] = useState<FileItem | null>(null)
  const [downloading, setDownloading] = useState("")

  const list = useQuery({ queryKey: ["files"], queryFn: files.list })

  const remove = useMutation({
    mutationFn: (fileId: string) => files.remove(fileId),
    onSuccess: () => {
      toast.success("文件已删除。")
      setRemoving(null)
      void queryClient.invalidateQueries({ queryKey: ["files"] })
      // 关联的聊天会话随文件一起删除。
      void queryClient.invalidateQueries({ queryKey: ["chatSessions"] })
    },
    onError: (failure) => toast.error(errorMessage(failure)),
  })

  const rows = useMemo(() => {
    const keyword = search.trim().toLowerCase()
    return [...(list.data ?? [])]
      .filter((item) => kind === "all" || (kind === "excel" ? isExcel(item.fileName) : isSqlite(item.fileName)))
      .filter((item) => !keyword || item.fileName.toLowerCase().includes(keyword))
      .sort((a, b) => b.uploadTime - a.uploadTime)
  }, [list.data, kind, search])

  async function download(item: FileItem) {
    setDownloading(item.fileId)
    try {
      saveBlob(await files.download(item.fileId), item.fileName)
    } catch (failure) {
      toast.error(errorMessage(failure))
    } finally {
      setDownloading("")
    }
  }

  // 预览：Excel 跳转到智能 Excel 页；SQLite 跳转到智能 DB 页并建立对应连接。
  function preview(item: FileItem) {
    if (isSqlite(item.fileName)) navigate("db", { file: item.fileId, name: item.fileName })
    else navigate("excel", { file: item.fileId })
  }

  return (
    <div className="grid gap-3">
      <div className="flex flex-wrap items-center gap-2">
        <div role="group" aria-label="文件类型" className="flex gap-1 rounded-md border bg-card p-0.5">
          {filters.map((item) => (
            <button
              key={item.value}
              aria-pressed={kind === item.value}
              onClick={() => setKind(item.value)}
              className={cn("h-8 rounded-md px-3 text-sm transition-ui", kind === item.value ? "bg-secondary" : "text-muted-foreground hover:text-foreground")}
            >
              {item.label}
            </button>
          ))}
        </div>
        <div className="relative min-w-48 flex-1 sm:max-w-xs">
          <Search className="pointer-events-none absolute top-2.5 left-2.5 size-4 text-muted-foreground" aria-hidden="true" />
          <Input aria-label="搜索文件" placeholder="搜索文件名" className="pl-8" value={search} onChange={(event) => setSearch(event.target.value)} />
        </div>
      </div>

      {list.isPending ? <ListSkeleton /> : list.isError ? (
        <ErrorState error={list.error} onRetry={() => void list.refetch()} />
      ) : !list.data.length ? (
        <EmptyState title="还没有上传过文件" hint="在智能 Excel 助手或智能 DB 助手中上传文件后会显示在这里。" />
      ) : !rows.length ? (
        <EmptyState title="没有符合条件的文件" hint="换一个类型或关键词试试。" />
      ) : (
        <div className="overflow-x-auto rounded-md border">
          <table className="w-full min-w-[560px] border-collapse text-sm">
            <thead className="bg-card text-muted-foreground">
              <tr>
                {["名称", "大小", "上传时间", "操作"].map((head) => (
                  <th key={head} scope="col" className={cn("border-b px-3 py-2 text-left font-normal", head === "大小" && "text-right")}>{head}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {rows.map((item) => (
                <tr key={item.fileId} className="hover:bg-card">
                  <td className="max-w-[260px] border-b px-3 py-2">
                    <span className="flex items-center gap-2">
                      {isSqlite(item.fileName)
                        ? <Database className="size-4 shrink-0 text-muted-foreground" strokeWidth={1.5} aria-label="SQLite" />
                        : <FileSpreadsheet className="size-4 shrink-0 text-muted-foreground" strokeWidth={1.5} aria-label="Excel" />}
                      <span className="truncate" title={item.fileName}>{item.fileName}</span>
                    </span>
                  </td>
                  <td className="border-b px-3 py-2 text-right font-mono tabular-nums">{formatSize(item.fileSize)}</td>
                  <td className="border-b px-3 py-2 tabular-nums">{formatDateTime(item.uploadTime)}</td>
                  <td className="border-b px-3 py-1">
                    <div className="flex gap-1">
                      <Button variant="ghost" size="icon" aria-label={`预览 ${item.fileName}`} title="预览" onClick={() => preview(item)}><Eye /></Button>
                      <Button variant="ghost" size="icon" aria-label={`下载 ${item.fileName}`} title="下载" disabled={downloading === item.fileId} onClick={() => void download(item)}><Download /></Button>
                      <Button variant="ghost" size="icon" aria-label={`删除 ${item.fileName}`} title="删除" onClick={() => setRemoving(item)}><Trash2 /></Button>
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      <Dialog.Root open={!!removing} onOpenChange={(open) => { if (!open && !remove.isPending) setRemoving(null) }}>
        <Dialog.Portal>
          <Dialog.Overlay className="fixed inset-0 z-50 bg-foreground/30" />
          <Dialog.Content className="fixed top-1/2 left-1/2 z-50 grid w-[min(420px,calc(100vw-2rem))] -translate-x-1/2 -translate-y-1/2 gap-4 rounded-dialog border bg-popover p-6 shadow-float">
            <Dialog.Title className="text-lg">删除文件</Dialog.Title>
            <Dialog.Description className="text-sm text-muted-foreground">
              确定删除「{removing?.fileName}」吗？将同时删除该文件关联的聊天会话，且无法恢复。
            </Dialog.Description>
            <div className="flex justify-end gap-2">
              <Button variant="outline" disabled={remove.isPending} onClick={() => setRemoving(null)}>取消</Button>
              <Button variant="destructive" disabled={remove.isPending} onClick={() => removing && remove.mutate(removing.fileId)}>
                {remove.isPending ? "删除中…" : "确认删除"}
              </Button>
            </div>
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>
    </div>
  )
}
