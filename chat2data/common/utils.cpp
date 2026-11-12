#include "utils.hpp"

#include <atomic>
#include <cstdint>
#include <random>

namespace chat2Data
{
    std::string Utils::GenerateUUID()
    {
        // 各线程独立持有随机引擎，计数器在所有线程间共享。
        thread_local std::mt19937 generator(std::random_device{}()); // 使用随机设备初始化随机数生成器
        std::uniform_int_distribution<int> distribution(0, 255);     // 生成 0-255 的随机整数
        static std::atomic<std::uint16_t> counter{0};                // 各线程共享的 16 位计数器
        constexpr char digits[] = "0123456789abcdef";

        std::string uuid;
        uuid.reserve(18);
        for (int i = 0; i < 6; ++i)
        {
            if (i == 2)
            {
                uuid += '-';
            }
            const int byte = distribution(generator);
            uuid += digits[byte >> 4];
            uuid += digits[byte & 0x0f];
        }

        uuid += '-';
        const std::uint16_t value = counter.fetch_add(1, std::memory_order_relaxed); // 16 位计数器循环递增
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            uuid += digits[(value >> shift) & 0x0f];
        }
        return uuid;
    }
}
