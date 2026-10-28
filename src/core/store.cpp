#include "core/store.hpp"

#include "core/note_log.hpp"

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
    // start 是必填字段，而且必须是合法时间。
    //
    // 以前这里忽略了 fromIso 的返回值：{"start":"garbage"} 会静默变成
    // 1970-01-01，既不报错也没 warning —— 一条坏数据看起来像"确实发生在 1970 年"。
    // 现在改成抛 json::exception，由 load() 统一收成 warning 并跳过该行。
    // 跳过是安全的：load 不重写文件，原始那一行还在磁盘上。
    if (!j.contains("start") || !j["start"].is_string())
    {
        throw nlohmann::json::other_error::create(
            501, "occurred.start 缺失或不是字符串", &j);
    }

    std::chrono::sys_seconds start{};
    if (!fromIso(j["start"].get<std::string>(), start))
    {
        throw nlohmann::json::other_error::create(
            501, "occurred.start 不是合法的 ISO 8601 时间", &j);
    }
    t.start = start;

    // end 可选。写坏了就当"没有终点"（时间点），不因为一个可选字段丢掉整行。
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
        {"title",      e.title},
        {"summary",    e.summary},
        {"tags",       e.tags},                // vector<string> 有内置支持，直接给
        {"kind",       toString(e.kind)},      // 存字符串不存数字，见坑清单 #6
        {"confirmed",  e.confirmed},
        {"source",     toString(e.source)},
        {"draft_status", toString(e.draft_status)},
    };

    // 只在非默认时写这两个字段：绝大多数记录没有失败史，省得每行都拖两个空值。
    if (e.draft_attempts > 0) { j["draft_attempts"] = e.draft_attempts; }
    if (!e.draft_error.empty()) { j["draft_error"] = e.draft_error; }

    if (e.occurred)
    {
        j["occurred"] = *e.occurred;   // 这里会调到上面 TimeRange 的 to_json
    }

    // 墓碑只在删除后写：未删除的记录不必多一个 null 字段。
    if (e.deleted_at)
    {
        j["deleted_at"] = toIso(*e.deleted_at);
    }
}

void from_json(const nlohmann::json& j, Entry& e)
{
    e.id        = j.value("id",        std::string{});
    e.raw_text  = j.value("raw_text",  std::string{});
    e.title     = j.value("title",     std::string{});
    e.summary   = j.value("summary",   std::string{});
    e.tags      = j.value("tags",      std::vector<std::string>{});
    e.kind      = kindFromString(j.value("kind", std::string{}));
    e.confirmed = j.value("confirmed", false);
    e.source    = sourceFromString(j.value("source", std::string{"manual"}));
    e.draft_status   = draftStatusFromString(j.value("draft_status", std::string{"none"}));
    e.draft_attempts = j.value("draft_attempts", 0);
    e.draft_error    = j.value("draft_error", std::string{});

    // created_at 解析失败就保持默认（epoch）—— 不在这里报错，
    // 因为 from_json 没有地方放 warning。坏数据的警告由 load 统一收集。
    std::chrono::sys_seconds created{};
    if (fromIso(j.value("created_at", std::string{}), created))
    {
        e.created_at = created;
    }

    // 墓碑：解析不出来就当没删（宁可见到，也不要把记录误判成删除）。
    e.deleted_at.reset();
    if (j.contains("deleted_at") && j["deleted_at"].is_string())
    {
        std::chrono::sys_seconds deleted{};
        if (fromIso(j["deleted_at"].get<std::string>(), deleted))
        {
            e.deleted_at = deleted;
        }
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

    // exists() 的 ec 必须判。权限 / IO 错误以前会被当成"文件不存在"，
    // 返回成功 + 空列表 —— 用户看到"（还没有记录）"，以为数据丢了。
    // 现在真正区分：不存在 -> 成功空列表（首次运行的正常情况）；
    // 查不了 -> 失败，由上层报错。
    std::error_code ec;
    const bool fileExists = std::filesystem::exists(path, ec);
    if (ec)
    {
        return std::unexpected("检查文件失败: " + path.string() + " (" + ec.message() + ")");
    }
    if (!fileExists)
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
    bool        headerSeen = false;

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
                const std::string warning = "第 " + std::to_string(lineNo) + " 行不是 JSON 对象，已跳过";
                result.warnings.push_back(warning);
                appendLog(path, LogLevel::Warn, warning);
                continue;
            }

            // 头部记录 {"schema": N}：不是条目，只用来判版本。
            if (j.contains("schema") && !j.contains("id"))
            {
                if (!headerSeen)
                {
                    headerSeen = true;
                    const int version = j.value("schema", 0);
                    if (version > kSchemaVersion)
                    {
                        // 关键：不读、更不写。老程序去改新格式的文件，可能悄悄丢字段。
                        return std::unexpected(
                            "数据文件是更新版本的程序写的（文件版本 "
                            + std::to_string(version) + "，本程序支持到 "
                            + std::to_string(kSchemaVersion) + "），请升级程序后再打开");
                    }
                    if (version < kSchemaVersion)
                    {
                        const std::string warning = "数据文件是旧版本（" + std::to_string(version)
                                                    + "），下次保存会自动升级到 "
                                                    + std::to_string(kSchemaVersion);
                        result.warnings.push_back(warning);
                        appendLog(path, LogLevel::Warn, warning);
                    }
                }
                continue;
            }

            result.entries.push_back(j.get<Entry>());
        }
        catch (const nlohmann::json::exception& ex)
        {
            const std::string warning = "第 " + std::to_string(lineNo) + " 行解析失败，已跳过："
                                        + ex.what();
            result.warnings.push_back(warning);
            appendLog(path, LogLevel::Warn, warning);
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

        // 第一行是格式版本头。老文件（没有这一行）照样能读，见 load()。
        out << nlohmann::json{{"schema", kSchemaVersion}}.dump() << '\n';
        if (!out)
        {
            return std::unexpected("写入临时文件失败: " + tmp.string());
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
