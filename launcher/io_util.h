#pragma once
// 文件读写工具：全程宽路径（_wfopen），规避 MSVC 对窄串的 GBK 解码。
// 读出的文本按 UTF-8 处理（PXC 的语言文件是 UTF-8，可能带 BOM）。
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

#include "store.h"

namespace pxc_io {

// 读整个文件；失败返回 (空串, false)。自动去掉 UTF-8 BOM。
inline std::pair<std::string, bool> readText(const std::filesystem::path& p) {
    FILE* fp = _wfopen(p.wstring().c_str(), L"rb");
    if (!fp) return {{}, false};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return {out, true};
}

inline bool writeText(const std::filesystem::path& p, const std::string& text) {
    FILE* fp = _wfopen(p.wstring().c_str(), L"wb");
    if (!fp) return false;
    if (!text.empty() && fwrite(text.data(), 1, text.size(), fp) != text.size()) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

inline bool writeBytes(const std::filesystem::path& p, const std::string& bytes) {
    return writeText(p, bytes);
}

inline std::pair<std::string, bool> readBytes(const std::filesystem::path& p) {
    FILE* fp = _wfopen(p.wstring().c_str(), L"rb");
    if (!fp) return {{}, false};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    return {out, true};
}

// 读并解析 JSON（JSONC 容错：store::parse 已支持注释/BOM/尾随逗号）。
inline std::pair<pxc_store::JsonValue, bool> readJson(const std::filesystem::path& p) {
    auto [text, ok] = readText(p);
    if (!ok) return {pxc_store::JsonValue{}, false};
    return pxc_store::parse(text);
}

inline bool writeJson(const std::filesystem::path& p, const pxc_store::JsonValue& v) {
    return writeText(p, pxc_store::serialize(v));
}

} // namespace pxc_io
