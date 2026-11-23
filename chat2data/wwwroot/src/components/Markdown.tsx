// Markdown 渲染：Streamdown 专为流式设计，能处理未写完的块，内置 GFM 表格与 Shiki 代码高亮和复制按钮。
// 模型输出一律经 Markdown 解析渲染，不使用 dangerouslySetInnerHTML。
import { Streamdown } from "streamdown"

export function Markdown({ children }: { children: string }) {
  return (
    <div className="prose-chat min-w-0 max-w-full break-words text-sm leading-relaxed [&_pre]:max-w-full [&_table]:block [&_table]:overflow-x-auto">
      <Streamdown>{children}</Streamdown>
    </div>
  )
}
