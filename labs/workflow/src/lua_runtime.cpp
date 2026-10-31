#include "workflow/lua_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "workflow/node_registry.hpp"

// Lua 运行时的实现。三个要点：
//   1. **沙箱**：只注册 base/string/table/math/utf8，没有 io/os/package/debug；
//   2. **指令预算**：count hook 超限就报错，防死循环（用户脚本不该能冻住界面）；
//   3. **热重载**：按文件 mtime 判断要不要重新加载；语法错时**保留上一版**，
//      不能让一次手滑把正在用的节点搞失效。
//
// 指令预算为什么用 lua_getextraspace 而不是闭包上值：
//   lua_sethook 收的是普通 C 函数指针，没有上值可挂；每个 lua_State 自带
//   LUA_EXTRASPACE 字节，用来放"这个 state 的预算指针"最直接。

namespace workflow
{

namespace
{

using Json = nlohmann::json;

struct Budget
{
    std::size_t used  = 0;
    std::size_t limit = 0;
};

constexpr int kHookCount = 10000;   // 每 1 万条指令被回调一次

void instructionHook(lua_State* L, lua_Debug* debug)
{
    if (debug->event != LUA_HOOKCOUNT) { return; }

    auto* budget = *static_cast<Budget**>(lua_getextraspace(L));
    if (budget == nullptr) { return; }

    budget->used += static_cast<std::size_t>(kHookCount);
    if (budget->used > budget->limit)
    {
        luaL_error(L, "脚本执行超过指令上限（%d 条），可能有死循环",
                   static_cast<int>(budget->limit));
    }
}

// Lua 值 → json。表按"1..n 连续整数键覆盖全部键"判定为数组，否则当对象。
Json toJson(lua_State* L, int index, int depth = 0)
{
    if (depth > 32) { return Json(nullptr); }   // 防自引用表无限递归

    index = lua_absindex(L, index);

    switch (lua_type(L, index))
    {
        // 注意全部用圆括号：nlohmann::json 的花括号会被当成 initializer_list，
        // Json{42} 是 [42] 而不是 42 —— 这个坑在测试里也踩过一次。
        case LUA_TNIL: return Json(nullptr);
        case LUA_TBOOLEAN: return Json(lua_toboolean(L, index) != 0);
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) { return Json(lua_tointeger(L, index)); }
            return Json(lua_tonumber(L, index));
        case LUA_TSTRING: return Json(std::string{ lua_tostring(L, index) });
        case LUA_TTABLE: break;
        default: return Json(nullptr);   // function/userdata/thread 不往 json 里搬
    }

    lua_Integer keyCount    = 0;
    lua_Integer arrayLength = 0;

    lua_pushnil(L);
    while (lua_next(L, index) != 0)
    {
        ++keyCount;
        if (lua_isinteger(L, -2))
        {
            const lua_Integer key = lua_tointeger(L, -2);
            if (key >= 1) { arrayLength = std::max(arrayLength, key); }
        }
        lua_pop(L, 1);
    }

    const bool isArray = arrayLength > 0 && arrayLength == keyCount;

    if (isArray)
    {
        Json array = Json::array();
        for (lua_Integer i = 1; i <= arrayLength; ++i)
        {
            lua_rawgeti(L, index, i);
            array.push_back(toJson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return array;
    }

    Json object = Json::object();
    lua_pushnil(L);
    while (lua_next(L, index) != 0)
    {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) { key = lua_tostring(L, -2); }
        else if (lua_isinteger(L, -2)) { key = std::to_string(lua_tointeger(L, -2)); }
        else
        {
            lua_pop(L, 1);
            continue;
        }

        object[key] = toJson(L, -1, depth + 1);
        lua_pop(L, 1);
    }
    return object;
}

void pushJson(lua_State* L, Json const& value)
{
    if (value.is_null()) { lua_pushnil(L); return; }
    if (value.is_boolean()) { lua_pushboolean(L, value.get<bool>() ? 1 : 0); return; }
    if (value.is_number_integer()) { lua_pushinteger(L, value.get<lua_Integer>()); return; }
    if (value.is_number_unsigned())
    {
        lua_pushinteger(L, static_cast<lua_Integer>(value.get<std::uint64_t>()));
        return;
    }
    if (value.is_number_float()) { lua_pushnumber(L, value.get<double>()); return; }
    if (value.is_string()) { lua_pushstring(L, value.get<std::string>().c_str()); return; }

    if (value.is_array())
    {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        int index = 1;
        for (const auto& item : value)
        {
            pushJson(L, item);
            lua_rawseti(L, -2, index++);
        }
        return;
    }

    lua_createtable(L, 0, static_cast<int>(value.size()));
    for (const auto& [key, item] : value.items())
    {
        pushJson(L, item);
        lua_setfield(L, -2, key.c_str());
    }
}

std::string mtimeText(std::filesystem::file_time_type time)
{
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    return std::to_string(seconds);
}

}  // namespace

struct ScriptRuntime::Impl
{
    struct Loaded
    {
        std::filesystem::file_time_type mtime{};
        int                             functionRef = LUA_NOREF;
        std::string                     error;   // 上次加载失败的原因（仍保留旧函数）
    };

