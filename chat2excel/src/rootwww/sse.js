/**
 * 按 SSE 的行与空行分帧，不能把一次 reader.read() 当成一个完整事件。
 * 支持 LF / CRLF / CR、注释、多行 data，以及分块边界恰好落在 CRLF 中间的情况。
 * UTF-8 字节的增量解码由调用方的 TextDecoder(stream: true) 完成。
 */
export class SSEParser {
  constructor(onEvent) {
    this.onEvent = onEvent;
    this.buffer = "";
    this.data = [];
    this.event = "message";
    this.firstChunk = true;
  }

  feed(text) {
    if (this.firstChunk && text.length > 0) {
      text = text.replace(/^\uFEFF/, "");
      this.firstChunk = false;
    }
    this.buffer += text;
    let start = 0;
    for (let i = 0; i < this.buffer.length; i += 1) {
      const character = this.buffer[i];
      if (character !== "\r" && character !== "\n") continue;
      // 等待下一块，才能判断尾部 CR 是否还跟着 LF。
      if (character === "\r" && i + 1 === this.buffer.length) break;
      this.readLine(this.buffer.slice(start, i));
      if (character === "\r" && this.buffer[i + 1] === "\n") i += 1;
      start = i + 1;
    }
    this.buffer = this.buffer.slice(start);
  }

  readLine(line) {
    if (line === "") {
      if (this.data.length > 0) this.onEvent({ event: this.event, data: this.data.join("\n") });
      this.data = [];
      this.event = "message";
      return;
    }
    if (line.startsWith(":")) return;
    const separator = line.indexOf(":");
    const field = separator < 0 ? line : line.slice(0, separator);
    let value = separator < 0 ? "" : line.slice(separator + 1);
    if (value.startsWith(" ")) value = value.slice(1);
    if (field === "data") this.data.push(value);
    else if (field === "event") this.event = value;
  }

  finish() {
    // 最后一个 CR 也是完整行结束符；没有空行终止的事件按 SSE 规范丢弃。
    if (this.buffer.endsWith("\r")) this.readLine(this.buffer.slice(0, -1));
    this.buffer = "";
    this.data = [];
    this.event = "message";
  }
}

/** 后端当前把片段编码为 JSON 字符串，同时兼容文档中的纯文本 data。 */
export function decodeSSEData(data) {
  try {
    const decoded = JSON.parse(data);
    return typeof decoded === "string" ? decoded : data;
  } catch {
    return data;
  }
}
