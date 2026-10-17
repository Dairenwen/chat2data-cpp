#include "Chat2ExcelServer.h"
#include "ExeclParser.h"

#include <ChatSDK.h>
#include <util/myLog.h>
// 只使用参考 SDK 的日志器声明，清除其宏，应用代码统一使用 bite_scaffold 的日志宏。
#undef TRACE
#undef DBG
#undef INFO
#undef WARN
#undef ERR
#undef CRIT
#include <bite_scaffold/log.h>
#include <bite_scaffold/util.h>
#include <fmt/format.h>
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

/**
 * @brief 将参考 ChatSDK 的内部日志接到 bite_scaffold，避免引入另一套日志初始化和文件。
 * @return 应用日志器；调用方尚未初始化日志时使用 spdlog 默认日志器。
 */
std::shared_ptr<spdlog::logger> bite::Logger::getLogger()
{
    return bitelog::g_logger ? bitelog::g_logger : spdlog::default_logger();
}

namespace
{

    // SDK 的流式回调没有取消返回值；写入失败时用该异常退出上游请求调用栈。
    class ClientDisconnected final : public std::exception
    {
    public:
        /** @return 固定的断连原因，不分配内存且不抛出异常。 */
        const char *what() const noexcept override { return "SSE 客户端已断开或服务器正在停止"; }
    };

    /**
     * @brief 比较 HTTP 媒体类型，忽略大小写、首尾空白及分号后的参数。
     * @param request 提供 Content-Type 请求头的 HTTP 请求。
     * @param expectedType 小写、不带参数的期望媒体类型。
     * @return 请求媒体类型匹配时返回 true，缺失或不匹配返回 false。
     * @throws std::exception 字符串分配失败等标准库异常。
     */
    bool hasContentType(const httplib::Request &request, const std::string &expectedType)
    {
        auto contentType = request.get_header_value("Content-Type");
        const auto parameterSeparator = contentType.find(';');
        if (parameterSeparator != std::string::npos)
            contentType.resize(parameterSeparator);
        const auto first = contentType.find_first_not_of(" \t");
        const auto last = contentType.find_last_not_of(" \t");
        if (first == std::string::npos)
            return false;
        contentType = contentType.substr(first, last - first + 1);
        std::transform(contentType.begin(), contentType.end(), contentType.begin(), [](unsigned char character)
                       { return static_cast<char>(std::tolower(character)); });
        return contentType == expectedType;
    }

    /**
     * @brief 使用脚手架 JSON 工具返回统一的 HTTP 接口错误响应。
     * @param response 待填充的 HTTP 响应。
     * @param status HTTP 错误状态码。
     * @param message 面向调用方的错误说明。
     * @throws std::exception 响应构造或字符串分配失败。
     */
    void setJsonError(httplib::Response &response, int status, const std::string &message)
    {
        Json::Value body;
        body["success"] = false;
        body["error"] = message;
        const auto payload = biteutil::JSON::serialize(body);
        response.status = payload ? status : 500;
        response.set_content(payload.value_or(R"({"success":false,"error":"Response serialization failed"})"), "application/json; charset=utf-8");
    }

} // namespace

Chat2ExcelServer::Chat2ExcelServer(const std::string &host, int port)
    : host(host), port(port)
{
    if (host.empty())
    {
        throw std::invalid_argument("服务器监听地址不能为空");
    }
    if (port < 1 || port > 65535)
    {
        throw std::invalid_argument("服务器端口必须在 1～65535 之间");
    }

    const char *apiKey = std::getenv("DEEPSEEK_API_KEY");
    if (!apiKey || !*apiKey)
        apiKey = std::getenv("deepseek_apikey");
    if (!apiKey || !*apiKey)
    {
        if (bitelog::g_logger)
            WRN("未配置 DEEPSEEK_API_KEY，消息接口暂不可用");
        return;
    }

    auto modelConfig = std::make_shared<ai_chat_sdk::APIConfig>();
    modelConfig->_modelName = "deepseek-chat";
    modelConfig->_apiKey = apiKey;
    modelConfig->_temperature = 0.7;
    modelConfig->_maxTokens = 2048;
    chatSdk = std::make_unique<ai_chat_sdk::ChatSDK>();
    if (!chatSdk->initModels({modelConfig}))
    {
        throw std::runtime_error("ChatSDK 模型初始化失败");
    }
    // 参考 SDK 的 initModels() 即使某个模型失败也可能返回 true，再检查实际可用模型。
    const auto models = chatSdk->getAvailableModels();
    if (std::none_of(models.begin(), models.end(), [](const ai_chat_sdk::ModelInfo &model)
                     { return model._modelName == "deepseek-chat" && model._isAvailable; }))
    {
        throw std::runtime_error("deepseek-chat 模型未就绪");
    }
    if (bitelog::g_logger)
        INF("ChatSDK 初始化完成，分析模型：deepseek-chat");
}

