#pragma once
// 显示表达式：把一条「英文 + 中文」按用户模板渲染成最终显示文本。
// 占位符只有两个：%en（英文）、%zh（中文）；其余字符原样输出。
// 例：%en[%zh] -> Outline[描边]；%zh - %en -> 描边 - Outline
#include <string>

namespace pxc_expr {

struct Options {
    bool fallbackEn = true;      // 中文缺失时回退英文
    bool cleanupBrackets = true; // 渲染后清理因空占位符残留的空括号对 [] ()（）
};

std::string render(const std::string& tmpl, const std::string& en, const std::string& zh,
                   const Options& opt = {});

} // namespace pxc_expr
