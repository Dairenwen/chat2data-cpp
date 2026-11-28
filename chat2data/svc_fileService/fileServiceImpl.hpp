#pragma once

#include "fileBusiness.hpp"
#include "fileService.pb.h"

namespace chat2Data
{
    // 将 brpc 请求附件及协议字段转换为文件业务参数。
    class FileServiceImpl final : public FileService
    {
    public:
        /**
         * @brief 接管文件业务对象。
         */
        explicit FileServiceImpl(FileBusiness business);

        /**
         * @brief 登记 Excel 文件名、大小及后缀，成功时返回业务文件 ID。
         */
        void UploadFileInfo(google::protobuf::RpcController *controller, const UploadFileInfoRequest *request,
                            UploadFileInfoResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 按文件 ID 查询已就绪文件的对外元数据。
         */
        void GetFileInfo(google::protobuf::RpcController *controller, const GetFileInfoRequest *request,
                         GetFileInfoResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 完成 Excel 第二步上传，文件字节从请求附件读取。
         * @param controller brpc 控制器，持有 Excel 原件。
         */
        void UploadFile(google::protobuf::RpcController *controller, const UploadFileRequest *request,
                        UploadFileResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 返回原文件名，将原始字节写入 brpc 响应附件。
         */
        void DownloadFile(google::protobuf::RpcController *controller, const DownloadFileRequest *request,
                          DownloadFileResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 删除文件及关联资源，清理完成后返回成功。
         */
        void DeleteFile(google::protobuf::RpcController *controller, const DeleteFileRequest *request,
                        DeleteFileResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 按工作表原名及分页参数返回已导入数据。
         */
        void PreviewExcel(google::protobuf::RpcController *controller, const PreviewExcelRequest *request,
                          PreviewExcelResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 返回当前用户的已就绪文件列表。
         */
        void GetFileList(google::protobuf::RpcController *controller, const GetFileListRequest *request,
                         GetFileListResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 将已就绪 Excel 文件关联到指定的聊天会话。
         */
        void HandleFileChatSessionMap(google::protobuf::RpcController *controller, const HandleFileChatSessionMapRequest *request,
                                      HandleFileChatSessionMapResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 使用请求附件上传 SQLite 快照，成功时返回文件 ID。
         */
        void UploadSQLiteFile(google::protobuf::RpcController *controller, const UploadSQLiteFileRequest *request,
                              UploadSQLiteFileResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 向存储子服务返回 SQLite 原件的 FastDFS 文件 ID。
         */
        void GetSQLiteFile(google::protobuf::RpcController *controller, const GetSQLiteFileRequest *request,
                           GetSQLiteFileResponse *response, google::protobuf::Closure *done) override;
        /**
         * @brief 向 AI 子服务返回按工作簿顺序排列的实际表名。
         */
        void GetWorksheetDBTables(google::protobuf::RpcController *controller, const GetWorksheetDBTablesRequest *request,
                                  GetWorksheetDBTablesResponse *response, google::protobuf::Closure *done) override;

    private:
        FileBusiness _business;
    };
}
