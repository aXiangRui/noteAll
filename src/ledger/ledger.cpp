#include "ledger/ledger.hpp"

#include <array>
#include <string>
#include <vector>

namespace ledger
{

namespace
{

// 关键词表：先做确定性版本。以后接模型时，这里就是"兜底规则"。
struct CategoryRule
{
    std::string_view keyword_;
    std::string_view category_;
};

constexpr std::array<CategoryRule, 26> kExpenseRules = { {
    { "饭", "餐饮" },   { "吃", "餐饮" },   { "早餐", "餐饮" }, { "午餐", "餐饮" },
    { "晚餐", "餐饮" }, { "咖啡", "餐饮" }, { "奶茶", "餐饮" }, { "外卖", "餐饮" },
    { "地铁", "交通" }, { "公交", "交通" }, { "打车", "交通" }, { "出租", "交通" },
    { "加油", "交通" }, { "停车", "交通" }, { "高铁", "交通" }, { "机票", "交通" },
    { "超市", "购物" }, { "买", "购物" },   { "淘宝", "购物" }, { "京东", "购物" },
    { "房租", "居家" }, { "水电", "居家" }, { "物业", "居家" }, { "话费", "居家" },
    { "电影", "娱乐" }, { "游戏", "娱乐" },
} };

constexpr std::array<CategoryRule, 6> kIncomeRules = { {
    { "工资", "工资" },
    { "薪", "工资" },
    { "奖金", "奖金" },
    { "报销", "报销" },
    { "退款", "退款" },
    { "利息", "理财" },
} };

constexpr std::array<std::string_view, 5> kIncomeKeywords = {
    "工资", "薪", "奖金", "报销", "退款",
};

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

std::string toString(Direction direction)
{
    return direction == Direction::Income ? "income" : "expense";
}

Direction directionFromString(std::string_view text)
{
    return text == "income" ? Direction::Income : Direction::Expense;
}

std::vector<std::string> expenseCategories()
{
    return { "餐饮", "交通", "购物", "居家", "娱乐", "医疗", "教育", "人情", "其他" };
}

std::vector<std::string> incomeCategories()
{
    return { "工资", "奖金", "报销", "退款", "理财", "其他" };
}

Direction guessDirection(std::string_view note)
{
    for (const auto keyword : kIncomeKeywords)
    {
        if (contains(note, keyword)) { return Direction::Income; }
    }
    return Direction::Expense;
}

std::string guessCategory(std::string_view note, Direction direction)
{
    if (direction == Direction::Income)
    {
        for (const auto& rule : kIncomeRules)
        {
            if (contains(note, rule.keyword_)) { return std::string{ rule.category_ }; }
        }
        return "其他";
    }

    for (const auto& rule : kExpenseRules)
    {
        if (contains(note, rule.keyword_)) { return std::string{ rule.category_ }; }
    }
    return "其他";
}

}  // namespace ledger
