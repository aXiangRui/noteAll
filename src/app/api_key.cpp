#include "app/api_key.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace app
{

std::optional<std::string> readKeyFile(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // 去掉 UTF-8 BOM（记事本保存的文件常常带）
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }

    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };

    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) { --end; }
    std::string key = text.substr(begin, end - begin);

    if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front())
    {
        key = key.substr(1, key.size() - 2);
    }

    if (key.empty())
    {
        return std::nullopt;
    }
    return key;
}

std::vector<std::filesystem::path> apiKeySearchPaths(const std::filesystem::path& dataFile)
{
    std::vector<std::filesystem::path> paths;

    std::filesystem::path dir = dataFile.parent_path();
    if (dir.empty())
    {
        dir = ".";
    }
    paths.push_back(dir / "note.key");

#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && *appData != '\0')
    {
        paths.push_back(std::filesystem::path{ appData } / "noteAll" / "note.key");
    }
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        paths.push_back(std::filesystem::path{ home } / ".config" / "noteAll" / "note.key");
    }
#endif

    return paths;
}

std::optional<std::string> findApiKey(const std::filesystem::path& dataFile)
{
    if (const char* env = std::getenv("NOTE_LLM_KEY"); env != nullptr && *env != '\0')
    {
        return std::string{ env };
    }

    for (const auto& candidate : apiKeySearchPaths(dataFile))
    {
        if (auto key = readKeyFile(candidate))
        {
            return key;
        }
    }
    return std::nullopt;
}

}  // namespace app
