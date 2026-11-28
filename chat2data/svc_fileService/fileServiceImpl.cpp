#include "fileServiceImpl.hpp"

#include "BreezeLog.hpp"
#include "errorHandler.hpp"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <brpc/protocol.h>
#include <sw/redis++/errors.h>
#include <utility>

namespace chat2Data
{
    namespace
    {
        // 判断文件 ID 是否为 32 位十六进制字符串
        bool IsFileId(const std::string &file_id)
        {
            return file_id.size() == 32 && file_id.find_first_not_of("0123456789abcdef") == std::string::npos;
        }
        // 读取 brpc 请求附件，返回连续缓冲区的文件字节，这里使用attachment的好处是可以避免多次拷贝数据，直接使用 brpc 的内存管理机制
        std::string ReadAttachment(google::protobuf::RpcController &controller)
        {
            const auto &attachment = static_cast<brpc::Controller &>(controller).request_attachment();
            if (attachment.empty() || attachment.size() > kMaxFileSize)
                throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);

            // FastDFS 的内存上传接口需要连续缓冲区。
            std::string content;
            attachment.copy_to(&content);
            return content;
        }

        template <typename Result>
        void SetFileFields(const FileInfoEntity &file, Result &result)
        {
            result.set_file_id(file.fileId);
            result.set_file_name(file.fileName);
            result.set_file_size(file.fileSize);
            result.set_upload_time(file.uploadTime);
        }

