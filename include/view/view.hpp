#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/entry.hpp"

// 呈现层接口。形状来自 DESIGN.md §4.2：数据作为**参数**传入，视图不持有 core。
// 依赖方向是单向的 view -> core（DESIGN.md §3），所以这里没有 core/ 反过来包含 view。
namespace view
{

class View
{
public:
    // 列表：每条一行，紧凑
    virtual void RenderView(const std::vector<core::Entry>& entries) = 0;

    // 详情：一条，多行，字段展开
    virtual void RenderEntry(const core::Entry& entry) = 0;

    virtual void RenderTags(const std::vector<std::string>& tags) = 0;

    // 非列表输出 —— "已记录 xxx"、"已删除 xxx" 这类
    virtual void RenderMessage(std::string_view msg) = 0;

    // 出错。走 stderr，好让人 `note list > out.txt` 时错误仍然看得见
    virtual void RenderError(std::string_view msg) = 0;

    // ⚠️ 基类析构必须虚，否则通过 View* 删除 ConsoleView 是未定义行为。
    virtual ~View() = default;
};

}  // namespace view
