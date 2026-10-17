import { SSEParser, decodeSSEData } from "./sse.js";

// 页面由 Chat2ExcelServer 提供，API 使用同源地址，避免写死 IP 和跨域请求。
// 在文档指定的服务器部署后，入口为 http://192.168.112.128:9000/。
const MAX_CHARACTERS = 2000;
const $ = (id) => document.getElementById(id);
const fileInput = $("file-input");
const chooseButton = $("choose-file");
const messageInput = $("message-input");
const sendButton = $("send-button");
const conversation = $("conversation");
const hint = $("composer-hint");
const state = { fileId: "", fileName: "", uploading: false, streaming: false, composing: false, controller: null };
const timeFormatter = new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit", hour12: false });
const markdownReady = Boolean(window.marked && window.DOMPurify && window.hljs);

function announce(text) {
  $("announcer").textContent = text;
}

function setHint(text, error = false) {
  hint.textContent = text;
  hint.dataset.state = error ? "error" : "normal";
}

function setUploadStatus(text, status = "normal") {
  $("upload-status").textContent = text;
  $("upload-status").dataset.state = status;
}

/** 按 Unicode 码点计数，截断时不切开 emoji 的 UTF-16 代理对。 */
function updateComposer(enforceLimit = true) {
  let characters = Array.from(messageInput.value);
  if (enforceLimit && !state.composing && characters.length > MAX_CHARACTERS) {
    const caret = messageInput.selectionStart;
    messageInput.value = characters.slice(0, MAX_CHARACTERS).join("");
    messageInput.setSelectionRange(Math.min(caret, messageInput.value.length), Math.min(caret, messageInput.value.length));
    characters = Array.from(messageInput.value);
    setHint("最多可输入 2000 字，超出的内容未保留", true);
  }
  $("character-count").textContent = `${characters.length}/${MAX_CHARACTERS}`;
  $("character-count").dataset.limit = String(characters.length >= MAX_CHARACTERS);
  chooseButton.disabled = state.uploading || state.streaming;
  sendButton.disabled = !state.streaming && (!markdownReady || state.uploading || !state.fileId || !messageInput.value.trim() || characters.length > MAX_CHARACTERS || state.composing);
  sendButton.querySelector("span").textContent = state.streaming ? "停止" : "发送";
  sendButton.querySelector("img").src = state.streaming ? "icons/square.svg" : "icons/arrow-up.svg";
  sendButton.setAttribute("aria-label", state.streaming ? "停止生成" : "发送消息");
}

/** HTTP 错误发生在 SSE 建立之前，以后端 JSON 中的错误说明为准。 */
async function responseError(response) {
  try {
    const body = await response.json();
    if (typeof body.error === "string") return body.error;
    if (typeof body.message === "string") return body.message;
  } catch { /* 非 JSON 错误页仍应给用户可读的状态说明。 */ }
  return `请求失败（HTTP ${response.status}），请稍后重试`;
}

async function checkHealth() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch("/api/health", { signal: controller.signal, cache: "no-store" });
    const body = response.ok ? await response.json() : null;
    if (body?.status !== "healthy") throw new Error("unhealthy");
    $("connection-status").dataset.state = "healthy";
    $("connection-label").textContent = "服务已连接";
  } catch {
    $("connection-status").dataset.state = "offline";
    $("connection-label").textContent = "服务未连接";
  } finally {
    clearTimeout(timeout);
  }
}