Chat2ExcelServer::~Chat2ExcelServer()
{
    // 保留后台线程所有权，确保其访问的 this 和 httpServer 存活到线程退出。
    if (serverThread.joinable())
    {
        stop();
    }
}

bool Chat2ExcelServer::start()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    if (running.load() && httpServer && httpServer->is_running())
    {
        if (bitelog::g_logger)
        {
            WRN("服务器已在运行，地址：{}，端口：{}", host, port);
        }
        return false;
    }

    // 异常退出后的线程可能已经结束但仍可 join，重启前先回收旧线程。
    if (serverThread.joinable())
    {
        serverThread.join();
    }
    // 每次启动使用新实例，使停止后重启不会重复绑定路由或继承旧监听状态。
    httpServer = std::make_unique<httplib::Server>();
    httpServer->set_socket_options([](socket_t socket)
                                   {
        httplib::default_socket_options(socket);
        // 当前 httplib 默认启用 SO_REUSEPORT；关闭后才能明确发现另一实例占用端口。
#ifdef SO_REUSEPORT
        const int reusePort = 0;
        setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reusePort), sizeof(reusePort));
#endif
        // 保留 SO_REUSEADDR，允许关闭后重新绑定仍有 TIME_WAIT 连接的地址。
        const int reuseAddress = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress)); });
    setRoutes();
    // 使用构建时确定的目录，同源提供网页和 API，不依赖启动时的工作目录。
    if (!httpServer->set_mount_point("/", CHAT2EXCEL_WEB_DIRECTORY))
    {
        if (bitelog::g_logger)
            ERR("网页目录不存在或无法访问：{}", CHAT2EXCEL_WEB_DIRECTORY);
        return false;
    }
    running.store(true);
    threadFinished.store(false);

    try
    {
        serverThread = std::thread([this]()
                                   {
            try {
                if (!httpServer->listen(host, port) && bitelog::g_logger) {
                    ERR("HTTP 监听失败，请检查地址或端口占用，地址：{}，端口：{}", host, port);
                }
            } catch (const std::exception& error) {
                if (bitelog::g_logger) {
                    ERR("HTTP 后台线程异常：{}", error.what());
                }
            } catch (...) {
                if (bitelog::g_logger) {
                    ERR("HTTP 后台线程发生未知异常");
                }
            }
            running.store(false);
            threadFinished.store(true); });
    }
    catch (...)
    {
        running.store(false);
        threadFinished.store(true);
        throw;
    }

    // listen() 可能因为地址无效或端口冲突立即返回；不能仅凭线程创建成功
    // 就声称服务器已经运行。完成标记同时避免后台抛出异常时一直等待。
    while (!httpServer->is_running() && !threadFinished.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!running.load() || !httpServer->is_running())
    {
        serverThread.join();
        running.store(false);
        return false;
    }

    if (bitelog::g_logger)
    {
        INF("Chat2ExcelServer 启动成功，地址：{}，端口：{}，健康检查：/health", host, port);
    }
    return true;
}

void Chat2ExcelServer::stop()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const bool wasRunning = running.load() && httpServer && httpServer->is_running();
    if (!wasRunning && bitelog::g_logger)
    {
        WRN("服务器未运行，无需再次停止，地址：{}，端口：{}", host, port);
    }

    // 先通知 SSE 回调停止，避免 join 等待工作线程时仍继续向模型请求数据。
    running.store(false);
    if (httpServer && httpServer->is_running())
    {
        httpServer->stop();
    }
    // httplib 会等待其请求工作线程退出；join 后才可以释放服务器实例。
    if (serverThread.joinable())
    {
        serverThread.join();
    }
    running.store(false);
    if (wasRunning && bitelog::g_logger)
    {
        INF("Chat2ExcelServer 已停止，地址：{}，端口：{}", host, port);
    }
}

