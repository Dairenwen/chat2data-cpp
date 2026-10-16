#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace ai_chat_sdk {
class ChatSDK;
}

/**
 * @brief 基于 cpp-httplib 的 Chat2Excel HTTP 服务器。
 *
 * 使用独立后台线程接收请求，提供健康检查、Excel 文件上传及 AI 分析的 SSE 流式响应。
 * start() 等待实际监听成功后返回，主线程可继续执行；stop() 和析构函数负责回收后台线程。
 * start()/stop() 的生命周期操作通过互斥量串行执行，应由控制线程调用，不应在 HTTP 请求处理函数内部调用 stop()。
 * 日志使用 bite_scaffold；调用方应在创建服务器前初始化 bitelog 日志器。
 */
class Chat2ExcelServer {
public:
    /**
     * @brief 初始化监听地址和端口，暂不启动监听。
     * @param host 监听地址，默认 0.0.0.0，不能为空。
     * @param port 监听端口，默认 8080，有效范围为 1～65535。
     * @throws std::invalid_argument 地址为空或端口不在有效范围内。
     * @note 从 DEEPSEEK_API_KEY（兼容 deepseek_apikey）读取密钥，配置 deepseek-chat。
     *       未配置密钥时消息接口返回 503，服务器仍可启动并接受文件上传。
     * @throws std::exception ChatSDK 初始化或内存分配失败。
     */
    explicit Chat2ExcelServer(const std::string& host = "0.0.0.0", int port = 8080);

    /** @brief 对象销毁前停止服务器，并等待后台线程退出。 */
    ~Chat2ExcelServer();

    Chat2ExcelServer(const Chat2ExcelServer&) = delete;
    Chat2ExcelServer& operator=(const Chat2ExcelServer&) = delete;
    Chat2ExcelServer(Chat2ExcelServer&&) = delete;
    Chat2ExcelServer& operator=(Chat2ExcelServer&&) = delete;

    /**
     * @brief 设置路由并在独立线程中启动服务器。
     * @return 成功进入监听返回 true；已在运行、绑定失败或监听启动失败返回 false。
     *         已运行时记录提示，不重复创建线程或注册路由。
     * @throws std::exception 分配服务器、设置路由或创建线程失败时传播标准库异常。
     *         后台监听异常会记录日志并使运行状态变为 false。
     */
    bool start();

    /**
     * @brief 停止接受请求，等待正在处理的请求及后台线程退出。
     * @note 未运行时记录提示；已结束但尚未回收的线程也会被回收。
     * @throws std::system_error 互斥量操作或线程回收失败时传播线程库异常。
     */
    void stop();

    /**
     * @brief 获取实际运行状态。
     * @return 服务器已成功启动且 cpp-httplib 仍在监听时返回 true，否则返回 false。
     * @throws std::system_error 获取生命周期互斥量失败时传播标准库异常。
     */
    bool isRunning() const;

    /**
     * @brief 根据已上传文件的 ID，解析所有 worksheet 并汇总为文本。
     * @param fileId 当前服务器对象通过上传接口返回的文件 ID，不能为空。
     * @return 按工作簿顺序输出 worksheet1: 表名、worksheet2: 表名等标题，
     *         每个标题后接对应工作表的 CSV 数据；工作表之间使用换行分隔。
     *         CSV 中的空字段、引号及单元格内换行由 ExcelParser 原样保留。
     * @note 按文件大小预留字符串空间，解析结果更大时由字符串自动扩容。
     *       查询映射时加锁，文件读取和解析过程不占用映射锁。
     * @throws std::invalid_argument fileId 为空。
     * @throws std::out_of_range fileId 不存在于当前服务器对象的文件映射中。
     * @throws std::filesystem::filesystem_error 文件不存在、无法获取文件大小等。
     * @throws std::exception 无效 Excel、OpenXLSX 解析错误、字符串空间不足等；
     *         记录 bite_scaffold 日志后原样向调用方传播，不返回部分解析结果。
     */
    std::string parseExcelFile(const std::string& fileId);

