#pragma once

#include "fileInfoEntity.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <odb/connection.hxx>
#include <odb/transaction.hxx>

namespace sw::redis
{
    class Redis;
}

namespace chat2Data
{
    inline constexpr char kFilePending[] = "pending";     // 文件上传中，未完成上传前的临时状态
    inline constexpr char kFileUploading[] = "uploading"; // 文件上传中，已完成上传前的临时状态
    inline constexpr char kFileReady[] = "ready";         // 文件上传完成，已就绪可导入
    inline constexpr char kFileDeleting[] = "deleting";   // 文件删除中，已发起删除请求但未完成删除前的临时状态

    // 工作表映射，按导入顺序排列
    struct WorksheetMapping
    {
        std::string worksheet_name;
        std::string table_name;
    };

    // 从连接池取出一条连接持有 MySQL 命名锁。同一文件的各次事务共用该连接。操作结束时释放命名锁，再把连接交还连接池。
    class FileOperation
    {
    public:
        /**
         * @brief 在专用连接上尝试获取文件操作锁。
         * @param file_id 业务文件 ID。
         * @throws Chat2DataException 锁被占用时报告 FILE_BUSY，GET_LOCK 返回 NULL 时报告 FILE_BUSINESS_FAILED。
         */
        FileOperation(odb::database &database, const std::string &file_id);
        ~FileOperation();

        FileOperation(const FileOperation &) = delete;
        FileOperation &operator=(const FileOperation &) = delete;

        /**
         * @brief 在持有命名锁的同一连接上开启事务，事务结束后仍保留命名锁。
         */
        odb::transaction Begin() const;

    private:
        odb::database &_database;        // ODB 数据库句柄。
        odb::connection_ptr _connection; // 从连接池取得并持有的 MySQL 连接。
        std::string _key;                // MySQL 命名锁的名称，格式为 chat2data:file:<file_id>
    };

    // MySQL 方法使用业务层开启的事务，Redis 方法单独处理缓存。
    class FileData
    {
    public:
        /**
         * @brief 保存 ODB 数据库句柄和 Redis 客户端。
         */
        FileData(std::shared_ptr<odb::database> database, std::shared_ptr<sw::redis::Redis> redis);

        /**
         * @brief 获取文件操作锁。
         */
        FileOperation LockOperation(const std::string &file_id);

        /**
         * @brief 开启 MySQL 事务。
         */
        odb::transaction Begin() const;

        // 在当前事务内插入文件记录，并由 ODB 回填自增主键 id。
        void Save(FileInfoEntity &file);

        // 按实体的主键 id 更新。调用方先锁定文件行并校验归属和状态。
        void Update(const FileInfoEntity &file);

        // 按实体的主键 id 删除文件记录，工作表映射由 MySQL 外键级联删除，不需要额外操作。
        void Delete(const FileInfoEntity &file);

        /**
         * @brief 在当前事务中按文件 ID 加锁查询，缺失时返回空指针。
         */
        std::unique_ptr<FileInfoEntity> FindForUpdate(const std::string &file_id);

        /**
         * @brief 查询用户名下所有已就绪文件，同一上传时间按文件 ID 升序排列。
         */
        std::vector<FileInfoEntity> ListReady(const std::string &user_id);

        /**
         * @brief 在调用方事务内按工作簿顺序逐条写入映射。
         */
        void SaveWorksheets(const std::string &file_id, const std::vector<WorksheetMapping> &worksheets);

        /**
         * @brief 按映射的自增主键升序恢复工作簿中的工作表顺序。
         */
        std::vector<WorksheetMapping> FindWorksheets(const std::string &file_id);

        /**
         * @brief 读取文件元数据缓存，键缺失时返回空值。
         */
        std::optional<FileInfoEntity> ReadFileCache(const std::string &file_id);

        /**
         * @brief 批量读取元数据和映射缓存，映射在业务层通过权限校验后解析。
         */
        std::pair<std::optional<FileInfoEntity>, std::optional<std::string>> ReadExcelCache(const std::string &file_id);

        /**
         * @brief 解析工作表映射缓存，缺失或格式错误时抛出 FILE_CACHE_FAILED。
         */
        std::vector<WorksheetMapping> ParseWorksheetCache(const std::string &text);

        // 将带归属和状态的文件记录写入 Hash 的 data 字段，保留 3 天。
        void SaveFileCache(const FileInfoEntity &file);

        // 将有序的工作表映射数组写入 Hash 的 data 字段，保留 1 小时。
        void SaveWorksheetCache(const std::string &file_id, const std::vector<WorksheetMapping> &worksheets);

        /**
         * @brief 在同一个 Redis DEL 命令中清除元数据和映射。
         */
        void ClearCache(const std::string &file_id);

    private:
        std::shared_ptr<odb::database> _database;
        std::shared_ptr<sw::redis::Redis> _redis;
    };
}