bool Chat2ExcelServer::isRunning() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return running.load() && httpServer && httpServer->is_running();
}

std::string Chat2ExcelServer::parseExcelFile(const std::string &fileId)
{
    try
    {
        if (fileId.empty())
        {
            throw std::invalid_argument("文件 ID 不能为空");
        }

        std::string filePath;
        {
            // 上传请求会修改映射；持锁复制路径后立即释放，避免耗时的 Excel 解析阻塞上传。
            std::lock_guard<std::mutex> lock(fileMapMutex);
            const auto fileIterator = filePathMap.find(fileId);
            if (fileIterator == filePathMap.end())
            {
                throw std::out_of_range("未找到文件 ID 对应的上传文件：" + fileId);
            }
            filePath = fileIterator->second;
        }

        std::string result;
        const auto fileSize = std::filesystem::file_size(filePath);
        if (fileSize > result.max_size())
        {
            throw std::length_error("Excel 文件过大，无法预留解析结果空间");
        }
        // .xlsx 是压缩文件，磁盘大小只作为初始容量估计，不限制解压后文本的长度。
        result.reserve(static_cast<std::string::size_type>(fileSize));

        const auto worksheetNames = ExcelParser::getWorksheetNames(filePath);
        for (std::size_t worksheetIndex = 0; worksheetIndex < worksheetNames.size(); ++worksheetIndex)
        {
            const auto &worksheetName = worksheetNames[worksheetIndex];
            const auto worksheetData = ExcelParser::parseWorksheet(filePath, worksheetName);
            if (worksheetIndex > 0)
            {
                result.push_back('\n');
            }
            result += "worksheet";
            result += std::to_string(worksheetIndex + 1);
            result += ": ";
            result += worksheetName;
            if (!worksheetData.empty())
            {
                result.push_back('\n');
                // 直接追加现有 CSV，不能再按逗号或换行拆分，否则会破坏转义及单元格内换行。
                result += worksheetData;
            }
        }

        if (bitelog::g_logger)
        {
            INF("解析上传 Excel 成功，fileId：{}，工作表数量：{}，结果字节数：{}",
                fileId, worksheetNames.size(), result.size());
        }
        return result;
    }
    catch (const std::exception &error)
    {
        if (bitelog::g_logger)
        {
            ERR("解析上传 Excel 失败，fileId：{}，原因：{}", fileId, error.what());
        }
        throw;
    }
}

std::string Chat2ExcelServer::buildPrompt(const std::string &fileName, const std::string &excelData, const std::string &userQuestion) const
{
    static constexpr char promptTemplate[] = R"(## 角色定义
你是一个专业的Excel数据分析专家，能够根据提供的Excel数据，直接进行数据分析并给出具体的分析结果。

## 输出要求(必须严格遵守)
回答必须包含以下结构:

### 标题
用一句话(10-20字)提炼本次分析的核心结论或主题

### 分析结果
根据用户问题，直接展示数据分析得到的具体数值、统计结果、对比数据等。要求:
- 以表格或文本形式呈现主要数据结果
- 数据要具体、准确，基于提供的Excel内容
- 如有排名，给出具体排名列表
- 如有对比，明确标注差异和特点

### 数据洞察
从分析结果中提炼3-5个关键发现或规律，例如:
- 最高/最低表现的专业或班级
- 成绩分布的特点
- 异常或值得关注的数据点

### 总结与建议
- 综合概括本次分析的核心结论
- 基于数据提出1-2条具体可行的建议

## 重要约束
- 直接输出分析结果，以“已经为您完成分析，结果如下：”开头
- 所有数据必须从提供的Excel内容中提取计算
- 回答必须使用中文

## 数据上下文
excel文件名:{filename}

excel文件内容:{excelData}

## 用户问题
用户输入:{userQuestion})";

    try
    {
        // 使用已有的 fmt 依赖一次替换模板中的命名占位符；参数只作为文本插入，
        // 不会继续替换 Excel 内容或用户问题中碰巧出现的同名占位符及其他花括号。
        auto prompt = fmt::format(fmt::runtime(promptTemplate), fmt::arg("filename", fileName), fmt::arg("excelData", excelData), fmt::arg("userQuestion", userQuestion));
        if (bitelog::g_logger)
        {
            INF("构建 Excel 分析提示词成功，文件名：{}，Excel 数据字节数：{}，用户问题字节数：{}，提示词字节数：{}",
                fileName, excelData.size(), userQuestion.size(), prompt.size());
        }
        return prompt;
    }
    catch (const std::exception &error)
    {
        if (bitelog::g_logger)
        {
            ERR("构建 Excel 分析提示词失败，文件名：{}，原因：{}", fileName, error.what());
        }
        throw;
    }
}