    lua_State*                              state = nullptr;
    ScriptSettings                          settings;
    std::unordered_map<std::string, Loaded> scripts;

    ~Impl()
    {
        if (state != nullptr) { lua_close(state); }
    }

    void ensureState()
    {
        if (state != nullptr) { return; }

        state = luaL_newstate();
        if (state == nullptr) { return; }

        // 白名单式开库：io / os / package / debug 一个都不开
        luaL_requiref(state, "_G", luaopen_base, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);

        // base 库里那些能碰文件/动态加载的入口一律摘掉
        for (const char* name : { "dofile", "loadfile", "load", "require", "collectgarbage" })
        {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
    }
};

ScriptRuntime::ScriptRuntime(ScriptSettings settings) : impl_(new Impl{})
{
    impl_->settings = settings;
    impl_->ensureState();
}

ScriptRuntime::~ScriptRuntime()
{
    delete impl_;
}

void ScriptRuntime::reload(std::optional<std::filesystem::path> const& scriptPath)
{
    if (impl_->state == nullptr) { return; }

    if (!scriptPath)
    {
        for (auto& [path, loaded] : impl_->scripts)
        {
            if (loaded.functionRef != LUA_NOREF)
            {
                luaL_unref(impl_->state, LUA_REGISTRYINDEX, loaded.functionRef);
            }
        }
        impl_->scripts.clear();
        return;
    }

    const std::string key = scriptPath->string();
    if (auto it = impl_->scripts.find(key); it != impl_->scripts.end())
    {
        if (it->second.functionRef != LUA_NOREF)
        {
            luaL_unref(impl_->state, LUA_REGISTRYINDEX, it->second.functionRef);
        }
        impl_->scripts.erase(it);
    }
}

std::map<std::string, std::string> ScriptRuntime::cachedScripts() const
{
    std::map<std::string, std::string> result;
    for (const auto& [path, loaded] : impl_->scripts)
    {
        result[path] = loaded.error.empty() ? mtimeText(loaded.mtime) : "错误: " + loaded.error;
    }
    return result;
}

std::expected<Json, std::string> ScriptRuntime::call(const std::filesystem::path& scriptPath,
                                                     std::string const&            entry,
                                                     Json const&                   input)
{
    if (impl_->state == nullptr) { return std::unexpected("Lua 初始化失败"); }

    std::error_code ec;
    const auto      mtime = std::filesystem::last_write_time(scriptPath, ec);
    if (ec) { return std::unexpected("读不到脚本: " + scriptPath.string()); }

    const std::string key  = scriptPath.string();
    auto&             slot = impl_->scripts[key];

    // ---- 热重载：文件变了才重新加载 ----
    if (slot.functionRef == LUA_NOREF || slot.mtime != mtime)
    {
        lua_State* L = impl_->state;

        if (luaL_loadfile(L, key.c_str()) != LUA_OK)
        {
            const std::string message =
                lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "脚本加载失败";
            lua_pop(L, 1);

            // 语法错误：保留旧函数，只记错误（一次手滑不该让节点失效）
            slot.error = message;
            if (slot.functionRef == LUA_NOREF) { return std::unexpected(message); }
        }
        else if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            const std::string message =
                lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "脚本执行失败";
            lua_pop(L, 1);
            slot.error = message;
            if (slot.functionRef == LUA_NOREF) { return std::unexpected(message); }
        }
        else
        {
            lua_getglobal(L, entry.c_str());
            if (!lua_isfunction(L, -1))
            {
                lua_pop(L, 1);
                const std::string message = "脚本里找不到函数 " + entry + "()（" + key + "）";
                slot.error                = message;
                return std::unexpected(message);
            }

            if (slot.functionRef != LUA_NOREF)
            {
                luaL_unref(L, LUA_REGISTRYINDEX, slot.functionRef);
            }
            slot.functionRef = luaL_ref(L, LUA_REGISTRYINDEX);
            slot.error.clear();
            slot.mtime = mtime;
        }
    }

    if (slot.functionRef == LUA_NOREF)
    {
        return std::unexpected(slot.error.empty() ? "脚本未加载成功" : slot.error);
    }

    lua_State* L = impl_->state;

    // 指令预算：每次调用重新开一个，跑完撤掉
    Budget budget{ 0, impl_->settings.instructionLimit };
    *static_cast<Budget**>(lua_getextraspace(L)) = &budget;
    lua_sethook(L, instructionHook, LUA_MASKCOUNT, kHookCount);

    lua_rawgeti(L, LUA_REGISTRYINDEX, slot.functionRef);
    pushJson(L, input);

    const int status = lua_pcall(L, 1, 1, 0);

    lua_sethook(L, nullptr, 0, 0);

    if (status != LUA_OK)
    {
        const std::string message =
            lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "脚本运行失败";
        lua_pop(L, 1);
        return std::unexpected(message);
    }

    Json output = toJson(L, -1);
    lua_pop(L, 1);
    return output;
}

}  // namespace workflow
