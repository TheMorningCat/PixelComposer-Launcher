// 搜索归一化与匹配规则（纯函数、无依赖，便于 headless 回归）。
//
// 抽成头文件的原因：这条规则曾经在 UI 里"看起来能搜"但实际漏了——用户改过译文后，
// 原名就再也搜不到了。做成纯函数后 `pxc_gen_test` 能直接断言行为，而不是靠人点。
//
// 规则：
//   1. 归一化：全角 ASCII（U+FF01–U+FF5E）与全角空格压成半角，再转小写；中日韩文字原样保留。
//   2. 命中：整串当子串命中任一字段（现用译文 / 对照表原名 / 英文 / ID），
//      或按空格切词后**每个词**都在任一字段里出现（顺序无关的模糊搜）。
//   3. 全程零分配：切词用下标而不用 substr，按键过滤不会抖动。
#pragma once

#include <cstddef>
#include <string>

namespace pxc_search {

// 全角 ASCII 与全角空格 → 半角，并转小写。
inline std::string fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (i + 2 < s.size()) {
            const unsigned char b2 = (unsigned char)s[i + 1];
            const unsigned char b3 = (unsigned char)s[i + 2];
            // 全角空格是 U+3000（E3 80 80），不落在 EF 区间里，必须单独判；漏掉它
            // 会出现"肉眼看起来只差一个空格却搜不到"。
            if (c == 0xE3 && b2 == 0x80 && b3 == 0x80) {
                out.push_back(' ');
                i += 3;
                continue;
            }
            if (c == 0xEF) {  // 全角 ASCII 区 U+FF01–U+FF5E
                const unsigned cp = ((unsigned)(c & 0x0Fu) << 12) | ((unsigned)(b2 & 0x3Fu) << 6) |
                                    (unsigned)(b3 & 0x3Fu);
                if (cp >= 0xFF01 && cp <= 0xFF5E) {
                    out.push_back((char)(cp - 0xFEE0));
                    i += 3;
                    continue;
                }
            }
        }
        out.push_back((char)c);
        ++i;
    }
    for (auto& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    return out;
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// field 中是否包含 q[qs, qs+qn)；不分配内存。
inline bool contains(const std::string& field, const std::string& q, size_t qs, size_t qn) {
    if (qn == 0) return true;
    if (field.size() < qn) return false;
    const size_t last = field.size() - qn;
    for (size_t i = 0; i <= last; ++i)
        if (field.compare(i, qn, q, qs, qn) == 0) return true;
    return false;
}

// fields 必须已 fold 过；query 也必须是 fold+trim 过的。
inline bool hit(const std::string* const* fields, size_t n, const std::string& query) {
    if (query.empty()) return true;

    // 1) 整串子串
    for (size_t i = 0; i < n; ++i)
        if (contains(*fields[i], query, 0, query.size())) return true;

    // 2) 多词：每个词都要出现（单词的情况上面已经覆盖，这里直接判失败）
    size_t count = 0;
    for (size_t a = 0; a < query.size();) {
        size_t b = query.find(' ', a);
        if (b == std::string::npos) b = query.size();
        if (b > a) ++count;
        a = b + 1;
    }
    if (count < 2) return false;

    for (size_t a = 0; a < query.size();) {
        size_t b = query.find(' ', a);
        if (b == std::string::npos) b = query.size();
        if (b > a) {
            bool any = false;
            for (size_t i = 0; i < n && !any; ++i) any = contains(*fields[i], query, a, b - a);
            if (!any) return false;
        }
        a = b + 1;
    }
    return true;
}

}  // namespace pxc_search
