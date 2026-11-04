#include "version_probe.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "log.h"
#include "pxc_detect.h"
#include "store.h"

namespace pxc_probe {
namespace {

struct CacheEntry {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string ver;      // 探测结果；空串 = 扫过但没找到（也是有效结论，要缓存）
};

std::map<std::string, CacheEntry> g_cache;   // key = exe 路径（UTF-8，绝对）
bool g_dirty = false;
int g_scans = 0;

// 文件身份：大小 + 修改时间。任一读不到就返回 false（此时不缓存 —— 缓存一个"读不到"的结论
// 会把一次临时故障永久化）。
bool stampOf(const std::filesystem::path& exe, std::uint64_t& size, std::int64_t& mtime) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(exe, ec);
    if (ec) return false;
    const auto t = std::filesystem::last_write_time(exe, ec);
    if (ec) return false;
    size = (std::uint64_t)sz;
    // 毫秒级：file_time 的原始 count 量级 1.3e17 超过 double 的 53 位精度，
    // 直接存进 JSON（Num 是 double）会被抹平 → 每次比较都不相等 → 缓存永远失效。
    mtime = (std::int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                t.time_since_epoch()).count();
    return true;
}

} // namespace

std::string embeddedVersion(const std::filesystem::path& exe, const std::string& peVersion) {
    if (peVersion.empty() || exe.empty()) return "";
    const std::string key = exe.u8string();
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    const bool stamped = stampOf(exe, size, mtime);
    if (stamped) {
        const auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.size == size && it->second.mtime == mtime)
            return it->second.ver;
    }
    const std::string ver = pxc::readEmbeddedVersion(exe, peVersion);
    ++g_scans;
    if (stamped) {
        CacheEntry e;
        e.size = size;
        e.mtime = mtime;
        e.ver = ver;
        g_cache[key] = e;
        g_dirty = true;
    }
    return ver;
}

void load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return;
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) return;
    auto parsed = pxc_store::parse(text);
    if (!parsed.second) {
        pxc_log::write("WARN", "Probe", "版本探测缓存解析失败，忽略：" + file.u8string());
        return;
    }
    const auto* arr = parsed.first.find("entries");
    if (!arr || !arr->isArr()) return;
    for (const auto& o : arr->arr) {
        if (!o.isObj()) continue;
        const auto* p = o.find("path");
        if (!p || !p->isStr() || p->str.empty()) continue;
        CacheEntry e;
        // size / mtime 存成**字符串**而不是 JSON 数字：store 的数字序列化是 %.10g（给 .pxc 用的，
        // 10 位有效数字就够）。毫秒时间戳是 13 位 → 会被截成 1.337820025e+12，
        // 于是每次读回来都和真实值不等 → 缓存永远失效。这不是理论问题，是实测踩到的。
        auto getU64 = [&](const char* k, std::uint64_t& dst) {
            if (const auto* f = o.find(k)) {
                if (f->type == pxc_store::JsonValue::Str) dst = std::strtoull(f->str.c_str(), nullptr, 10);
                else if (f->type == pxc_store::JsonValue::Num) dst = (std::uint64_t)f->num;
            }
        };
        auto getI64 = [&](const char* k, std::int64_t& dst) {
            if (const auto* f = o.find(k)) {
                if (f->type == pxc_store::JsonValue::Str) dst = (std::int64_t)std::strtoll(f->str.c_str(), nullptr, 10);
                else if (f->type == pxc_store::JsonValue::Num) dst = (std::int64_t)f->num;
            }
        };
        getU64("size", e.size);
        getI64("mtime", e.mtime);
        if (const auto* v = o.find("ver")) if (v->type == pxc_store::JsonValue::Str) e.ver = v->str;
        g_cache[p->str] = e;
    }
    g_dirty = false;
    pxc_log::write("INFO", "Probe", "版本探测缓存载入 " + std::to_string(g_cache.size()) + " 条");
}

void flush(const std::filesystem::path& file) {
    if (!g_dirty) return;
    pxc_store::JsonValue root;
    root.type = pxc_store::JsonValue::Obj;
    pxc_store::JsonValue ver;
    ver.type = pxc_store::JsonValue::Num;
    ver.num = 1.0;
    root.obj.emplace_back("v", ver);
    pxc_store::JsonValue arr;
    arr.type = pxc_store::JsonValue::Arr;
    for (const auto& kv : g_cache) {
        pxc_store::JsonValue o;
        o.type = pxc_store::JsonValue::Obj;
        auto putStr = [&o](const char* k, const std::string& v) {
            pxc_store::JsonValue n;
            n.type = pxc_store::JsonValue::Str;
            n.str = v;
            o.obj.emplace_back(k, std::move(n));
        };
        putStr("path", kv.first);
        putStr("size", std::to_string(kv.second.size));
        putStr("mtime", std::to_string(kv.second.mtime));
        putStr("ver", kv.second.ver);
        arr.arr.push_back(std::move(o));
    }
    root.obj.emplace_back("entries", std::move(arr));
    const std::string text = pxc_store::serialize(root, 2);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, file.wstring().c_str(), L"wb") != 0 || !fp) {
        pxc_log::write("WARN", "Probe", "版本探测缓存写不进去：" + file.u8string());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), fp);
    std::fclose(fp);
    g_dirty = false;
}

int scanCount() { return g_scans; }

std::size_t size() { return g_cache.size(); }

} // namespace pxc_probe
