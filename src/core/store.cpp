#include "core/store.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace core
{
namespace
{
   
    std::expected<void, std::string> renameWithRetry(const std::filesystem::path& from,
                                                     const std::filesystem::path& to)
    {
        constexpr int  MAX_ATTEMPTS = 3;
        constexpr auto RETRY_DELAY  = std::chrono::milliseconds{50};

        std::error_code ec;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
        {
            std::filesystem::rename(from, to, ec);
            if (!ec)
            {
                return {};  
            }
            std::this_thread::sleep_for(RETRY_DELAY);
        }

        return std::unexpected("重命名失败: " + from.string() + " -> " + to.string()
                               + " (" + ec.message() + ")");
    }
}   

std::string toIso(std::chrono::sys_seconds t)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}

bool fromIso(std::string_view s, std::chrono::sys_seconds& out)
{
    std::istringstream        in{std::string{s}};
    std::chrono::sys_seconds  parsed{};

    std::chrono::from_stream(in, "%Y-%m-%dT%H:%M:%SZ", parsed);

    if (in.fail())
    {
        return false;
    }

    out = parsed;
    return true;
}


void to_json(nlohmann::json& j, const TimeRange& t)
{
    j = nlohmann::json{{"start", toIso(t.start)}};

    if (t.end)
    {
        j["end"] = toIso(*t.end);
    }
}

void from_json(const nlohmann::json& j, TimeRange& t)
{
    
    fromIso(j.value("start", std::string{}), t.start);

    t.end.reset();  
    if (j.contains("end") && j["end"].is_string())
    {
        std::chrono::sys_seconds end{};
        if (fromIso(j["end"].get<std::string>(), end))
        {
            t.end = end;
        }
    }
}

void to_json(nlohmann::json& j, const Entry& e)
{
    j = nlohmann::json{
        {"id",         e.id},
        {"raw_text",   e.raw_text},            // 原文原样存，不 trim
        {"created_at", toIso(e.created_at)},   // chrono -> 字符串
        {"summary",    e.summary},
        {"tags",       e.tags},                // vector<string> 有内置支持，直接给
        {"kind",       toString(e.kind)},      // 存字符串不存数字，见坑清单 #6
        {"confirmed",  e.confirmed},
    };

    if (e.occurred)
    {
        j["occurred"] = *e.occurred;   // 这里会调到上面 TimeRange 的 to_json
    }
}

void from_json(const nlohmann::json& j, Entry& e)
{
    e.id        = j.value("id",        std::string{});
    e.raw_text  = j.value("raw_text",  std::string{});
    e.summary   = j.value("summary",   std::string{});
    e.tags      = j.value("tags",      std::vector<std::string>{});
    e.kind      = kindFromString(j.value("kind", std::string{}));
    e.confirmed = j.value("confirmed", false);

    // created_at 解析失败就保持默认（epoch）—— 不在这里报错，
    // 因为 from_json 没有地方放 warning。坏数据的警告由 load 统一收集。
    std::chrono::sys_seconds created{};
    if (fromIso(j.value("created_at", std::string{}), created))
    {
        e.created_at = created;
    }

    e.occurred.reset();
    if (j.contains("occurred") && j["occurred"].is_object())
    {
        TimeRange t{};
        from_json(j["occurred"], t);
        e.occurred = t;
    }
}

std::expected<LoadResult, std::string> load(const std::filesystem::path& path)
{
    LoadResult result;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return result;
    }

    // binary：Windows 文本模式会把 '\n' 悄悄翻译成 '\r\n'，字节数对不上，
    // JSONL 的"一行一条"假设也就没了（ROADMAP §2.3 细节 3）。
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::unexpected("打不开文件: " + path.string());
    }

    std::string line;
    int         lineNo = 0;

    while (std::getline(in, line))
    {
        ++lineNo;

        if (line.empty())
        {
            continue;
        }
        if (line.back() == '\r')
        {
            line.pop_back();
            if (line.empty())
            {
                continue;
            }
        }

        try
        {
            const nlohmann::json j = nlohmann::json::parse(line);

            if (!j.is_object())
            {
                result.warnings.push_back("第 " + std::to_string(lineNo)
                                          + " 行不是 JSON 对象，已跳过");
                continue;
            }

            result.entries.push_back(j.get<Entry>());
        }
        catch (const nlohmann::json::exception& ex)
        {
            
            result.warnings.push_back("第 " + std::to_string(lineNo)
                                      + " 行解析失败，已跳过：" + ex.what());
        }
    }

    if (in.bad())
    {
        return std::unexpected("读取过程中出错: " + path.string());
    }

    return result;
}

std::expected<void, std::string> save(const std::filesystem::path& path,
                                      const std::vector<Entry>&    entries)
{
    // 原子写：先全写进 .tmp，成功后再 rename 覆盖正式文件。
    // 这样中途断电 / 崩溃时，正式文件要么是旧的完整版、要么是新的完整版，
    // 绝不会出现"写了一半"的文件。
    std::filesystem::path tmp = path;
    tmp += ".tmp";   // path 的 += 是往 native 字符串后面接，不会自己加分隔符

    {
        // 这个花括号很重要：让 ofstream 在这里就析构、把文件句柄关掉。
        // 句柄没关就 rename，Windows 上会失败。
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::unexpected("打不开临时文件: " + tmp.string());
        }

        for (const Entry& e : entries)
        {
            // dump() **不传参数**：传了 indent 就变成多行，JSONL 直接坏掉（细节 1）。
            // 默认 ensure_ascii=false，正好让中文原样存进去。
            out << nlohmann::json(e).dump() << '\n';

            if (!out)
            {
                return std::unexpected("写入临时文件失败: " + tmp.string());
            }
        }

        out.flush();
        if (!out)
        {
            return std::unexpected("刷新临时文件失败: " + tmp.string());
        }
    }

    return renameWithRetry(tmp, path);
}

}   // namespace core
