#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "view/view.hpp"

// ConsoleView —— View 的第一个实现：打到终端。
//
// 注意 DESIGN.md §8.1：Windows 控制台默认不是 UTF-8，中文会乱码。
// 程序启动时要 SetConsoleOutputCP(CP_UTF8)。
namespace view
{

class ConsoleView : public View
{
public:
    // override 写上：签名和基类对不上时编译器会报错。
    // 不写的话就是静默多了一个新函数，基类那个纯虚的还是没实现。
    void RenderView(const std::vector<core::Entry>& entries) override;
    void RenderEntry(const core::Entry& entry)               override;
    void RenderTags(const std::vector<std::string>& tags)    override;
    void RenderMessage(std::string_view msg)                 override;
    void RenderError(std::string_view msg)                   override;
};

}  // namespace view
