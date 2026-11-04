#pragma once
// 版本顾问：给定一个 .pxc 文件的结构信息，从"已登记的 PXC 版本"里排出一个推荐顺序。
//
// 这里只有纯计算，不读盘、不起进程、不碰 UI —— 目的就是能被 headless 测试逐条覆盖。
// 排序的第一关键字是**结构族**（同一套容器格式），第二才是**版本距离**：
// 版本号接近但容器换过（例如 1.19 的 zlib 与 1.20 的 PXCX）跨版本打开失败率远高于
// 版本号差得多但同结构的两个小版本。这也是为什么不能只按版本号排。
#include <string>
#include <vector>

#include "pxc_detect.h"
#include "store.h"

namespace pxc_adv {

// PXC 工程文件的容器结构族。同族 = 同一套序列化格式。
//   A60 : JSON 容器，1.17 及更早
//   A71 : JSON 容器，1.18.x（多了若干字段，仍是 JSON）
//   B80 : zlib 压缩容器，1.19.x
//   CX  : PXCX 容器，1.20 及以后
// 这些名字取自结构标记本身，不是产品版本号 —— 版本号会漂，容器换代的点是固定的。
enum class Family { Unknown, A60, A71, B80, CX };

const char* familyName(Family f);

// 拆 "1.18.5.0" / "1.18.5" / "1.21rc2" 这类串。ok=false 表示一个数字都没读到。
struct VKey {
    int major = 0, minor = 0, patch = 0, build = 0;
    bool ok = false;
};
VKey parseVersion(const std::string& v);

// 仅凭版本号推结构族（用于 exe：exe 没有"文件格式"这个输入）。
Family familyOfVersion(const std::string& productVersion);

// 有文件格式时优先用格式判（比版本号更准：1.22 里仍可能存着 1.18 的老工程）。
Family familyOf(pxc::Format fmt, const std::string& productVersion);

// 10000*|Δmajor| + 100*|Δminor| + 10*|Δpatch| + |Δbuild|。
// 任一侧解析失败返回 -1（= 无法比较，排序时排到最后）。
int versionDistance(const std::string& a, const std::string& b);

struct Candidate {
    std::string exe_path;   // UTF-8
    std::string version;
    Family family = Family::Unknown;
    bool sameFamily = false;
    int distance = -1;      // -1 = 无法比较
    std::string reason;     // 一句话理由，直接显示给用户
};

struct Advice {
    bool hasFile = false;                    // 文件侧是否有可用信息
    std::string fileVersion;                 // 识别到的产品版本（可能为空）
    Family fileFamily = Family::Unknown;
    pxc::Confidence confidence = pxc::Confidence::NONE;
    std::vector<Candidate> ordered;          // 已按推荐顺序排好，ordered[0] 就是推荐
    std::string headline;                    // 一句结论（弹窗标题下那行）
    std::string detail;                      // 更细的说明（结构/置信度/跨族风险）
};

// 已登记版本 → 排序后的推荐。registry 为空时 ordered 也为空。
Advice advise(const std::vector<pxc_store::ExeEntry>& registry,
              const pxc::DetectionResult& det);

} // namespace pxc_adv