async function uploadFile() {
  const file = fileInput.files[0];
  if (!file) return;
  // 在清空选择器前保存路径；现代浏览器一般提供 C:\fakepath\文件名。
  const displayPath = fileInput.value || file.name;
  fileInput.value = ""; // 允许再次选择同一文件，重试上传。
  if (state.streaming || state.uploading) return;
  if (!/\.xlsx$/i.test(file.name)) {
    setUploadStatus("仅支持 .xlsx 格式，请重新选择 Excel 文件", "error");
    return;
  }
  if (file.size === 0) {
    setUploadStatus("文件为空，请选择有效的 Excel 文件", "error");
    return;
  }

  // 新文件上传失败时，不能把它与上一份文件的 fileId 混用。
  state.fileId = "";
  state.fileName = file.name;
  state.uploading = true;
  $("file-path").value = displayPath;
  setUploadStatus(`正在上传 ${file.name}…`);
  setHint("正在上传文件，请稍候");
  updateComposer();
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 120000);
  try {
    const query = new URLSearchParams({ fileName: file.name });
    const response = await fetch(`/api/uploadFile?${query}`, {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: file,
      signal: controller.signal,
    });
    if (!response.ok) throw new Error(await responseError(response));
    const body = await response.json();
    if (body.success !== true || typeof body.result?.fileId !== "string" || !body.result.fileId.trim()) {
      throw new Error("服务器未返回有效的文件 ID，请重新上传");
    }
    state.fileId = body.result.fileId;
    setUploadStatus(`${file.name} · 上传成功`, "success");
    setHint("文件已就绪，输入问题开始分析");
    announce("Excel 文件上传成功，可以开始提问");
    messageInput.focus();
  } catch (error) {
    setUploadStatus(error.name === "AbortError" ? "上传超时，请重新选择文件重试" : `上传失败：${error.message}`, "error");
    setHint("请重新上传 Excel 文件", true);
  } finally {
    clearTimeout(timeout);
    state.uploading = false;
    updateComposer();
    void checkHealth();
  }
}

function nearBottom() {
  return conversation.scrollHeight - conversation.scrollTop - conversation.clientHeight < 90;
}

function scrollToBottom() {
  conversation.scrollTop = conversation.scrollHeight;
  $("scroll-bottom").hidden = true;
}

function refreshScrollButton() {
  $("scroll-bottom").hidden = nearBottom();
}

function createMessage(role, fileName) {
  $("empty-state").hidden = true;
  const article = document.createElement("article");
  article.className = `message ${role}`;
  const label = document.createElement("div");
  label.className = "message-label";
  const author = document.createElement("strong");
  author.textContent = role === "user" ? "你" : "Chat2Excel";
  label.append(author);
  const file = document.createElement("span");
  file.textContent = fileName;
  label.append(file);
  const bubble = document.createElement("div");
  bubble.className = "message-bubble";
  const time = document.createElement("time");
  const now = new Date();
  time.className = "message-time";
  time.dateTime = now.toISOString();
  time.textContent = timeFormatter.format(now);
  time.title = now.toLocaleString("zh-CN");
  article.append(label, bubble, time);
  $("messages").append(article);
  return { article, bubble, time, content: "" };
}

/**
 * 模型输出也是不可信内容。先解析 Markdown，再清洗 HTML；语言名和复制按钮
 * 通过 DOM API 创建，不把文件名、语言名或代码拼入 HTML 属性。
 */
