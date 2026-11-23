// Excel 上传状态机：校验 → 上传信息 → 上传文件 → 服务端解析 → 预览就绪。
// 上传成功后立即发起预览；支持取消和失败重试。
import { useCallback, useRef, useState } from "react"
import { ApiError } from "../api/client"
import { type ExcelPreview, files } from "../api/files"
import { excelExt, excelMaxBytes } from "../lib/constants"

export type UploadStage = "idle" | "validating" | "info" | "uploading" | "parsing" | "ready" | "error"

export const stageText: Record<UploadStage, string> = {
  idle: "",
  validating: "校验文件",
  info: "上传文件信息",
  uploading: "上传文件",
  parsing: "服务端解析",
  ready: "预览就绪",
  error: "上传失败",
}

export type UploadResult = { fileId: string; preview: ExcelPreview }

// 前端先校验格式和大小，不通过时给出明确原因。
export function validateExcel(file: File) {
  const ext = file.name.includes(".") ? file.name.split(".").pop()!.toLowerCase() : ""
  if (ext !== excelExt) return "仅支持 .xlsx 格式的 Excel 文件。"
  if (file.size > excelMaxBytes) return "文件超过 10 MB，请压缩或拆分后再上传。"
  if (file.size === 0) return "文件为空。"
  return ""
}

export function useUpload(onReady: (result: UploadResult, file: File) => void) {
  const [stage, setStage] = useState<UploadStage>("idle")
  const [percent, setPercent] = useState(0)
  const [error, setError] = useState<unknown>(null)
  const controller = useRef<AbortController | null>(null)
  const lastFile = useRef<File | null>(null)

  const start = useCallback(async (file: File) => {
    lastFile.current = file
    setError(null)
    setPercent(0)
    setStage("validating")
    const problem = validateExcel(file)
    if (problem) {
      setError(new ApiError(problem, "business"))
      setStage("error")
      return
    }
    const abort = new AbortController()
    controller.current = abort
    try {
      setStage("info")
      const fileId = await files.uploadInfo(file.name, file.size, excelExt)
      setStage("uploading")
      await files.uploadExcel(fileId, file, (value) => {
        setPercent(value)
        // 数据发送完毕后等待服务端解析响应。
        if (value >= 100) setStage("parsing")
      }, abort.signal)
      setStage("parsing")
      const preview = await files.preview(fileId)
      setStage("ready")
      onReady({ fileId, preview }, file)
    } catch (failure) {
      if (failure instanceof ApiError && failure.kind === "aborted") {
        setStage("idle")
        return
      }
      setError(failure)
      setStage("error")
    } finally {
      controller.current = null
    }
  }, [onReady])

  const cancel = useCallback(() => controller.current?.abort(), [])
  const retry = useCallback(() => {
    if (lastFile.current) void start(lastFile.current)
  }, [start])
  const reset = useCallback(() => {
    controller.current?.abort()
    setStage("idle")
    setError(null)
    setPercent(0)
  }, [])

  const active = stage === "validating" || stage === "info" || stage === "uploading" || stage === "parsing"
  return { stage, percent, error, active, start, cancel, retry, reset }
}
