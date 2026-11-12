#include "utils.hpp"

#include <array>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

int main()
{
    const std::regex format("[0-9a-f]{4}-[0-9a-f]{8}-[0-9a-f]{4}");
    const std::string first = chat2Data::Utils::GenerateUUID();
    const std::string second = chat2Data::Utils::GenerateUUID();
    if (!std::regex_match(first, format) || !std::regex_match(second, format) ||
        first.substr(14) != "0000" || second.substr(14) != "0001")
    {
        std::cerr << "UUID 格式或计数器不正确。\n";
        return 1;
    }

    constexpr int thread_count = 4;
    constexpr int per_thread = 100;
    std::array<std::string, thread_count * per_thread> values; // 存储每个线程生成的 UUID
    std::vector<std::thread> threads;                          // 存储线程对象
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([i, &values]
                             {
                                for (int j = 0; j < per_thread; ++j)
                                {
                                    values[i * per_thread + j] = chat2Data::Utils::GenerateUUID();
                                } });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    std::set<std::string> counters;
    for (const auto &value : values)
    {
        if (!std::regex_match(value, format) || !counters.insert(value.substr(14)).second)
        {
            std::cerr << "并发生成的 UUID 格式或计数器不正确。\n";
            return 1;
        }
    }
    return 0;
}