void Chat2ExcelServer::setRoutes()
{
    const auto healthHandler = [](const httplib::Request &, httplib::Response &response)
                    {
        // 请求能进入此处理函数说明 HTTP 服务仍在处理请求。
        // 处理函数不获取生命周期锁，保证停止时可等待请求正常结束。
        response.status = 200;
        response.set_content(R"({"status": "healthy"})", "application/json; charset=utf-8"); };
    // 保留已有健康检查地址，同时提供 frontend.md 指定的 API 路径。
    httpServer->Get("/health", healthHandler);
    httpServer->Get("/api/health", healthHandler);
    httpServer->Post("/api/uploadFile", [this](const httplib::Request &request, httplib::Response &response)
                     { handleUploadFile(request, response); });
    httpServer->Post("/api/sendMessage", [this](const httplib::Request &request, httplib::Response &response)
                     { sendMessage(request, response); });
}

void Chat2ExcelServer::sendMessage(const httplib::Request &request, httplib::Response &response)
{
    try
    {
        if (!hasContentType(request, "application/json"))
        {
            setJsonError(response, 415, "Content-Type 必须为 application/json");
            return;
        }
        const auto body = biteutil::JSON::unserialize(request.body);
        if (!body || !body->isObject() || !(*body)["fileId"].isString() || !(*body)["message"].isString())
        {
            setJsonError(response, 400, "请求必须是包含字符串 fileId 和 message 的 JSON 对象");
            return;
        }
        const auto fileId = (*body)["fileId"].asString();
        const auto message = (*body)["message"].asString();
        const auto isBlank = [](const std::string &text)
        {
            return std::all_of(text.begin(), text.end(), [](unsigned char character)
                               { return std::isspace(character); });
        };
        if (isBlank(fileId) || isBlank(message))
        {
            setJsonError(response, 400, "fileId 和 message 不能为空或全为空白字符");
            return;
        }

        std::string fileName;
        {
            std::lock_guard<std::mutex> lock(fileMapMutex);
            const auto file = filePathMap.find(fileId);
            if (file == filePathMap.end())
            {
                setJsonError(response, 404, "未找到 fileId 对应的上传文件，请重新上传");
                return;
            }
            fileName = std::filesystem::path(file->second).filename().string();
        }
        std::string excelData;
        try
        {
            excelData = parseExcelFile(fileId);
        }
        catch (const std::exception &error)
        {
            if (bitelog::g_logger)
                ERR("消息请求解析 Excel 失败，fileId：{}，原因：{}", fileId, error.what());
            setJsonError(response, 422, "Excel 文件无法解析，请检查文件内容后重新上传");
            return;
        }
        auto prompt = buildPrompt(fileName, excelData, message);
        if (!chatSdk)
        {
            setJsonError(response, 503, "尚未配置 DEEPSEEK_API_KEY，请配置后重启服务器");
            return;
        }

        response.status = 200;
        response.set_header("Cache-Control", "no-cache");
        response.set_header("Connection", "keep-alive");
        // httplib 自动设置 Transfer-Encoding: chunked；捕获值而非 request 的引用
        response.set_chunked_content_provider("text/event-stream; charset=utf-8",
                                              [this, fileId, prompt = std::move(prompt)](std::size_t, httplib::DataSink &sink) -> bool
                                              {
                                                  std::unique_lock<std::mutex> sdkLock(chatSdkMutex, std::defer_lock);
                                                  std::string sessionId;
                                                  bool connected = true;
                                                  // 负责实际写入
                                                  const auto writeFrame = [&](const std::string &frame)
                                                  {
                                                      if (!running.load() || !httpServer->is_running() || (sink.is_writable && !sink.is_writable()) ||
                                                          !sink.write(frame.data(), frame.size()))
                                                          throw ClientDisconnected();
                                                  };
                                                  // 负责组织 SSE 格式
                                                  const auto writeStringEvent = [&](const std::string &text, bool isError = false)
                                                  {
                                                      const auto json = biteutil::JSON::serialize(Json::Value(text));
                                                      if (!json)
                                                          throw std::runtime_error("SSE 消息序列化失败");
                                                      // JSON 转义文本内部的换行、引号和控制字符，保证每帧只由末尾的双换行分隔。
                                                      writeFrame(std::string(isError ? "event: error\n" : "") + "data: " + *json + "\n\n");
                                                  };
                                                  try
                                                  {
                                                      writeStringEvent("");
                                                      // 独立保护参考 SDK 的会话生命周期；上传、健康检查使用其他工作线程继续处理。
                                                      sdkLock.lock();
                                                      if (!running.load() || (sink.is_writable && !sink.is_writable()))
                                                          throw ClientDisconnected();
                                                      sessionId = chatSdk->createSession("deepseek-chat");
                                                      if (sessionId.empty())
                                                          throw std::runtime_error("模型会话创建失败");
                                                      bool modelFinished = false;
                                                      bool receivedContent = false;
                                                      const auto fullResponse = chatSdk->sendMessageStream(sessionId, prompt,
                                                                                                           [&](const std::string &chunk, bool last)
                                                                                                           {
                                                                                                               if (modelFinished)
                                                                                                                   return;
                                                                                                               if (!chunk.empty())
                                                                                                               {
                                                                                                                   writeStringEvent(chunk);
                                                                                                                   receivedContent = true;
                                                                                                               }
                                                                                                               if (last)
                                                                                                                   modelFinished = true;
                                                                                                           });
                                                      if (fullResponse.empty() || !modelFinished)
                                                          throw std::runtime_error("模型请求失败或流未完整结束");
                                                      if (!receivedContent)
                                                          writeStringEvent(fullResponse);
                                                      if (bitelog::g_logger)
                                                          INF("Excel 分析流完成，fileId：{}，结果字节数：{}", fileId, fullResponse.size());
                                                  }
                                                  catch (const ClientDisconnected &)
                                                  {
                                                      connected = false;
                                                      if (bitelog::g_logger)
                                                          INF("Excel 分析客户端断开，fileId：{}", fileId);
                                                  }
                                                  catch (const std::exception &error)
                                                  {
                                                      if (bitelog::g_logger)
                                                          ERR("Excel 分析流失败，fileId：{}，原因：{}", fileId, error.what());
                                                      try
                                                      {
                                                          writeStringEvent("模型请求失败，请检查密钥、网络连接及服务器日志", true);
                                                      }
                                                      catch (const std::exception &)
                                                      {
                                                          connected = false;
                                                      }
                                                  }
                                                  if (!sessionId.empty())
                                                  {
                                                      try
                                                      {
                                                          chatSdk->deleteSession(sessionId);
                                                      }
                                                      catch (const std::exception &error)
                                                      {
                                                          if (bitelog::g_logger)
                                                              ERR("清理模型会话失败：{}", error.what());
                                                      }
                                                  }
                                                  if (sdkLock.owns_lock())
                                                      sdkLock.unlock();
                                                  if (!connected)
                                                      return false;
                                                  try
                                                  {
                                                      // 成功或模型报错均只在这里发送一次结束标记，并正确结束 HTTP chunked 响应。
                                                      writeFrame("data: [DONE]\n\n");
                                                      sink.done();
                                                      return true;
                                                  }
                                                  catch (const std::exception &)
                                                  {
                                                      return false;
                                                  }
                                              });
    }
    catch (const std::exception &error)
    {
        if (bitelog::g_logger)
            ERR("处理消息请求失败：{}", error.what());
        setJsonError(response, 500, "处理消息请求失败，请检查服务器日志");
    }
}