        // 各 RPC 共用回调、身份字段校验和错误响应，业务方法保留各自的参数校验。
        template <typename Request, typename Response, typename Run>
        void RunFileRpc(const Request &request, Response &response, google::protobuf::Closure *done,
                        const char *method, const std::string *requested_file_id, Run run)
        {
            brpc::ClosureGuard guard(done);
            response.Clear(); // 每次调用从空响应开始，复用响应对象时也只返回本次结果。
            response.set_request_id(request.request_id());
            std::string file_id = requested_file_id ? *requested_file_id : std::string{};
            auto code = ErrorCode::SUCCESS;
            try
            {
                if (request.request_id().empty() || request.session_id().empty() || request.user_id().empty() ||
                    request.user_id().size() > 32 || (requested_file_id && !IsFileId(file_id)))
                    throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
                run(file_id);
            }
            catch (const Chat2DataException &error)
            {
                code = error.Code();
            }
            catch (const sw::redis::Error &)
            {
                code = ErrorCode::FILE_CACHE_FAILED;
            }
            catch (const std::exception &)
            {
                code = ErrorCode::FILE_BUSINESS_FAILED;
            }

            // 错误响应只保留公共字段，预览超限时也不会返回部分结果。
            if (code != ErrorCode::SUCCESS)
            {
                response.Clear();
                response.set_request_id(request.request_id());
                response.set_error_msg(error2String(code));
                WRN("文件 RPC {} 失败，请求 ID {}，文件 ID {}，错误码 {}", method, request.request_id(), file_id, static_cast<int>(code));
            }
            else
            {
                response.clear_error_msg();
                INF("文件 RPC {} 完成，请求 ID {}，文件 ID {}，错误码 0", method, request.request_id(), file_id);
            }
            response.set_error_code(static_cast<int>(code));
        }
    }

    FileServiceImpl::FileServiceImpl(FileBusiness business)
        : _business(std::move(business))
    {
    }

    void FileServiceImpl::UploadFileInfo(google::protobuf::RpcController *, const UploadFileInfoRequest *request,
                                         UploadFileInfoResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "UploadFileInfo", nullptr,
                   [&](std::string &file_id)
                   {
                       if (!request->has_file_info())
                           throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
                       file_id = _business.UploadFileInfo(request->user_id(), request->file_info());
                       response->mutable_result()->set_file_id(file_id);
                   });
    }

    void FileServiceImpl::GetFileInfo(google::protobuf::RpcController *, const GetFileInfoRequest *request,
                                      GetFileInfoResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "GetFileInfo", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       const auto file = _business.GetFileInfo(request->user_id(), file_id);
                       auto *result = response->mutable_result();
                       SetFileFields(file, *result);
                       result->set_file_ext(file.fileExt);
                   });
    }

    void FileServiceImpl::UploadFile(google::protobuf::RpcController *controller, const UploadFileRequest *request,
                                     UploadFileResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "UploadFile", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       _business.UploadFile(request->request_id(), request->user_id(), file_id, ReadAttachment(*controller));
                       response->mutable_result()->set_file_id(file_id);
                   });
    }

    void FileServiceImpl::DownloadFile(google::protobuf::RpcController *controller, const DownloadFileRequest *request,
                                       DownloadFileResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "DownloadFile", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       auto [filename, content] = _business.DownloadFile(request->user_id(), file_id);
                       // 先准备完整附件，成功后再写入文件名和内容。
                       butil::IOBuf attachment;
                       if (attachment.append(content) != 0)
                           throw Chat2DataException(ErrorCode::FILE_BUSINESS_FAILED);
                       response->set_file_name(std::move(filename));
                       static_cast<brpc::Controller *>(controller)->response_attachment().swap(attachment);
                   });
    }

    void FileServiceImpl::DeleteFile(google::protobuf::RpcController *, const DeleteFileRequest *request,
                                     DeleteFileResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "DeleteFile", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       _business.DeleteFile(request->user_id(), file_id);
                   });
    }

    void FileServiceImpl::PreviewExcel(google::protobuf::RpcController *, const PreviewExcelRequest *request,
                                       PreviewExcelResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "PreviewExcel", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       auto result = _business.PreviewExcel(request->user_id(), file_id, request->sheet_name(),
                                                            request->page_number(), request->page_size());
                       response->mutable_result()->Swap(&result);
                       if (response->ByteSizeLong() > brpc::FLAGS_max_body_size)
                           throw Chat2DataException(ErrorCode::FILE_PREVIEW_TOO_LARGE);
                   });
    }

    void FileServiceImpl::GetFileList(google::protobuf::RpcController *, const GetFileListRequest *request,
                                      GetFileListResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "GetFileList", nullptr,
                   [&](std::string &)
                   {
                       const auto files = _business.GetFileList(request->user_id());
                       auto *result = response->mutable_result();
                       for (const auto &file : files)
                           SetFileFields(file, *result->add_file_list());
                   });
    }

    void FileServiceImpl::HandleFileChatSessionMap(google::protobuf::RpcController *, const HandleFileChatSessionMapRequest *request,
                                                   HandleFileChatSessionMapResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "HandleFileChatSessionMap", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       if (request->chat_session_id().empty() || request->chat_session_id().size() > 32)
                           throw Chat2DataException(ErrorCode::FILE_INVALID_INPUT);
                       _business.HandleFileChatSessionMap(request->user_id(), file_id, request->chat_session_id());
                   });
    }

    void FileServiceImpl::UploadSQLiteFile(google::protobuf::RpcController *controller, const UploadSQLiteFileRequest *request,
                                           UploadSQLiteFileResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "UploadSQLiteFile", nullptr,
                   [&](std::string &file_id)
                   {
                       file_id = _business.UploadSQLiteFile(request->user_id(), request->filename(), ReadAttachment(*controller));
                       response->mutable_result()->set_file_id(file_id);
                   });
    }

    void FileServiceImpl::GetSQLiteFile(google::protobuf::RpcController *, const GetSQLiteFileRequest *request,
                                        GetSQLiteFileResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "GetSQLiteFile", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       response->mutable_result()->set_fdfs_file_id(_business.GetSQLiteFile(request->user_id(), file_id));
                   });
    }

    void FileServiceImpl::GetWorksheetDBTables(google::protobuf::RpcController *, const GetWorksheetDBTablesRequest *request,
                                               GetWorksheetDBTablesResponse *response, google::protobuf::Closure *done)
    {
        RunFileRpc(*request, *response, done, "GetWorksheetDBTables", &request->file_id(),
                   [&](const std::string &file_id)
                   {
                       const auto mappings = _business.GetWorksheetDBTables(request->user_id(), file_id);
                       auto *result = response->mutable_result();
                       for (const auto &mapping : mappings)
                           result->add_worksheet_db_tables(mapping.table_name);
                   });
    }
}
