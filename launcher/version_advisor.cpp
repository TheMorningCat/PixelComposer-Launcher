// 版本顾问实现：纯函数，无 IO。
#include "version_advisor.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <set>

namespace pxc_adv {

const char* familyName(Family f) {
    switch (f) {
        case Family::A60: return "A@11.6";
        case Family::A71: return "A@11.71";
        case Family::B80: return "B@11.80";
        case Family::CX:  return "C/PXCX";
        default:          return "未知";
    }
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

VKey parseVersion(const std::string& v) {
    VKey k;
    int part[4] = {0, 0, 0, 0};
    int n = 0;
    size_t i = 0;
    // 逐段取数字：跳过分隔符与非数字（rc/alpha/beta 之类后缀不参与主版本，落到 patch 位）。
    while (i < v.size() && n < 4) {
        while (i < v.size() && !isDigit(v[i])) ++i;
        if (i >= v.size()) break;
        long val = 0;
        while (i < v.size() && isDigit(v[i])) {
            if (val < 100000) val = val * 10 + (v[i] - '0');
            ++i;
        }
        part[n++] = (int)val;
    }
    if (n == 0) return k;  // ok 保持 false
    k.major = part[0];
    k.minor = part[1];
    k.patch = part[2];
    k.build = part[3];
    k.ok = true;
    return k;
}

Family familyOfVersion(const std::string& productVersion) {
    VKey k = parseVersion(productVersion);
    if (!k.ok) return Family::Unknown;
    if (k.major > 1) return Family::CX;      // 2.x 之后没理由再回到 JSON
    if (k.minor >= 20) return Family::CX;    // 1.20+ 换 PXCX 容器
    if (k.minor == 19) return Family::B80;   // 1.19 = zlib
    if (k.minor == 18) return Family::A71;   // 1.18 = JSON（字段比 1.17 多）
    return Family::A60;
}

Family familyOf(pxc::Format fmt, const std::string& productVersion) {
    switch (fmt) {
        case pxc::Format::PXCX: return Family::CX;
        case pxc::Format::ZLIB: return Family::B80;
        case pxc::Format::JSON: {
            VKey k = parseVersion(productVersion);
            if (!k.ok) return Family::A60;   // JSON 但读不到版本：按最早的容器算
            return (k.minor >= 18) ? Family::A71 : Family::A60;
        }
        default:
            // 格式识别不出时退回版本号推断；两者都没有就是未知。
            return familyOfVersion(productVersion);
    }
}

int versionDistance(const std::string& a, const std::string& b) {
    VKey x = parseVersion(a), y = parseVersion(b);
    if (!x.ok || !y.ok) return -1;
    long long d = 10000LL * std::abs(x.major - y.major)
                +   100LL * std::abs(x.minor - y.minor)
                +    10LL * std::abs(x.patch - y.patch)
                +            std::abs(x.build - y.build);
    if (d > INT_MAX) d = INT_MAX;
    return (int)d;
}

// 同距离时版本高者优先（新版本遇到老工程通常是"能读"，反过来不一定）。
static bool versionGreater(const std::string& a, const std::string& b) {
    VKey x = parseVersion(a), y = parseVersion(b);
    if (!x.ok || !y.ok) return false;
    if (x.major != y.major) return x.major > y.major;
    if (x.minor != y.minor) return x.minor > y.minor;
    if (x.patch != y.patch) return x.patch > y.patch;
    return x.build > y.build;
}

static std::string reasonFor(const Candidate& c, Family fileFamily, const std::string& fileVersion) {
    if (c.sameFamily) {
        if (c.distance == 0) return "结构相同、版本一致";
        if (c.distance > 0) return "结构相同，版本最接近（差 " + std::to_string(c.distance) + "）";
        return "结构相同";
    }
    if (c.distance >= 0) {
        std::string s = "结构不同（文件 " + std::string(familyName(fileFamily)) +
                        "，此版本 " + familyName(c.family) + "），跨结构打开会损坏文件";
        if (fileVersion.empty()) s = "结构不同（此版本 " + std::string(familyName(c.family)) +
                                     "），跨结构打开会损坏文件";
        return s;
    }
    return "版本号无法比对";
}

Advice advise(const std::vector<pxc_store::ExeEntry>& registry,
              const pxc::DetectionResult& det) {
    Advice a;
    a.confidence = det.confidence;
    a.fileVersion = det.version;
    a.fileFamily = familyOf(det.format, det.version);
    a.hasFile = (det.format != pxc::Format::UNKNOWN) || !det.version.empty();

    // 同一个 exe 可能因版本库里的历史登记出现两次，只留第一条。
    std::set<std::string> seen;
    for (const auto& e : registry) {
        if (e.exe_path.empty()) continue;
        if (!seen.insert(e.exe_path).second) continue;

        Candidate c;
        c.exe_path = e.exe_path;
        c.version = e.product_version;
        c.family = familyOfVersion(e.product_version);
        c.sameFamily = (a.fileFamily != Family::Unknown && c.family == a.fileFamily);
        c.distance = a.fileVersion.empty() ? -1 : versionDistance(a.fileVersion, e.product_version);
        c.reason = reasonFor(c, a.fileFamily, a.fileVersion);
        a.ordered.push_back(std::move(c));
    }

    std::stable_sort(a.ordered.begin(), a.ordered.end(),
                     [](const Candidate& x, const Candidate& y) {
                         if (x.sameFamily != y.sameFamily) return x.sameFamily;
                         int dx = (x.distance < 0) ? INT_MAX : x.distance;
                         int dy = (y.distance < 0) ? INT_MAX : y.distance;
                         if (dx != dy) return dx < dy;
                         return versionGreater(x.version, y.version);
                     });

    if (a.ordered.empty()) {
        a.headline = "本机还没有登记任何 PXC 版本。先去「版本库」页添加一个 exe。";
        a.detail.clear();
        return a;
    }

    const Candidate& top = a.ordered.front();
    if (top.sameFamily) {
        a.headline = "推荐 " + top.version +
                     (top.distance == 0 ? "：文件结构与它相同、版本也一致。"
                                        : "：文件结构与它相同，版本最接近。");
    } else if (a.fileFamily == Family::Unknown) {
        a.headline = "推荐 " + top.version + "：文件的结构与版本都没识别出来，只能按已登记版本挑。";
    } else {
        a.headline = "没有结构相同的已登记版本。按版本号最近推荐 " + top.version +
                     "，但跨结构打开会损坏文件。";
    }

    a.detail = "文件结构 " + std::string(familyName(a.fileFamily));
    if (a.fileVersion.empty()) a.detail += "（版本未识别）";
    else a.detail += "（版本 " + a.fileVersion + "）";
    a.detail += "；识别置信度 ";
    switch (a.confidence) {
        case pxc::Confidence::HIGH:   a.detail += "高"; break;
        case pxc::Confidence::MEDIUM: a.detail += "中"; break;
        case pxc::Confidence::LOW:    a.detail += "低"; break;
        default:                      a.detail += "无"; break;
    }
    a.detail += "。";
    return a;
}

} // namespace pxc_adv