std::string Chat2ExcelServer::generateFileId()
{
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const auto currentTime = static_cast<std::uint64_t>(std::max<std::int64_t>(timestamp, 0));
    auto previousId = lastFileId.load();
    while (true)
    {
        const auto candidateId = std::max(currentTime, previousId + 1);
        // 比较交换失败时 previousId 自动更新，下一次尝试基于最新值生成 ID。
        if (lastFileId.compare_exchange_weak(previousId, candidateId))
        {
            return std::to_string(candidateId);
        }
    }
}

void Chat2ExcelServer::handleUploadFile(const httplib::Request &request, httplib::Response &response)
{
    std::string fileId;
    std::filesystem::path reservedDirectory;
    std::filesystem::path filePath;
    bool mapped = false;
    try
    {
        if (!request.has_param("fileName") || request.get_param_value_count("fileName") != 1)
        {
            setJsonError(response, 400, "查询参数 fileName 必须提供且仅提供一次");
            return;
        }
        const auto fileName = request.get_param_value("fileName");
        const bool hasControlCharacter = std::any_of(fileName.begin(), fileName.end(), [](unsigned char character)
                                                     { return character < 32 || character == 127; });
        // 查询参数已经由 httplib 解码；拒绝路径分隔符、目录名及 NUL 等控制字符，保证调用方只能提供文件名，不能通过 ../ 或绝对路径改变保存位置。
        if (fileName.empty() || fileName == "." || fileName == ".." ||
            fileName.find_first_of("/\\") != std::string::npos || hasControlCharacter)
        {
            setJsonError(response, 400, "fileName 必须是有效文件名，不能包含路径或控制字符");
            return;
        }

        if (!hasContentType(request, "application/octet-stream"))
        {
            setJsonError(response, 415, "Content-Type 必须为 application/octet-stream");
            return;
        }

        if (request.body.empty())
        {
            setJsonError(response, 400, "上传文件内容不能为空");
            return;
        }

        // CMake 将此目录固定为源码中的 src/ExcelFile，从其他目录启动仍保存到同一位置。
        const std::filesystem::path uploadDirectory(CHAT2EXCEL_UPLOAD_DIRECTORY);
        std::filesystem::create_directories(uploadDirectory);
        while (true)
        {
            fileId = generateFileId();
            auto candidateDirectory = uploadDirectory / fileId;
            std::error_code directoryError;
            if (std::filesystem::create_directory(candidateDirectory, directoryError))
            {
                reservedDirectory = std::move(candidateDirectory);
                break;
            }
            // 创建目录是原子占位：既防止同名上传相互覆盖，也防止不同进程或重启后生成相同时间戳 ID 时覆盖已有文件；已有 ID 改用下一个时间戳 ID 重试。
            if (directoryError && directoryError != std::errc::file_exists)
            {
                throw std::filesystem::filesystem_error("无法创建上传文件目录", candidateDirectory, directoryError);
            }
        }

        filePath = reservedDirectory / fileName;
        // 工具写入完成后已关闭文件，再核对长度，覆盖关闭时才出现的短写错误。
        if (!biteutil::FUTIL::write(filePath.string(), request.body) ||
            std::filesystem::file_size(filePath) != request.body.size())
        {
            throw std::runtime_error("上传文件写入失败");
        }
        {
            // 磁盘写入结束后才登记映射，其他工作线程只能看到已经保存完成的文件。
            std::lock_guard<std::mutex> lock(fileMapMutex);
            mapped = filePathMap.emplace(fileId, filePath.string()).second;
            if (!mapped)
                throw std::runtime_error("上传文件 ID 重复");
        }

        Json::Value result;
        result["success"] = true;
        result["result"]["fileId"] = fileId;
        const auto payload = biteutil::JSON::serialize(result);
        if (!payload)
            throw std::runtime_error("上传响应序列化失败");
        response.set_content(*payload, "application/json; charset=utf-8");
        response.status = 200;
        if (bitelog::g_logger)
        {
            INF("文件上传成功，fileId：{}，路径：{}，字节数：{}", fileId, filePath.string(), request.body.size());
        }
    }
    catch (const std::exception &error)
    {
        if (mapped)
        {
            std::lock_guard<std::mutex> lock(fileMapMutex);
            filePathMap.erase(fileId);
        }
        // 只清理本次独占目录和文件，不递归删除，避免影响其他上传或已有目录。
        std::error_code cleanupError;
        if (!filePath.empty())
            std::filesystem::remove(filePath, cleanupError);
        if (!reservedDirectory.empty())
            std::filesystem::remove(reservedDirectory, cleanupError);
        if (bitelog::g_logger)
        {
            ERR("文件上传失败，fileId：{}，原因：{}", fileId, error.what());
            if (cleanupError)
                WRN("上传临时文件清理失败：{}", cleanupError.message());
        }
        setJsonError(response, 500, "文件保存失败，请检查服务器日志");
    }
}
