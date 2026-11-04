#pragma once
// 路径编码工具：MSVC 的 std::filesystem::path 对窄字符串按 ANSI(GBK) 解码，
// 故所有"UTF-8 字符串 -> path"必须显式转宽路径；"path -> UTF-8"仅用于 JSON/显示。
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pxc_util {

inline std::string toUtf8(const wchar_t* ws) {
    if (!ws) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline std::filesystem::path pathFromUtf8(const std::string& u8) {
    if (u8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return std::filesystem::path(u8); // 退化：纯 ASCII
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return std::filesystem::path(w);
}

// 窄串(UTF-8) 拼到路径上。**任何** std::string 参与路径拼接都必须走这里：
// MSVC 的 path(const std::string&) 按 ANSI(GBK) 解码，遇到合法 UTF-8 但不是合法 GBK 的字节串
// （例如奇数个中文字节收尾）会直接抛 std::system_error 1113「在多字节的目标代码页中，
// 没有此 Unicode 字符可以映射到的字符」。这类异常只发生在 string→path，
// 排查时先看是不是哪里写了 `path / std::string`。
inline std::filesystem::path joinUtf8(const std::filesystem::path& base, const std::string& u8) {
    if (u8.empty()) return base;
    return base / pathFromUtf8(u8);
}

// 异常 what() / 系统错误消息可能是 ANSI(GBK)，转成 UTF-8 再显示或写日志。
inline std::string autoToUtf8(const std::string& s) {
    if (s.empty()) return s;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    if (n > 0) return s; // 本身就是合法 UTF-8
    int m = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (m <= 0) return s;
    std::wstring w(m, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), w.data(), m);
    return toUtf8(w.c_str());
}

// 把用户输入清洗成合法的 Windows 目录名（保留中文，去掉 < > : " / \ | ? * 与控制字符）。
inline std::string sanitizeDirName(const std::string& u8) {
    std::wstring w;
    std::filesystem::path p = pathFromUtf8(u8);
    w = p.wstring();
    std::wstring out;
    for (wchar_t c : w) {
        if (c < 0x20) continue;
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
            c == L'|' || c == L'?' || c == L'*')
            continue;
        out.push_back(c);
    }
    // 去掉首尾空格与点（Windows 会静默截断）
    while (!out.empty() && (out.front() == L' ' || out.front() == L'.')) out.erase(out.begin());
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return toUtf8(out.c_str());
}

// 转扩展长度路径（\\?\C:\...），突破 Win32 MAX_PATH(260) 限制。
// 仅对绝对盘符路径加前缀；UNC/相对路径原样返回。
inline std::filesystem::path toExtended(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    std::wstring s = abs.native(); // Windows 上为本机分隔符（反斜杠）
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') return abs; // 已是扩展/UNC
    if (s.size() >= 2 && s[1] == L':') return std::filesystem::path(L"\\\\?\\" + s);
    return abs;
}

} // namespace pxc_util
