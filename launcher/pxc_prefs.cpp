// PXC 偏好设置读写（见 pxc_prefs.h 的设计说明）。
#include "pxc_prefs.h"

#include <algorithm>
#include <chrono>
#include <windows.h>
#include <tlhelp32.h>

#include "io_util.h"
#include "util.h"

namespace pxc_prefs {

namespace fs = std::filesystem;
using pxc_store::JsonValue;

namespace {

// 偏好文件里存"当前语言目录名"的那个键。**是 local，不是 locale** ——
// 拼错就静默无效，所以只在这里写一次，别处一律引用。
constexpr const char* kLocalKey = "local";

bool isRegular(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

} // namespace

LocaleState probe(const fs::path& userDir) {
    LocaleState st;
    st.userDir = userDir;
    if (userDir.empty()) return st;

    // 1) Locale/ 下真实存在的语言目录（生成出来的、以及 PXC 自带的 en 都在这里）
    std::error_code ec;
    for (const auto& it : fs::directory_iterator(userDir / "Locale", ec)) {
        if (!it.is_directory(ec)) continue;
        st.available.push_back(pxc_util::toUtf8(it.path().filename().wstring().c_str()));
    }
    std::sort(st.available.begin(), st.available.end());

    // 2) Preferences/<构建号>/keys.json —— 构建号会随版本变，不能写死 1171，
    //    所以扫一层，取"有 local 键、且修改时间最新"的那份。
    fs::path best;
    std::string bestLocal;
    fs::file_time_type bestTime{};
    bool haveBest = false;
    std::error_code ec2;
    for (const auto& it : fs::directory_iterator(userDir / "Preferences", ec2)) {
        if (!it.is_directory(ec2)) continue;
        const fs::path kf = it.path() / "keys.json";
        if (!isRegular(kf)) continue;
        auto [root, ok] = pxc_io::readJson(kf);
        if (!ok || !root.isObj()) continue;
        PrefsFile pf;
        pf.path = kf;
        if (const JsonValue* v = root.find(kLocalKey); v && v->isStr()) pf.local = v->str;
        st.all.push_back(pf);
        st.readable = true;

        const auto wt = fs::last_write_time(kf, ec2);
        bool better = false;
        if (!haveBest) better = true;
        else if (!pf.local.empty() && bestLocal.empty()) better = true;          // 有 local 的优先
        else if ((pf.local.empty() == bestLocal.empty()) && !ec2 && wt > bestTime) better = true;
        if (better) {
            best = kf;
            bestLocal = pf.local;
            bestTime = ec2 ? fs::file_time_type{} : wt;
            haveBest = true;
        }
    }
    st.prefsFile = best;
    st.current = bestLocal;
    return st;
}

bool writeLocal(const fs::path& prefsFile, const std::string& lang, std::string& err) {
    if (prefsFile.empty()) {
        err = "没找到 PXC 的偏好文件（Preferences/<构建号>/keys.json）。";
        return false;
    }
    auto [root, ok] = pxc_io::readJson(prefsFile);
    if (!ok || !root.isObj()) {
        err = "偏好文件读不出来（不是有效 JSON），不敢改。";
        return false;
    }
    JsonValue val;
    val.type = JsonValue::Str;
    val.str = lang;
    if (JsonValue* cur = root.find(kLocalKey); cur) {
        if (cur->isStr() && cur->str == lang) return true;  // 已经是这个值
        *cur = val;                                        // 就地把值改掉，其它字段一个不动
    } else {
        root.obj.emplace_back(kLocalKey, val);
    }
    // 改之前留一份：万一 PXC 对格式有额外期待，维护者还能拿回原样。
    std::error_code ec;
    const fs::path bak = prefsFile.parent_path() / "keys.json.bak";
    fs::copy_file(pxc_util::toExtended(prefsFile), pxc_util::toExtended(bak),
                  fs::copy_options::overwrite_existing, ec);
    if (!pxc_io::writeJson(prefsFile, root)) {
        err = "写入偏好文件失败：" + pxc_util::toUtf8(prefsFile.wstring().c_str());
        return false;
    }
    return true;
}

bool pxcRunning() {
    // PXC 退出时会用内存设置整份覆盖 keys.json —— 那种情况下我们写什么都会被冲掉，
    // 所以调用方要先查这个。按进程名匹配，不按 pid（用户可能开了多个窗口）。
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"PixelComposer.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace pxc_prefs
