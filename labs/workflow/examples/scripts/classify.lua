-- 账单分类节点（示例）
--
-- 用法：流程 JSON 里写 { "type": "script", "script": "examples/scripts/classify.lua" }
--
-- 脚本看到的东西（和 {{ 节点id.字段 }} 同一套命名）：
--   input["in"].note           上游 input 节点的 note
--   input["in"].amount_cents   金额（整数分，永远别用浮点算钱）
--   input.__node               本节点 id
--
-- 返回的 table 就是这个节点的输出，下游用 {{ classify.category }} 引用。
--
-- ⚠️ Lua 的标识符只认 ASCII：`分类 = "餐饮"` 是语法错误，必须写成
--    `["分类"] = "餐饮"`。下面用英文键，既安全也不需要引号。
--
-- 为什么金额用整数分：32.50 元是 3250 分。浮点会出现 0.1 + 0.2 ~= 0.3，
-- 而账本里差几毛钱是最难查的问题。

function run(input)
    local bill  = input["in"] or {}
    local note  = tostring(bill.note or "")
    local cents = tonumber(bill.amount_cents or 0) or 0

    -- 先用关键词：比让模型判断更快、更便宜、也更容易预测
    local rules = {
        { "麦当劳", "餐饮" }, { "肯德基", "餐饮" }, { "外卖", "餐饮" },
        { "咖啡", "餐饮" },   { "奶茶", "餐饮" },   { "饭", "餐饮" },
        { "打车", "交通" },   { "地铁", "交通" },   { "单车", "交通" },
        { "超市", "购物" },   { "便利店", "购物" },
        { "房租", "居家" },   { "水费", "居家" },   { "电费", "居家" },
        { "电影", "娱乐" },   { "游戏", "娱乐" },
    }

    for _, rule in ipairs(rules) do
        if string.find(note, rule[1], 1, true) then
            return { category = rule[2], basis = "关键词:" .. rule[1] }
        end
    end

    -- 认不出来就明确说"需人工确认"，不要瞎猜 —— 分类错了比没分类更难发现
    if cents >= 100000 then
        return { category = nil, needs_review = true, basis = "金额较大且认不出商户" }
    end

    return { category = "其他", basis = "无关键词命中" }
end
