#pragma once
// 「启动前自动生成汉化」（docs/08 §8.2.5）。
//
// 为什么必须"每次启动前生成"而不是"每版留一份"：PXC 的偏好目录是
// `Preferences/<PREF_VERSION>/`，而 `PREF_VERSION = 1_17_1` 写死在源码里
// （`scripts/globals/globals.ml:33`），所以所有版本共用一个 `keys.json`、只有一个 `local`，
// 同一个语言目录只能存一份，谁用谁写。
//
// 三条纪律（任何一条破了都会变成"启动被拦住"）：
//   ① 失败不拦启动：汉化不对是次要问题，拦住启动是灾难；
//   ② 内容没变就不写盘：语言目录里放 `.pxc_gen_stamp.json`（骨架版本 + 对照表版本），
//      两者都没变就跳过（191 ms → ~0）。PXC 只读固定文件名，不读它，无副作用；
//   ③ 语言不是本启动器管的就整个跳过：`keys.json` 的 `local` 与本启动器生成的名字不同，
//      说明用户在用别人的汉化包，重写只会打扰他。
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "baseline_scanner.h"
#include "locale_generator.h"
#include "store.h"
#include "translation_db.h"

namespace pxc_autogen {

struct Request {
    const pxc_tdb::Db* db = nullptr;
    const std::map<std::string, std::string>* overrides = nullptr;
    pxc_gen::ExprSet expr;
    pxc_gen::FontPick font;
    pxc_gen::Options opt;
    std::filesystem::path userDir;    // PXC 用户目录（含 Locale/）
    std::filesystem::path cacheRoot;  // Steam zip 解压缓存根
    std::vector<pxc_store::ExeEntry> registry;
    std::string langDir;              // 本启动器生成的语言目录名
    std::string installedLang;        // 当前 keys.json 里的 local（空 = 读不到）
};

struct Result {
    bool ran = false;      // 真的走了一遍（无论成败）
    bool ok = false;
    bool skipped = false;  // 骨架与对照表都没变，跳过写盘
    std::string message;   // 一行中文结论（进日志 / red toast）
    pxc_gen::Stats stats;
    pxc_base::Baseline base;
};

// 以即将启动的那个 exe 的版本生成一次。exe 为空或 version 为空 → 不做事（ran=false）。
Result beforeLaunch(const Request& req, const std::filesystem::path& exe, const std::string& version);

// 生成成功后落一份指纹（手动「生成并应用」也要落，否则自动生成会重复做一遍同样的活）。
void writeStamp(const std::filesystem::path& outRoot, const std::string& langDir,
                const std::string& skeletonVersion, const std::string& dbVersion);

} // namespace pxc_autogen