    /**
     * @brief 按 promptTemplate.md 中的固定模板构建 Excel 数据分析提示词。
     * @param fileName 填入 {filename} 的 Excel 文件名。
     * @param excelData 填入 {excelData} 的 Excel 解析结果。
     * @param userQuestion 填入 {userQuestion} 的用户问题。
     * @return 保留模板的中文要求及 Markdown 结构、完成三个占位符替换的提示词。
     * @note 输入按原文插入，空字符串保留为空；输入自身的花括号不作为占位符解析。
     *       本方法不读写服务器状态，可供不同请求线程调用。
     * @throws fmt::format_error 模板格式无效。
     * @throws std::exception 内存分配失败等异常；记录 bite_scaffold 日志后原样传播。
     */
    std::string buildPrompt(const std::string& fileName, const std::string& excelData,
                            const std::string& userQuestion) const;

private:
    /**
     * @brief 在当前 HTTP 服务器实例上绑定所有路由。
     * @note 仅在 start() 持有生命周期锁、创建 HTTP 服务器之后调用。
     * @throws std::exception 路由注册中的分配或正则表达式构造失败。
     */
    void setRoutes();

    /**
     * @brief 接收原始二进制文件并使用 bite_scaffold 保存到本地。
     * @param request POST /api/uploadFile?fileName=xxx，请求类型为 application/octet-stream。
     * @param response 成功返回 HTTP 200 和 {success:true,result:{fileId:"..."}}；
     *        参数错误返回 400，媒体类型不支持返回 415，存储失败返回 500。
     * @note 文件保存于 src/excelFile/<fileId>/<fileName>，路径不依赖启动目录。
     *       fileId 到完整文件路径的映射保存在当前服务器对象中，并使用独立互斥量保护。
     *       本方法只保存二进制内容，不执行工作表解析。失败时撤销映射并清理本次文件。
     * @throws std::exception 错误响应自身构造失败等无法恢复的标准库异常；
     *         常见参数、文件系统和写入错误均在方法内转换为 JSON 错误响应。
     */
    void handleUploadFile(const httplib::Request& request, httplib::Response& response);

    /**
     * @brief 接收 fileId 和 message，解析 Excel 并将模型分析结果以 SSE 实时返回。
     * @param request POST /api/sendMessage，application/json 对象，包含非空字符串 fileId、message。
     * @param response 校验失败返回 JSON 错误及 400/404/415/422/503 状态；
     *        成功建立 HTTP 200、text/event-stream、分块传输的响应。
     * @note 数据帧为 data: <JSON字符串>\n\n，结束帧为 data: [DONE]\n\n。
     *       建流后的模型错误通过 event: error 事件返回，并结束流；断连后停止写入。
     *       每个请求独立创建、释放 SDK 会话，避免不同文件和问题混入历史上下文。
     * @throws std::exception 错误响应自身构造失败等无法恢复的异常；
     *         请求、解析及模型错误均记录 bite_scaffold 日志后转为响应。
     * @return 无，结果由 response 及其流式内容提供器输出。
     */
    void sendMessage(const httplib::Request& request, httplib::Response& response);

    /**
     * @brief 根据 Unix 微秒时间戳生成递增的数字字符串 ID。
     * @return 本对象内并发安全且不重复的 fileId；同一微秒或时钟回拨时取上次 ID 加一。
     * @throws std::exception 字符串分配失败等标准库异常。
     */
    std::string generateFileId();

    const std::string host;
    const int port;
    std::unique_ptr<httplib::Server> httpServer;
    std::thread serverThread;
    mutable std::mutex lifecycleMutex;
    std::atomic<bool> running{false};
    std::atomic<bool> threadFinished{true};
    std::atomic<std::uint64_t> lastFileId{0};
    // 上传线程只在操作映射时持锁，不用生命周期锁，避免停止等待请求时发生相互等待。
    std::mutex fileMapMutex;
    std::unordered_map<std::string, std::string> filePathMap;
    std::unique_ptr<ai_chat_sdk::ChatSDK> chatSdk;
    // 参考 SDK 的会话操作并非完全线程安全，串行保护模型调用，与文件映射锁相互独立。
    std::mutex chatSdkMutex;
};
