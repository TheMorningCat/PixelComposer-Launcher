#include "expr.h"

namespace pxc_expr {
namespace {

// 清理空括号对：把 "Outline[]" / "Outline()" / "Outline（）" 收成 "Outline"。
// 只清理紧邻的空对，不动用户有意书写的其它符号。
void cleanupEmpty(std::string& s) {
    static const char* pairs[][2] = {{"[]", ""}, {"()", ""}, {"\xEF\xBC\x88\xEF\xBC\x89", ""}};
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& p : pairs) {
            size_t pos = s.find(p[0]);
            if (pos != std::string::npos) {
                s.erase(pos, std::string(p[0]).size());
                changed = true;
            }
        }
    }
}

} // namespace

std::string render(const std::string& tmpl, const std::string& en, const std::string& zh,
                   const Options& opt) {
    std::string out;
    out.reserve(tmpl.size() + en.size() + zh.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%' || i + 1 >= tmpl.size()) { out.push_back(tmpl[i]); continue; }
        char c1 = tmpl[i + 1];
        if ((c1 == 'e' || c1 == 'z') && i + 2 < tmpl.size()) {
            if (c1 == 'e' && tmpl[i + 2] == 'n') { out += en; i += 2; continue; }
            if (c1 == 'z' && tmpl[i + 2] == 'h') {
                std::string v = zh;
                if (v.empty() && opt.fallbackEn) v = en;
                out += v;
                i += 2;
                continue;
            }
        }
        out.push_back(tmpl[i]); // 非占位符的 '%' 原样保留
    }
    if (opt.cleanupBrackets) cleanupEmpty(out);
    return out;
}

} // namespace pxc_expr
