// `.pxc` 关联实现（HKCU only）。
#include "assoc.h"

#include "log.h"
#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <vector>

namespace pxc_assoc {

namespace {

std::string lastErr() {
    DWORD e = GetLastError();
    return "Windows 错误 " + std::to_string((unsigned long)e);
}

// 读 REG_SZ / REG_EXPAND_SZ；不存在或类型不对返回空。
std::string readSz(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    DWORD type = 0, size = 0;
    std::string out;
    if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
        std::wstring buf(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf.data(), &size) == ERROR_SUCCESS) {
            buf.resize(wcslen(buf.c_str()));
            out = pxc_util::toUtf8(buf.c_str());
        }
    }
    RegCloseKey(k);
    return out;
}

bool writeSz(HKEY root, const wchar_t* sub, const wchar_t* name, const std::wstring& value,
             std::string& err) {
    HKEY k = nullptr;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, &disp);
    if (rc != ERROR_SUCCESS) {
        err = "无法创建注册表项 " + pxc_util::toUtf8(sub) + "（" + std::to_string((long)rc) + "）";
        return false;
    }
    rc = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        err = "无法写入 " + pxc_util::toUtf8(sub) + "\\" +
              pxc_util::toUtf8(name ? name : L"(默认)") + "（" + std::to_string((long)rc) + "）";
        return false;
    }
    return true;
}

bool deleteTree(const wchar_t* sub) {
    // RegDeleteTreeW 会连同子键一起删；不存在不算失败。
    LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, sub);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

const wchar_t* kExtKey = L"Software\\Classes\\.pxc";
const wchar_t* kProgKey = L"Software\\Classes\\PXCLauncher.pxc";
const wchar_t* kUserChoice =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pxc\\UserChoice";

} // namespace

Info query() {
    Info info;
    info.progId = readSz(HKEY_CURRENT_USER, kExtKey, nullptr);
    info.userChoice = readSz(HKEY_CURRENT_USER, kUserChoice, L"ProgId");

    if (!info.userChoice.empty() && info.userChoice != kProgId) {
        info.state = State::Other;
        //  FriendlyTypeName 读不到就退 ProgId —— 至少让用户知道是谁占着
        const std::wstring other = L"Software\\Classes\\" +
                                   pxc_util::pathFromUtf8(info.userChoice).wstring();
        info.otherName = readSz(HKEY_CURRENT_USER, other.c_str(), L"FriendlyTypeName");
        if (info.otherName.empty()) info.otherName = info.userChoice;
        return info;
    }
    if (!info.progId.empty() && info.progId == kProgId) {
        info.state = State::Ours;
        return info;
    }
    info.state = State::None;
    return info;
}

bool registerSelf(const std::filesystem::path& launcherExe, std::string& previousProgId,
                  std::string& err) {
    err.clear();
    previousProgId = readSz(HKEY_CURRENT_USER, kExtKey, nullptr);   // 记住原来的，取消时还原
    if (previousProgId == kProgId) previousProgId.clear();

    const std::wstring exe = launcherExe.wstring();
    const std::wstring prog = pxc_util::pathFromUtf8(kProgId).wstring();
    const std::wstring classes = L"Software\\Classes\\";

    // .pxc：默认值 + OpenWithProgids（这一步只影响"打开方式"候选，不碰 UserChoice）
    if (!writeSz(HKEY_CURRENT_USER, kExtKey, nullptr, prog, err)) return false;
    if (!writeSz(HKEY_CURRENT_USER, (classes + L".pxc\\OpenWithProgids").c_str(), prog.c_str(), L"", err))
        return false;

    // ProgId 子树
    const std::wstring pk = classes + prog;
    if (!writeSz(HKEY_CURRENT_USER, pk.c_str(), nullptr, L"PXC 工程文件", err)) return false;
    if (!writeSz(HKEY_CURRENT_USER, pk.c_str(), L"FriendlyTypeName", L"PXC 工程文件", err)) return false;
    if (!writeSz(HKEY_CURRENT_USER, pk.c_str(), L"DefaultIcon", L"\"" + exe + L"\",0", err)) return false;
    if (!writeSz(HKEY_CURRENT_USER, (pk + L"\\shell\\open\\command").c_str(), nullptr,
                 L"\"" + exe + L"\" \"%1\"", err))
        return false;

    // 通知资源管理器：不通知的话要等下次登录图标才变，用户会以为没生效。
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    pxc_log::write("INFO", "Assoc", "已注册 .pxc 关联 → " + launcherExe.u8string());
    return true;
}

bool unregisterSelf(const std::string& previousProgId, std::string& err) {
    err.clear();
    // 只有默认值仍是我们时才动 .pxc，否则会把别人的关联一起删掉。
    const std::string cur = readSz(HKEY_CURRENT_USER, kExtKey, nullptr);
    bool ok = true;
    if (cur == kProgId) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kExtKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, nullptr);
            RegCloseKey(k);
        }
        // 只摘掉 OpenWithProgids 里我们自己那一条，别整片删（那是共享的）
        HKEY ow = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.pxc\\OpenWithProgids", 0,
                          KEY_WRITE, &ow) == ERROR_SUCCESS) {
            RegDeleteValueW(ow, pxc_util::pathFromUtf8(kProgId).c_str());
            RegCloseKey(ow);
        }
        if (!previousProgId.empty()) {
            std::string werr;
            if (!writeSz(HKEY_CURRENT_USER, kExtKey, nullptr,
                         pxc_util::pathFromUtf8(previousProgId).wstring(), werr)) {
                err = werr;
                ok = false;
            }
        }
    }
    if (!deleteTree(kProgKey)) {
        err = "无法删除 ProgId 子树（可能被占用）";
        ok = false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    pxc_log::write("INFO", "Assoc", ok ? "已取消 .pxc 关联" : "取消 .pxc 关联时出错：" + err);
    return ok;
}

bool openDefaultAppsSettings() {
    // ms-settings:defaultapps 由系统处理；ShellExecute 失败时退到控制面板。
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr,
                                SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

} // namespace pxc_assoc
