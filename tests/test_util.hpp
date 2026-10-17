#pragma once

// 测试用的小工具。只给 tests/ 下的文件 include，不进产品代码。

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

namespace testutil
{
    // 每次构造一个独立的临时文件；析构时连 .tmp 一起删掉。
    // 用例失败（提前 return / 抛异常）也不会在磁盘上堆垃圾。
    class TempFile
    {
    public:
        TempFile()
        {
            // 随机名字：同一台机器上并行跑多个测试进程也不会互相踩。
            static std::mt19937_64 rng{std::random_device{}()};
            const std::string name = "noteall-test-" + std::to_string(rng()) + ".jsonl";
            path_ = std::filesystem::temp_directory_path() / name;
        }

        ~TempFile()
        {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            std::filesystem::remove(path_.string() + ".tmp", ec);
        }

        TempFile(const TempFile&)            = delete;
        TempFile& operator=(const TempFile&) = delete;

        const std::filesystem::path& path() const { return path_; }

    private:
        std::filesystem::path path_;
    };

    inline void writeFile(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    inline std::string readFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};
    }

    // 2026-10-18 09:30:00 这种，写起来比 sys_days{...} 短。
    inline std::chrono::sys_seconds at(int           year_,
                                       unsigned      month_,
                                       unsigned      day_,
                                       int           hour_   = 0,
                                       int           minute_ = 0)
    {
        using namespace std::chrono;
        return sys_days{ std::chrono::year{ year_ } / std::chrono::month{ month_ }
                         / std::chrono::day{ day_ } }
             + hours{ hour_ } + minutes{ minute_ };
    }
}
