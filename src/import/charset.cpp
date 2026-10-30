#include "import/charset.hpp"

#include <fstream>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace import
{

namespace
{

bool hasUtf8Bom(std::string_view bytes)
{
    return bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
           static_cast<unsigned char>(bytes[1]) == 0xBB &&
           static_cast<unsigned char>(bytes[2]) == 0xBF;
}

}  // namespace

bool isValidUtf8(std::string_view bytes)
{
    std::size_t i = 0;
    while (i < bytes.size())
    {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);

        if (c < 0x80)
        {
            ++i;
            continue;
        }

        int extra = 0;
        if ((c & 0xE0) == 0xC0) { extra = 1; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; }
        else { return false; }

        if (i + static_cast<std::size_t>(extra) >= bytes.size()) { return false; }

        for (int k = 1; k <= extra; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0) != 0x80) { return false; }
        }

        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

std::optional<std::string> gbkToUtf8(std::string_view bytes)
{
#if defined(_WIN32)
    if (bytes.empty()) { return std::string{}; }

    const int wideLength = MultiByteToWideChar(936 /* CP_GBK */, 0, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
    if (wideLength <= 0) { return std::nullopt; }

    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(936, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                            wideLength) <= 0)
    {
        return std::nullopt;
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0,
                                               nullptr, nullptr);
    if (utf8Length <= 0) { return std::nullopt; }

    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), utf8Length, nullptr,
                            nullptr) <= 0)
    {
        return std::nullopt;
    }

    return utf8;
#else
    (void)bytes;
    return std::nullopt;   // 只支持 Windows 构建；别的平台交给调用方处理
#endif
}

std::expected<TextFile, std::string> readTextFile(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) { return std::unexpected("打不开文件: " + file.string()); }

    std::string bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    if (!in.good() && !in.eof()) { return std::unexpected("读文件失败: " + file.string()); }

    TextFile result;

    if (hasUtf8Bom(bytes))
    {
        result.utf8     = bytes.substr(3);
        result.encoding = "utf-8";
        return result;
    }

    if (isValidUtf8(bytes))
    {
        result.utf8     = bytes;
        result.encoding = "utf-8";
        return result;
    }

    if (const auto converted = gbkToUtf8(bytes))
    {
        result.utf8     = *converted;
        result.encoding = "gbk";
        return result;
    }

    // 两种都不是：原样返回，让上层看乱码总比直接失败强，但要留下痕迹。
    result.utf8     = std::move(bytes);
    result.encoding = "unknown";
    result.warnings.push_back("无法识别文件编码（既不是 UTF-8 也不是 GBK），中文可能显示为乱码");
    return result;
}

}  // namespace import