function renderMarkdown(message, finished = false) {
  const shouldFollow = nearBottom();
  let fragment;
  try {
    fragment = DOMPurify.sanitize(marked.parse(message.content, { gfm: true, breaks: true }), {
      USE_PROFILES: { html: true },
      FORBID_TAGS: ["style", "form", "input", "button", "textarea", "select", "img", "video", "audio"],
      FORBID_ATTR: ["style", "id", "name"],
      RETURN_DOM_FRAGMENT: true,
    });
  } catch {
    // 特殊输入触发 Markdown 解析异常时保留原文，不让渲染错误卡住发送状态。
    message.bubble.textContent = message.content;
    message.bubble.classList.add("plain-text");
    if (shouldFollow) scrollToBottom();
    else refreshScrollButton();
    return;
  }
  message.bubble.classList.remove("plain-text");
  fragment.querySelectorAll("a").forEach((link) => {
    link.target = "_blank";
    link.rel = "noopener noreferrer";
  });
  fragment.querySelectorAll("pre").forEach((pre) => {
    const code = pre.querySelector("code");
    if (!code) return;
    const language = Array.from(code.classList).find((name) => name.startsWith("language-"))?.slice(9) || "text";
    // 大代码块在生成结束时高亮，避免每个片段都进行昂贵的语法分析。
    // 不自动猜测语言，防止模型输出很长的未知代码时重复遍历所有语法。
    if ((finished || code.textContent.length < 12000) && hljs.getLanguage(language)) {
      try {
        code.innerHTML = hljs.highlight(code.textContent, { language, ignoreIllegals: true }).value;
        code.classList.add("hljs");
      } catch { /* 某种语法高亮失败时仍显示可复制的原始代码。 */ }
    }
    const block = document.createElement("div");
    block.className = "code-block";
    const toolbar = document.createElement("div");
    toolbar.className = "code-toolbar";
    const caption = document.createElement("span");
    caption.textContent = language;
    const copy = document.createElement("button");
    copy.type = "button";
    copy.className = "copy-code";
    copy.setAttribute("aria-label", "复制代码");
    const icon = document.createElement("img");
    icon.src = "icons/copy.svg";
    icon.alt = "";
    icon.width = 13;
    icon.height = 13;
    const text = document.createElement("span");
    text.textContent = "复制代码";
    copy.append(icon, text);
    toolbar.append(caption, copy);
    pre.replaceWith(block);
    block.append(toolbar, pre);
  });
  fragment.querySelectorAll("table").forEach((table) => {
    const wrapper = document.createElement("div");
    wrapper.className = "table-scroll";
    wrapper.tabIndex = 0;
    wrapper.setAttribute("role", "region");
    wrapper.setAttribute("aria-label", "数据表格，可横向滚动");
    table.replaceWith(wrapper);
    wrapper.append(table);
  });
  message.bubble.replaceChildren(fragment);
  if (shouldFollow) scrollToBottom();
  else refreshScrollButton();
}

function addMessageNotice(message, text, status) {
  message.article.dataset.state = status;
  const notice = document.createElement("p");
  notice.className = "message-notice";
  notice.textContent = text;
  message.article.append(notice);
  announce(text);
}

async function sendMessage(event) {
  event?.preventDefault();
  if (state.streaming) {
    state.controller?.abort();
    return;
  }
  const question = messageInput.value.trim();
  if (state.composing || state.uploading || !state.fileId || !question || Array.from(question).length > MAX_CHARACTERS || !markdownReady) return;

  const user = createMessage("user", state.fileName);
  user.bubble.textContent = question;
  const assistant = createMessage("assistant", state.fileName);
  assistant.article.setAttribute("aria-busy", "true");
  messageInput.value = "";
  state.streaming = true;
  state.controller = new AbortController();
  const controller = state.controller;
  updateComposer();
  setHint("正在分析，可以随时停止生成");
  scrollToBottom();

  let reader;
  let renderTimer = null;
  let completed = false;
  let receivedContent = false;
  const scheduleRender = () => {
    if (renderTimer !== null) return;
    // 合并密集片段；长回复降低刷新频率，避免不断重建 Markdown DOM。
    renderTimer = setTimeout(() => {
      renderTimer = null;
      renderMarkdown(assistant);
    }, assistant.content.length > 60000 ? 250 : 80);
  };

  try {
    const response = await fetch("/api/sendMessage", {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "text/event-stream" },
      body: JSON.stringify({ fileId: state.fileId, message: question }),
      signal: controller.signal,
    });
    if (!response.ok) {
      // 服务器重启会丢失内存中的上传映射，应引导重新上传而不是继续使用旧 ID。
      if (response.status === 404) {
        state.fileId = "";
        setUploadStatus("文件记录已失效，请重新上传 Excel 文件", "error");
      }
      throw new Error(await responseError(response));
    }
    if (!response.headers.get("Content-Type")?.toLowerCase().includes("text/event-stream") || !response.body) {
      throw new Error("服务器未返回有效的流式分析结果");
    }

    const parser = new SSEParser(({ event: type, data }) => {
      if (completed) return;
      if (type === "error") throw new Error(decodeSSEData(data) || "模型分析失败，请重试");
      if (data === "[DONE]") {
        completed = true;
        return;
      }
      if (type !== "message") return;
      const text = decodeSSEData(data);
      assistant.content += text;
      receivedContent ||= text.length > 0;
      scheduleRender();
    });
    // stream:true 保留跨网络分块的多字节字符，避免中文和 emoji 被替换成乱码。
    const decoder = new TextDecoder("utf-8");
    reader = response.body.getReader();
    while (!completed) {
      const { value, done } = await reader.read();
      if (done) {
        parser.feed(decoder.decode());
        parser.finish();
        break;
      }
      parser.feed(decoder.decode(value, { stream: true }));
    }
    if (!completed) throw new Error("连接中断，分析结果可能不完整，请重新发送问题");
    if (!receivedContent) throw new Error("模型未返回分析内容，请重新发送问题");
    setHint("分析完成，可以继续提问");
    announce("分析结果已生成");
  } catch (error) {
    const stopped = controller.signal.aborted;
    addMessageNotice(assistant, stopped ? "已停止生成，已接收的内容已保留" : error.message, stopped ? "stopped" : "error");
    setHint(stopped ? "已停止生成，可以继续提问" : error.message, !stopped);
  } finally {
    if (renderTimer !== null) clearTimeout(renderTimer);
    // 停止、异常和完成时都绘制最后收到的片段，不丢失最后一帧。
    renderMarkdown(assistant, true);
    assistant.article.setAttribute("aria-busy", "false");
    const now = new Date();
    assistant.time.dateTime = now.toISOString();
    assistant.time.textContent = timeFormatter.format(now);
    assistant.time.title = now.toLocaleString("zh-CN");
    if (reader) {
      try { await reader.cancel(); } catch { /* 已关闭或已中断的连接无需再次报告错误。 */ }
      reader.releaseLock();
    }
    state.streaming = false;
    state.controller = null;
    updateComposer();
    messageInput.focus();
  }
}

