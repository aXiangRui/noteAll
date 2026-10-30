#include "import/csv.hpp"

namespace import
{

CsvTable parseCsv(std::string_view text, char delimiter)
{
    CsvTable table;

    std::size_t pos = 0;

    // UTF-8 BOM：微信导出的账单第一行就带这个，不去掉的话第一个列名会变成 "\xEF\xBB\xBF交易时间"，
    // 后面按列名找列会全部对不上 —— 这是最典型的"读进来但读错了"。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        pos = 3;
    }

    std::vector<std::string> row;
    std::string              field;
    bool                     inQuotes = false;
    bool                     hasContent = false;   // 这一行是否已经有东西（用于跳过纯空行）

    // 收一个字段
    const auto endField = [&] {
        row.push_back(std::move(field));
        field.clear();
        hasContent = true;
    };

    // 收一行
    const auto endRow = [&] {
        row.push_back(std::move(field));
        field.clear();

        const bool blank = row.size() == 1 && row[0].empty();
        if (!blank) { table.rows.push_back(std::move(row)); }
        row.clear();
        hasContent = false;
    };

    while (pos < text.size())
    {
        const char c = text[pos++];

        if (inQuotes)
        {
            if (c == '"')
            {
                // "" → 一个字面引号；否则引号结束
                if (pos < text.size() && text[pos] == '"')
                {
                    field.push_back('"');
                    ++pos;
                }
                else
                {
                    inQuotes = false;
                }
            }
            else
            {
                field.push_back(c);   // 引号内的 \r \n , 都是普通字符
            }
            continue;
        }

        if (c == '"')
        {
            inQuotes   = true;
            hasContent = true;
            continue;
        }

        if (c == delimiter)
        {
            endField();
            continue;
        }

        if (c == '\r')
        {
            // \r\n 当一个换行；单独的 \r（老 Mac）也当换行
            if (pos < text.size() && text[pos] == '\n') { ++pos; }
            endRow();
            continue;
        }

        if (c == '\n')
        {
            endRow();
            continue;
        }

        field.push_back(c);
        hasContent = true;
    }

    if (inQuotes)
    {
        table.warnings.push_back("引号没有闭合：已把文件剩余内容当作该字段（账单文件可能被截断了）");
    }

    // 收尾：最后一行没有换行符时也要收；但如果文件以换行结束，这里什么都不该产出。
    if (hasContent || inQuotes || !field.empty() || !row.empty())
    {
        row.push_back(std::move(field));
        field.clear();

        const bool blank = row.size() == 1 && row[0].empty();
        if (!blank) { table.rows.push_back(std::move(row)); }
    }

    return table;
}

}  // namespace import
