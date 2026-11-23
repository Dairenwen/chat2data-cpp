// 我的文件：展示当前用户上传过的所有文件（Excel 与 SQLite）。
import { FileTable } from "../components/FileTable"

export function FilesPage() {
  return (
    <div className="h-full overflow-y-auto">
      <div className="mx-auto grid max-w-5xl gap-4 p-4 sm:p-6">
        <h1 className="text-2xl">我的文件</h1>
        <FileTable />
      </div>
    </div>
  )
}
