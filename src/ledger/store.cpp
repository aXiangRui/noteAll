#include "ledger/store.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "core/note_log.hpp"

namespace ledger
{

namespace
{

using json = nlohmann::json;

std::string toIsoUtc(std::chrono::sys_seconds time)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", time);
}

std::optional<std::chrono::sys_seconds> fromIsoUtc(std::string const& text)
{
    std::chrono::sys_seconds time{};
    std::istringstream        in(text);
    in >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", time);
    if (in.fail()) { return std::nullopt; }
    return time;
}

json toJson(Transaction const& t)
{
    json j{
        { "id",           t.id },
        { "amount_cents", t.amountCents },
        { "direction",    toString(t.direction) },
        { "occurred",     toIsoUtc(t.occurred) },
        { "category",     t.category },
        { "note",         t.note },
        { "created_at",   toIsoUtc(t.created_at) },
        { "external_id",  t.external_id },
    };

    if (t.deleted_at) { j["deleted_at"] = toIsoUtc(*t.deleted_at); }
    return j;
}

std::optional<Transaction> fromJson(json const& j)
{
    if (!j.contains("id") || !j.contains("occurred")) { return std::nullopt; }

    Transaction t;
    t.id          = j.value("id", std::string{});
    t.amountCents = j.value("amount_cents", static_cast<std::int64_t>(0));
    t.direction   = directionFromString(j.value("direction", std::string{ "expense" }));
    t.category    = j.value("category", std::string{ "其他" });
    t.note        = j.value("note", std::string{});
    t.external_id = j.value("external_id", std::string{});
    if (t.id.empty() || t.amountCents <= 0) { return std::nullopt; }

    const auto occurred = fromIsoUtc(j.value("occurred", std::string{}));
    if (!occurred) { return std::nullopt; }
    t.occurred = *occurred;

    const auto createdAt = fromIsoUtc(j.value("created_at", std::string{}));
    t.created_at = createdAt.value_or(t.occurred);

    if (j.contains("deleted_at"))
    {
        t.deleted_at = fromIsoUtc(j.value("deleted_at", std::string{}));
    }
    return t;
}

}  // namespace

std::expected<LoadedLedger, std::string> load(std::filesystem::path const& file)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
    {
        return LoadedLedger{};   // 没有文件 = 空账本，不是错误
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) { return std::unexpected("打不开账本文件: " + file.string()); }

    LoadedLedger result;
    std::string  line;
    int          lineNumber = 0;

    while (std::getline(in, line))
    {
        ++lineNumber;
        if (line.empty() || line == "\r") { continue; }

        json parsed;
        try
        {
            parsed = json::parse(line);
        }
        catch (std::exception const& ex)
        {
            result.warnings.push_back("第 " + std::to_string(lineNumber) + " 行解析失败，已跳过：" + ex.what());
            continue;
        }

        // schema 头：{"schema": N}
        if (parsed.is_object() && parsed.contains("schema") && !parsed.contains("id"))
        {
            const int version = parsed.value("schema", 0);
            if (version > kSchemaVersion)
            {
                return std::unexpected("账本文件版本过新（" + std::to_string(version) +
                                       "），当前程序支持到 " + std::to_string(kSchemaVersion));
            }
            continue;
        }

        if (!parsed.is_object()) { continue; }

        if (auto t = fromJson(parsed)) { result.transactions.push_back(std::move(*t)); }
        else
        {
            result.warnings.push_back("第 " + std::to_string(lineNumber) + " 行缺字段，已跳过");
        }
    }

    return result;
}

std::expected<void, std::string> save(std::filesystem::path const&    file,
                                      const std::vector<Transaction>& transactions)
{
    std::filesystem::path tmp = file;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { return std::unexpected("写不了临时文件: " + tmp.string()); }

        out << R"({"schema": )" << kSchemaVersion << "}\n";
        for (const auto& t : transactions) { out << toJson(t).dump() << '\n'; }

        if (!out) { return std::unexpected("写入失败: " + tmp.string()); }
    }   // 句柄必须关掉再 rename（Windows 上不关会失败）

    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) { return std::unexpected("替换账本文件失败: " + ec.message()); }

    return {};
}

}  // namespace ledger
