// 最小 zip 读取器 —— 实现（见 locale_zip.h 的取舍说明）。
#include "locale_zip.h"

#include <cstring>

#include "io_util.h"
#include "util.h"

#include <zlib.h>

namespace pxc_zip {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;   // PK\x05\x06
constexpr uint32_t kCentralSig = 0x02014b50; // PK\x01\x02
constexpr uint32_t kLocalSig = 0x04034b50;   // PK\x03\x04

uint16_t rd16(const std::string& b, size_t off) {
    if (off + 2 > b.size()) return 0;
    return (uint16_t)((unsigned char)b[off] | ((unsigned char)b[off + 1] << 8));
}
uint32_t rd32(const std::string& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return (uint32_t)((unsigned char)b[off] | ((unsigned char)b[off + 1] << 8) |
                      ((unsigned char)b[off + 2] << 16) | ((unsigned char)b[off + 3] << 24));
}

// 解压单个条目到 out。data 是整个 zip 的字节，localOffset 指向本地头。
bool inflateEntry(const std::string& zipBytes, const Entry& e, std::string& out, std::string& err) {
    if (e.localOffset + 30 > zipBytes.size() || rd32(zipBytes, e.localOffset) != kLocalSig) {
        err = "本地文件头损坏：" + e.name;
        return false;
    }
    const uint16_t nameLen = rd16(zipBytes, e.localOffset + 26);
    const uint16_t extraLen = rd16(zipBytes, e.localOffset + 28);
    const size_t start = (size_t)e.localOffset + 30 + nameLen + extraLen;
    if (start + e.compSize > zipBytes.size()) {
        err = "条目数据越界：" + e.name;
        return false;
    }
    const char* src = zipBytes.data() + start;
    out.resize(e.rawSize);
    if (e.rawSize == 0) return true;
    if (e.method == 0) {
        std::memcpy(out.data(), src, e.rawSize);
        return true;
    }
    if (e.method != 8) {
        err = "不支持的压缩方式 " + std::to_string(e.method) + "：" + e.name;
        return false;
    }
    z_stream zs{};
    // -MAX_WBITS：raw deflate（zip 不带 zlib 头）
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        err = "zlib 初始化失败";
        return false;
    }
    zs.next_in = (Bytef*)src;
    zs.avail_in = e.compSize;
    zs.next_out = (Bytef*)out.data();
    zs.avail_out = e.rawSize;
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) {
        err = "解压失败（" + std::to_string(rc) + "）：" + e.name;
        return false;
    }
    return true;
}

// zip 内名字 -> 目的路径。拒绝绝对路径与 `..`，避免解到别处。
bool safeJoin(const fs::path& base, const std::string& relU8, fs::path& out) {
    std::string rel = relU8;
    for (char& c : rel)
        if (c == '\\') c = '/';
    if (rel.empty() || rel[0] == '/') return false;
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= rel.size()) {
        const size_t j = rel.find('/', i);
        const std::string seg = rel.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") return false;
        if (!seg.empty() && seg != ".") segs.push_back(seg);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (segs.empty()) return false;
    fs::path p = base;
    for (const auto& s : segs) p /= pxc_util::pathFromUtf8(s);
    out = p;
    return true;
}

} // namespace

bool list(const fs::path& zip, std::vector<Entry>& out, std::string& err) {
    out.clear();
    auto [bytes, ok] = pxc_io::readBytes(zip);
    if (!ok) {
        err = "读不出文件：" + pxc_util::toUtf8(zip.wstring().c_str());
        return false;
    }
    if (bytes.size() < 22) {
        err = "文件太小，不是 zip";
        return false;
    }
    // EOCD 从尾部往前找（22 字节 + 最多 64 KB 注释）
    size_t eocd = std::string::npos;
    const size_t lowest = bytes.size() > (22 + 65535) ? bytes.size() - (22 + 65535) : 0;
    for (size_t i = bytes.size() - 22;; --i) {
        if (rd32(bytes, i) == kEocdSig) {
            eocd = i;
            break;
        }
        if (i == lowest) break;
    }
    if (eocd == std::string::npos) {
        err = "找不到中央目录结尾（不是有效 zip）";
        return false;
    }
    const uint16_t count = rd16(bytes, eocd + 10);
    const uint32_t cdOffset = rd32(bytes, eocd + 16);
    if (cdOffset >= bytes.size()) {
        err = "中央目录偏移越界";
        return false;
    }
    size_t p = cdOffset;
    for (uint16_t k = 0; k < count; ++k) {
        if (p + 46 > bytes.size() || rd32(bytes, p) != kCentralSig) {
            err = "中央目录第 " + std::to_string((int)k + 1) + " 项损坏";
            out.clear();
            return false;
        }
        Entry e;
        e.method = rd16(bytes, p + 10);
        e.compSize = rd32(bytes, p + 20);
        e.rawSize = rd32(bytes, p + 24);
        const uint16_t nameLen = rd16(bytes, p + 28);
        const uint16_t extraLen = rd16(bytes, p + 30);
        const uint16_t commentLen = rd16(bytes, p + 32);
        e.localOffset = rd32(bytes, p + 42);
        if (p + 46 + nameLen > bytes.size()) {
            err = "中央目录文件名越界";
            out.clear();
            return false;
        }
        e.name.assign(bytes, p + 46, nameLen);
        out.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

int extractDir(const fs::path& zip, const std::string& prefix, const fs::path& destDir,
               bool includeFonts, std::string& err) {
    std::vector<Entry> entries;
    if (!list(zip, entries, err)) return 0;
    auto [bytes, ok] = pxc_io::readBytes(zip);
    if (!ok) {
        err = "读不出文件";
        return 0;
    }
    std::string stdPrefix = prefix;
    if (!stdPrefix.empty() && stdPrefix.back() != '/') stdPrefix += '/';

    int written = 0;
    std::error_code ec;
    for (const Entry& e : entries) {
        if (e.name.size() <= stdPrefix.size()) continue;
        if (e.name.compare(0, stdPrefix.size(), stdPrefix) != 0) continue;
        std::string rel = e.name.substr(stdPrefix.size());
        if (!rel.empty() && rel.back() == '/') continue;  // 目录项
        const bool isFont = rel.rfind("fonts/", 0) == 0;
        if (isFont && rel != "fonts/fonts.json") {
            if (!includeFonts) continue;
        }
        fs::path dst;
        if (!safeJoin(destDir, rel, dst)) continue;
        std::string data;
        std::string one;
        if (!inflateEntry(bytes, e, data, one)) {
            err = one;
            return written;
        }
        fs::create_directories(dst.parent_path(), ec);
        if (!pxc_io::writeBytes(dst, data)) {
            err = "写不出：" + pxc_util::toUtf8(dst.wstring().c_str());
            return written;
        }
        ++written;
    }
    if (written == 0) err = "zip 里没有 " + stdPrefix + " 下的文件";
    return written;
}

bool extractOne(const fs::path& zip, const std::string& name, const fs::path& dst, std::string& err) {
    std::vector<Entry> entries;
    if (!list(zip, entries, err)) return false;
    auto [bytes, ok] = pxc_io::readBytes(zip);
    if (!ok) {
        err = "读不出文件";
        return false;
    }
    for (const Entry& e : entries) {
        if (e.name != name) continue;
        std::string data;
        if (!inflateEntry(bytes, e, data, err)) return false;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        if (!pxc_io::writeBytes(dst, data)) {
            err = "写不出：" + pxc_util::toUtf8(dst.wstring().c_str());
            return false;
        }
        return true;
    }
    err = "zip 里没有 " + name;
    return false;
}

} // namespace pxc_zip
