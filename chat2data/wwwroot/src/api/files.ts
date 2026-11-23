// 文件接口：字段以接口设计 1.3 节为准。
import { ApiError, getSessionId, newRequestId, parseReply, request, requestBlob, transportError } from "./client"

export type FileItem = { fileId: string; fileName: string; fileSize: number; uploadTime: number }

export type ExcelSheet = {
  name: string
  totalRows: number
  colCount: number
  currentPage: number
  totalPages: number
  pageSize: number
  columns: string[]
  data: string[][]
}

export type ExcelPreview = {
  fileId: string
  fileName: string
  fileSize: number
  fileExt: string
  excelData: { sheets: ExcelSheet[] }
}

function requireId(value: unknown, message: string) {
  if (typeof value !== "string" || !value) throw new Error(message)
  return value
}

// fetch 没有上传进度事件，二进制上传使用 XMLHttpRequest，支持进度与取消。
function uploadBinary(
  path: string,
  query: Record<string, string>,
  file: File,
  onProgress?: (percent: number) => void,
  signal?: AbortSignal,
) {
  return new Promise<{ fileId?: string }>((resolve, reject) => {
    const requestId = newRequestId()
    const params = new URLSearchParams({ requestId, sessionId: getSessionId() ?? "", ...query })
    const xhr = new XMLHttpRequest()
    xhr.open("POST", `${path}?${params}`)
    xhr.setRequestHeader("Content-Type", "application/octet-stream")
    xhr.timeout = 300_000
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) onProgress?.(Math.round((event.loaded * 100) / event.total))
    }
    xhr.onerror = () => reject(transportError(null))
    xhr.ontimeout = () => reject(new ApiError("上传超时，请稍后重试。", "timeout"))
    xhr.onabort = () => reject(new ApiError("上传已取消。", "aborted"))
    xhr.onload = () => {
      const reply = new Response(xhr.responseText, { status: xhr.status })
      parseReply<{ fileId?: string }>(path, reply, requestId).then(resolve, reject)
    }
    signal?.addEventListener("abort", () => xhr.abort(), { once: true })
    xhr.send(file)
  })
}

export const files = {
  async list() {
    const data = await request<{ fileList: FileItem[] }>("/api/file/list")
    return data?.fileList ?? []
  },
  // 上传第一步：提交文件信息获取 fileId，fileExt 不带点号。
  async uploadInfo(fileName: string, fileSize: number, fileExt: string) {
    const data = await request<{ fileId: string }>("/api/file/upload/info", {
      body: { fileInfo: { fileName, fileSize, fileExt } },
    })
    return requireId(data?.fileId, "网关未返回文件 ID。")
  },
  // 上传第二步：发送 Excel 二进制数据。
  uploadExcel: (fileId: string, file: File, onProgress?: (percent: number) => void, signal?: AbortSignal) =>
    uploadBinary("/api/file/upload", { fileId }, file, onProgress, signal),
  async uploadSqlite(file: File, onProgress?: (percent: number) => void, signal?: AbortSignal) {
    const data = await uploadBinary("/api/file/sqlite/upload", { filename: file.name }, file, onProgress, signal)
    return requireId(data?.fileId, "网关未返回文件 ID。")
  },
  preview: (fileId: string, sheetName?: string, page?: number, pageSize?: number) =>
    request<ExcelPreview>("/api/file/preview", { body: { fileId, sheetName, page, pageSize } }),
  remove: (fileId: string) => request(`/api/file/${encodeURIComponent(fileId)}`, { method: "DELETE" }),
  download: (fileId: string) => requestBlob("/api/file/download", { fileId }),
  mapChat: (fileId: string, chatSessionId: string) =>
    request("/api/file/chat/map", { body: { fileId, chatSessionId } }),
}
