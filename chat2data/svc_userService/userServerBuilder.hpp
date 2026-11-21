#pragma once

#include "BreezeODB.hpp"
#include "BreezeRedis.hpp"

#include <memory>
#include <string>

namespace chat2data
{
    class UserServer;

    using MysqlConfig = BreezeODB::mysql_settings;
    using RedisConfig = Breezeredis::redis_settings;

    // 根据独立配置构造并启动已注册的用户 RPC 服务。
    class UserServerBuilder
    {
    public:
        void SetMysqlConfig(MysqlConfig config);
        void SetRedisConfig(RedisConfig config);
        void SetPort(int port);
        void SetRegistryAddress(std::string address);
        void SetServiceName(std::string name);
        void SetServiceAddress(std::string address);
        void SetEtcdAddress(std::string address);
        void SetWatchedService(std::string name);

        /**
         * @brief 构造数据库、业务和 RPC 实例，启动发现及监听后注册节点。
         * @return 已启动并注册的用户服务。
         * @note 配置、监听或注册失败时通过异常报告。
         */
        std::unique_ptr<UserServer> Build() const;

    private:
        MysqlConfig _mysql;
        RedisConfig _redis;
        int _port = 0;
        std::string _registry_address;
        std::string _service_name;
        std::string _service_address;
        std::string _etcd_address;
        std::string _watched_service;
    };
}
