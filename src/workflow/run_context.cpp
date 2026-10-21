#include "workflow/run_context.hpp"

#include <string>
#include <utility>
#include <vector>

namespace workflow
{

namespace
{

std::string trim(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) { --end; }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> splitPath(std::string_view path)
{
    std::vector<std::string> parts;
    std::size_t              start = 0;
    while (start <= path.size())
    {
        const std::size_t dot = path.find('.', start);
        if (dot == std::string_view::npos)
        {
            parts.emplace_back(path.substr(start));
            break;
        }
        parts.emplace_back(path.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

/** 字符串直接用，其它类型按 JSON 文本化（数字/布尔/对象都能进提示词）。 */
std::string stringify(Value const& value)
{
    return value.is_string() ? value.get<std::string>() : value.dump();
}

}  // namespace

void RunContext::set(std::string id, Value value)
{
    vars_[std::move(id)] = std::move(value);
}

Value const* RunContext::get(std::string_view id) const
{
    const auto it = vars_.find(std::string(id));
    return it == vars_.end() ? nullptr : &it->second;
}

bool RunContext::has(std::string_view id) const
{
    return vars_.find(std::string(id)) != vars_.end();
}

std::expected<std::string, std::string> RunContext::interpolate(std::string_view text) const
{
    std::string out;
    std::size_t position = 0;

    while (true)
    {
        const std::size_t open = text.find("{{", position);
        if (open == std::string_view::npos)
        {
            out.append(text.substr(position));
            break;
        }

        out.append(text.substr(position, open - position));

        const std::size_t close = text.find("}}", open + 2);
        if (close == std::string_view::npos)
        {
            return std::unexpected("插值缺少结束的 }}：" + std::string(text));
        }

        const std::string expression = trim(text.substr(open + 2, close - open - 2));
        if (expression.empty()) { return std::unexpected("插值表达式为空"); }

        const std::vector<std::string> parts = splitPath(expression);

        // 第一段是节点 id，后面每段都是 JSON 字段。
        Value const* current = get(parts.front());
        if (current == nullptr)
        {
            return std::unexpected("引用了不存在的节点：" + parts.front());
        }

        for (std::size_t index = 1; index < parts.size(); ++index)
        {
            if (!current->is_object() || !current->contains(parts[index]))
            {
                return std::unexpected("节点 " + parts.front() + " 没有字段 " + parts[index]);
            }
            current = &(*current)[parts[index]];
        }

        out += stringify(*current);
        position = close + 2;
    }

    return out;
}

}  // namespace workflow