/** HTTP 私网地址通常不是安全上下文，Clipboard API 不可用时使用选择区复制。 */
async function copyCode(button) {
  const code = button.closest(".code-block").querySelector("code").textContent;
  const focused = document.activeElement;
  try {
    if (window.isSecureContext && navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(code);
    } else {
      const temporary = document.createElement("textarea");
      temporary.value = code;
      temporary.className = "sr-only";
      temporary.setAttribute("readonly", "");
      document.body.append(temporary);
      temporary.select();
      let copied;
      try { copied = document.execCommand("copy"); }
      finally { temporary.remove(); focused?.focus({ preventScroll: true }); }
      if (!copied) throw new Error("copy failed");
    }
    button.querySelector("span").textContent = "已复制";
    button.querySelector("img").src = "icons/check.svg";
    announce("代码已复制");
    setTimeout(() => {
      button.querySelector("span").textContent = "复制代码";
      button.querySelector("img").src = "icons/copy.svg";
    }, 1800);
  } catch {
    button.querySelector("span").textContent = "请手动选择复制";
    announce("复制失败，请选择代码并手动复制");
  }
}

chooseButton.addEventListener("click", () => fileInput.click());
fileInput.addEventListener("change", () => void uploadFile());
$("message-form").addEventListener("submit", (event) => void sendMessage(event));
messageInput.addEventListener("input", () => updateComposer());
messageInput.addEventListener("compositionstart", () => { state.composing = true; updateComposer(false); });
messageInput.addEventListener("compositionend", () => { state.composing = false; updateComposer(); });
messageInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey && !event.isComposing && !state.composing && event.keyCode !== 229) {
    event.preventDefault();
    // 生成过程中 Enter 不能误触停止，也不能并发提交第二个请求。
    if (!state.streaming) void sendMessage();
  }
});
$("messages").addEventListener("click", (event) => {
  const button = event.target.closest(".copy-code");
  if (button) void copyCode(button);
});
document.querySelectorAll("[data-question]").forEach((button) => {
  button.addEventListener("click", () => {
    messageInput.value = button.dataset.question;
    updateComposer();
    messageInput.focus();
    if (!state.fileId) setHint("问题已填入，请先上传 Excel 文件");
  });
});
conversation.addEventListener("scroll", refreshScrollButton, { passive: true });
$("scroll-bottom").addEventListener("click", scrollToBottom);
window.addEventListener("online", () => void checkHealth());
document.addEventListener("visibilitychange", () => { if (!document.hidden) void checkHealth(); });
if (!markdownReady) setHint("页面组件加载失败，请刷新页面后重试", true);
updateComposer();
void checkHealth();
