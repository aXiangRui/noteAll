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

// 关键词表：写在这里的都是**确定性强**的（品牌名、明确品类）。
// 2026-10-31 按真实账单补过一轮：原来 86% 落在「其他」，主要因为
// 微信账单的备注是商户名（麦当劳 / 哈啰骑行 / 美宜佳便利店…），旧表只认「饭/吃」。
constexpr std::array kExpenseRules = {
    CategoryRule{ "饭",           "餐饮" }, CategoryRule{ "吃",           "餐饮" },
    CategoryRule{ "早餐",         "餐饮" }, CategoryRule{ "午餐",         "餐饮" },
    CategoryRule{ "晚餐",         "餐饮" }, CategoryRule{ "咖啡",         "餐饮" },
    CategoryRule{ "奶茶",         "餐饮" }, CategoryRule{ "外卖",         "餐饮" },
    // 餐饮：连锁快餐 / 咖啡茶饮 / 面食
    CategoryRule{ "麦当劳",       "餐饮" }, CategoryRule{ "肯德基",       "餐饮" },
    CategoryRule{ "kfc",          "餐饮" }, CategoryRule{ "汉堡",         "餐饮" },
    CategoryRule{ "华莱士",       "餐饮" }, CategoryRule{ "塔斯汀",       "餐饮" },
    CategoryRule{ "瑞幸",         "餐饮" }, CategoryRule{ "luckin",       "餐饮" },
    CategoryRule{ "星巴克",       "餐饮" }, CategoryRule{ "库迪",         "餐饮" },
    CategoryRule{ "蜜雪",         "餐饮" }, CategoryRule{ "益禾堂",       "餐饮" },
    CategoryRule{ "茶颜",         "餐饮" }, CategoryRule{ "喜茶",         "餐饮" },
    CategoryRule{ "古茗",         "餐饮" }, CategoryRule{ "奶茶店",       "餐饮" },
    CategoryRule{ "饺子",         "餐饮" }, CategoryRule{ "水饺",         "餐饮" },
    CategoryRule{ "抄手",         "餐饮" }, CategoryRule{ "咖喱",         "餐饮" },
    CategoryRule{ "糖水",         "餐饮" }, CategoryRule{ "火锅",         "餐饮" },
    CategoryRule{ "烧烤",         "餐饮" }, CategoryRule{ "拉面",         "餐饮" },
    CategoryRule{ "面馆",         "餐饮" }, CategoryRule{ "烩菜",         "餐饮" },
    CategoryRule{ "沙县",         "餐饮" }, CategoryRule{ "小吃",         "餐饮" },
    CategoryRule{ "零食",         "餐饮" }, CategoryRule{ "水果",         "餐饮" },
    CategoryRule{ "点餐",         "餐饮" }, CategoryRule{ "饿了么",       "餐饮" },
    CategoryRule{ "美团",         "餐饮" }, CategoryRule{ "大众点评",     "餐饮" },
    // 交通：共享单车 / 打车 / 加油 / 充电
    CategoryRule{ "地铁",         "交通" }, CategoryRule{ "公交",         "交通" },
    CategoryRule{ "打车",         "交通" }, CategoryRule{ "出租",         "交通" },
    CategoryRule{ "滴滴",         "交通" }, CategoryRule{ "加油",         "交通" },
    CategoryRule{ "停车",         "交通" }, CategoryRule{ "高铁",         "交通" },
    CategoryRule{ "机票",         "交通" }, CategoryRule{ "火车",         "交通" },
    CategoryRule{ "哈啰",         "交通" }, CategoryRule{ "单车",         "交通" },
    CategoryRule{ "骑行",         "交通" }, CategoryRule{ "充电桩",       "交通" },
    CategoryRule{ "洗车",         "交通" }, CategoryRule{ "高速",         "交通" },
    // 购物
    CategoryRule{ "超市",         "购物" }, CategoryRule{ "便利店",       "购物" },
    CategoryRule{ "淘宝",         "购物" }, CategoryRule{ "京东",         "购物" },
    CategoryRule{ "拼多多",       "购物" }, CategoryRule{ "商场",         "购物" },
    CategoryRule{ "服装",         "购物" }, CategoryRule{ "运动鞋",       "购物" },
    CategoryRule{ "数码",         "购物" }, CategoryRule{ "宜家",         "购物" },
    CategoryRule{ "买",           "购物" }, CategoryRule{ "抖音",         "购物" },
    CategoryRule{ "理发",         "购物" }, CategoryRule{ "发型",         "购物" },
    CategoryRule{ "二维码收款",   "购物" },
    // 居家
    CategoryRule{ "房租",         "居家" }, CategoryRule{ "水电",         "居家" },
    CategoryRule{ "物业",         "居家" }, CategoryRule{ "话费",         "居家" },
    CategoryRule{ "燃气",         "居家" }, CategoryRule{ "快递",         "居家" },
    CategoryRule{ "宽带",         "居家" }, CategoryRule{ "家具",         "居家" },
    CategoryRule{ "电费",         "居家" }, CategoryRule{ "水费",         "居家" },
    CategoryRule{ "电信",         "居家" }, CategoryRule{ "联通",         "居家" },
    CategoryRule{ "中国移动",     "居家" },
    // 娱乐
    CategoryRule{ "电影",         "娱乐" }, CategoryRule{ "游戏",         "娱乐" },
    CategoryRule{ "mihoyo",       "娱乐" }, CategoryRule{ "米哈游",       "娱乐" },
    CategoryRule{ "网吧",         "娱乐" }, CategoryRule{ "ktv",          "娱乐" },
    CategoryRule{ "剧本杀",       "娱乐" }, CategoryRule{ "桌游",         "娱乐" },
    CategoryRule{ "健身",         "娱乐" }, CategoryRule{ "演出",         "娱乐" },
    CategoryRule{ "keep",         "娱乐" }, CategoryRule{ "会员",         "娱乐" },
    // 医疗
    CategoryRule{ "药",           "医疗" }, CategoryRule{ "医院",         "医疗" },
    CategoryRule{ "诊所",         "医疗" }, CategoryRule{ "挂号",         "医疗" },
    CategoryRule{ "体检",         "医疗" },
    // 教育
    CategoryRule{ "书",           "教育" }, CategoryRule{ "课程",         "教育" },
    CategoryRule{ "网课",         "教育" }, CategoryRule{ "学费",         "教育" },
    CategoryRule{ "文具",         "教育" }, CategoryRule{ "考试",         "教育" },
    CategoryRule{ "一卡通",       "教育" }, CategoryRule{ "校园卡",       "教育" },
    CategoryRule{ "充值",         "教育" },
    // 人情（含转账 / 红包）
    CategoryRule{ "红包",         "人情" }, CategoryRule{ "转账",         "人情" },
    CategoryRule{ "份子",         "人情" }, CategoryRule{ "礼物",         "人情" },
    CategoryRule{ "话费充值",     "人情" },
};
constexpr std::array kIncomeRules = {
    CategoryRule{ "工资", "工资" },
    CategoryRule{ "薪",   "工资" },
    CategoryRule{ "奖金", "奖金" },
    CategoryRule{ "报销", "报销" },
    CategoryRule{ "退款", "退款" },
    CategoryRule{ "利息", "理财" },
    CategoryRule{ "红利", "理财" },
    CategoryRule{ "中奖", "其他" },
};

constexpr std::array kIncomeKeywords = {
    "工资", "薪", "奖金", "报销", "退款",
};

// 大小写不敏感：品牌名写法不统一（miHoYo / MIHOYO / luckin），
// 中文没有大小写，所以只对 ASCII 做折叠，不会误伤。
bool contains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty()) { return true; }
    if (needle.size() > haystack.size()) { return false; }

    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };

    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool same = true;
        for (std::size_t k = 0; k < needle.size(); ++k)
        {
            if (lower(haystack[i + k]) != lower(needle[k])) { same = false; break; }
        }
        if (same) { return true; }
    }
    return false;
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
    // 「转账」单列一类：微信账单里转账的商品列是 "/"，金额往往不小，
    // 混进"其他"会让消费结构完全失真。
    return { "餐饮", "交通", "购物", "居家", "娱乐", "医疗", "教育", "人情", "转账", "其他" };
}

std::vector<std::string> incomeCategories()
{
    return { "工资", "奖金", "报销", "退款", "理财", "转账", "其他" };
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
