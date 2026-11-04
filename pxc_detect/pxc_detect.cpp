#include "pxc_detect.h"
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <windows.h>
#include "zlib.h"

namespace pxc {

static std::wstring utf16(const std::string& s) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wn);
    return w;
}
static std::string utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string o(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &o[0], n, nullptr, nullptr);
    return o;
}

static std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

static bool isZlibHeader(const std::string& b) {
    if (b.size() < 2) return false;
    if (b[0] != 0x78) return false;
    unsigned char m = (unsigned char)b[1];
    return m == 0x9c || m == 0xda || m == 0x01 || m == 0x5e ||
           m == 0xbb || m == 0x3e;
}

// Inflate a zlib (RFC1950) stream. Returns "" on error.
static std::string inflateZlib(const std::string& in) {
    z_stream zs;
    zs.zalloc = Z_NULL; zs.zfree = Z_NULL; zs.opaque = Z_NULL;
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return "";  // 15+32: zlib OR gzip autodetect
    std::string out;
    char buf[65536];
    int rc;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return "";
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (zs.avail_out == 0);
    inflateEnd(&zs);
    return out;
}

// Find the integer immediately following the first `"version" :` key.
static bool scanVersionInt(const std::string& s, long& out) {
    size_t pos = 0;
    const std::string key = "\"version\"";
    while ((pos = s.find(key, pos)) != std::string::npos) {
        size_t q = pos + key.size();
        while (q < s.size() && (s[q]==' '||s[q]=='\t'||s[q]=='\n'||s[q]=='\r')) q++;
        if (q < s.size() && s[q] == ':') {
            q++;
            while (q < s.size() && (s[q]==' '||s[q]=='\t'||s[q]=='\n'||s[q]=='\r')) q++;
            size_t start = q;
            if (q < s.size() && (s[q]=='-'||s[q]=='+')) q++;
            while (q < s.size() && std::isdigit((unsigned char)s[q])) q++;
            if (q > start) {
                try { out = std::stol(s.substr(start, q - start)); return true; }
                catch (...) {}
            }
        }
        pos += key.size();
    }
    return false;
}

// Collect a coarse fingerprint (node type tokens + top-level keys) for fallback.
static std::string fingerprint(const std::string& s) {
    size_t ntype = 0;
    size_t p = 0;
    while ((p = s.find("\"type\"", p)) != std::string::npos) {
        // count "type":"Node_..." occurrences
        size_t q = p + 6;
        while (q < s.size() && (s[q]==' '||s[q]=='\t'||s[q]=='\n'||s[q]=='\r')) q++;
        if (q < s.size() && s[q]==':') {
            q++;
            while (q < s.size() && (s[q]==' '||s[q]=='\t'||s[q]=='\n'||s[q]=='\r')) q++;
            if (q+1 < s.size() && s[q]=='"' && s[q+1]=='N') ntype++;
        }
        p += 6;
    }
    size_t nver = 0;
    p = 0;
    while ((p = s.find("\"version\"", p)) != std::string::npos) { nver++; p += 9; }
    return "types=" + std::to_string(ntype) + ",versionKeys=" + std::to_string(nver);
}

// Calibration: internal version integer -> product version string.
// (The integer encoding is non-linear across PXC builds; thus a table, not a formula.)
// Returns confidence too:
//   - hit table                 -> HIGH
//   - read int but not in table -> near-neighbor infer major.minor -> MEDIUM (design §3.2)
//   - otherwise                 -> LOW
struct CalibResult { std::string version; Confidence conf; };
static CalibResult calib(long raw) {
    // 表里一律写**完整四段**（x.y.z.0）。早先三条写成了三段（"1.18.5"），
    // 而版本库里登记的是 exe 的四段版本 → 明明装了 1.18.5.0 却匹配不上、弹协商窗。
    // 规范化在 pxc_reg::normalizeVersion 还有一层兜底，但源头就不该省。
    static const std::map<long, std::string> m = {
        {11610,  "1.16.1.0"},
        {11690,  "1.17.0.0"},
        {11700,  "1.17.9.0"},
        {117100, "1.18.5.0"},
        {118091, "1.19.0.11"},
    };
    auto it = m.find(raw);
    if (it != m.end()) return {it->second, Confidence::HIGH};

    // near-neighbor for unmapped-but-read integer
    if (m.empty()) return {"", Confidence::LOW};
    long minK = m.begin()->first;
    if (raw < minK) return {"", Confidence::LOW};   // older than any known -> truly unknown
    long nearest = minK, best = std::abs(minK - raw);
    for (auto& kv : m) {
        long d = std::abs(kv.first - raw);
        if (d < best) { best = d; nearest = kv.first; }
    }
    long band = (long)(nearest * 0.02);                       // allow small patch-level offset
    if (band < 50) band = 50;
    if (best <= band) return {m.at(nearest), Confidence::MEDIUM};
    return {"", Confidence::LOW};
}

DetectionResult detectPxc(const std::filesystem::path& path) {
    DetectionResult r;
    std::string b = readFile(path);
    if (b.empty()) { r.note = "empty/unreadable"; return r; }
    size_t n = b.size();

    if (n >= 4 && std::memcmp(b.data(), "PXCX", 4) == 0) {
        r.format = Format::PXCX;
        size_t p = b.find("META");
        if (p != std::string::npos && p + 8 <= n) {
            uint32_t plen = 0;
            std::memcpy(&plen, b.data() + p + 4, 4); // LE chunk length
            const char* vs = b.data() + p + 8 + 4;    // skip 4-byte build number
            size_t maxlen = (plen > 4) ? (plen - 4) : 0;
            size_t end = strnlen(vs, maxlen);
            r.version = std::string(vs, end);
            r.rawValue = r.version;
            r.confidence = Confidence::HIGH;
        } else {
            r.confidence = Confidence::LOW;
            r.note = "PXCX but no META version";
        }
        return r;
    }

    if (isZlibHeader(b)) {
        r.format = Format::ZLIB;
        std::string js = inflateZlib(b);
        if (js.empty()) { r.confidence = Confidence::LOW; r.note = "zlib inflate failed"; return r; }
        long raw = 0;
        if (scanVersionInt(js, raw)) {
            r.rawValue = std::to_string(raw);
            CalibResult cr = calib(raw);
            if (!cr.version.empty()) {
                r.version = cr.version; r.confidence = cr.conf;
                if (cr.conf == Confidence::MEDIUM) r.note = "unmapped integer; near-neighbor inferred (MEDIUM)";
            } else { r.confidence = Confidence::LOW; r.note = "unmapped integer (needs calibration)"; r.fingerprint = fingerprint(js); }
        } else { r.confidence = Confidence::LOW; r.note = "no version int in payload"; r.fingerprint = fingerprint(js); }
        return r;
    }

    // JSON (format A): try direct parse of version int
    size_t i = 0;
    if (n >= 3 && (unsigned char)b[0]==0xef && (unsigned char)b[1]==0xbb && (unsigned char)b[2]==0xbf) i = 3;
    while (i < n && (b[i]==' '||b[i]=='\n'||b[i]=='\r'||b[i]=='\t')) i++;
    if (i < n && (b[i]=='{' || b[i]=='[')) {
        r.format = Format::JSON;
        long raw = 0;
        if (scanVersionInt(b, raw)) {
            r.rawValue = std::to_string(raw);
            CalibResult cr = calib(raw);
            if (!cr.version.empty()) {
                r.version = cr.version; r.confidence = cr.conf;
                if (cr.conf == Confidence::MEDIUM) r.note = "unmapped integer; near-neighbor inferred (MEDIUM)";
            } else { r.confidence = Confidence::LOW; r.note = "unmapped integer (needs calibration)"; r.fingerprint = fingerprint(b); }
        } else { r.confidence = Confidence::LOW; r.note = "no version int"; r.fingerprint = fingerprint(b); }
        return r;
    }

    r.format = Format::UNKNOWN;
    r.confidence = Confidence::NONE;
    r.note = "unrecognized format";
    return r;
}

std::string readExeVersion(const std::filesystem::path& exePath) {
    DWORD sz = GetFileVersionInfoSizeW(exePath.c_str(), nullptr);
    if (!sz) return "";
    std::vector<char> buf(sz);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, sz, buf.data())) return "";
    struct LANGANDCODEPAGE { WORD lang; WORD cp; } *lp;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&lp, &len)) return "";
    wchar_t sub[64];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\ProductVersion", lp[0].lang, lp[0].cp);
    wchar_t* p = nullptr;
    if (!VerQueryValueW(buf.data(), sub, (void**)&p, &len) || !p) return "";
    return utf8(p);
}

// ---------- 内嵌数据包版本扫描（§11.8.2） ----------
namespace {

// 严格解析 a.b.c.d：四段、每段 1~5 位、无空段。不做任何"补 0"猜测。
bool parseFourSeg(const char* s, std::size_t n, int out[4]) {
    if (n < 7 || n > 23) return false;
    int seg = 0, val = 0, digits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 5) return false;
            val = val * 10 + (c - '0');
        } else if (c == '.') {
            if (digits == 0 || seg >= 3) return false;
            out[seg++] = val;
            val = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (digits == 0 || seg != 3) return false;
    out[3] = val;
    return true;
}

bool numEq(const int a[4], const int b[4]) {
    return a[0] == b[0] && a[1] == b[1];
}

// 在 [begin,end) 里扫四段版本串；只保留主次版本与 want 相同的，记下最大者（数值序）。
// 分两块缓冲轮转：GM 的字符串池没有长度前缀，只能靠"前后不是数字/点"划边界，
// 所以跨块要留 32 字节重叠（一个四段串最长 23 字节，留 32 足够）。
void scanRange(std::ifstream& f, std::uint64_t begin, std::uint64_t end, const int want[4],
               int best[4], std::string& bestStr, bool& sawFour) {
    const std::size_t kChunk = 8u << 20, kKeep = 32;
    std::vector<char> buf(kChunk + kKeep);
    f.clear();
    f.seekg((std::streamoff)begin, std::ios::beg);
    if (!f) return;
    std::uint64_t pos = begin;
    std::size_t carry = 0;
    while (pos < end) {
        const std::size_t want_n = (std::size_t)std::min<std::uint64_t>(kChunk, end - pos);
        f.read(buf.data() + carry, (std::streamsize)want_n);
        const std::size_t got = (std::size_t)f.gcount();
        if (got == 0) break;
        const std::size_t total = carry + got;
        std::size_t i = 0;
        while (i < total) {
            const unsigned char c = (unsigned char)buf[i];
            if (c < '0' || c > '9') { ++i; continue; }
            const std::size_t start = i;
            int seg = 1;
            while (i < total) {
                const unsigned char d = (unsigned char)buf[i];
                if (d >= '0' && d <= '9') { ++i; }
                else if (d == '.' && seg < 4) { ++seg; ++i; }
                else break;
            }
            const bool leftOk = (start == 0) || !((unsigned char)buf[start - 1] == '.' ||
                                 ((unsigned char)buf[start - 1] >= '0' && (unsigned char)buf[start - 1] <= '9'));
            const bool rightOk = (i >= total) || !((unsigned char)buf[i] == '.' ||
                                  ((unsigned char)buf[i] >= '0' && (unsigned char)buf[i] <= '9'));
            int v[4] = {0, 0, 0, 0};
            if (!leftOk || !rightOk || !parseFourSeg(buf.data() + start, i - start, v)) continue;
            sawFour = true;                      // 看到别的四段串也算：说明字符串池就在这段里
            if (numEq(v, want)) {
                bool better = bestStr.empty();
                if (!better) {
                    for (int k = 0; k < 4; ++k) {
                        if (v[k] != best[k]) { better = v[k] > best[k]; break; }
                    }
                }
                if (better) {
                    for (int k = 0; k < 4; ++k) best[k] = v[k];
                    bestStr.assign(buf.data() + start, i - start);
                }
            }
        }
        pos += got;
        // 末 32 字节搬到行首留给下一块，避免把一个串从中间切断
        carry = (total > kKeep) ? kKeep : total;
        if (carry) std::memmove(buf.data(), buf.data() + total - carry, carry);
    }
}

} // namespace

// 扫一个文件里 [begin,end) 之外那段"先尾部、必要时整份"的通用流程。
// 语义：尾部命中就用；尾部连一个四段串都没有（说明布局变了）才回退；否则直接返回（可能是空）。
static std::string probeWithTailWindow(std::ifstream& f, std::uint64_t size, const int want[4],
                                       std::uint64_t tailBytes) {
    int best[4] = {0, 0, 0, 0};
    std::string bestStr;
    bool sawFour = false;
    const std::uint64_t tailStart = (size > tailBytes) ? size - tailBytes : 0;
    scanRange(f, tailStart, size, want, best, bestStr, sawFour);
    if (!bestStr.empty() || sawFour) return bestStr;
    bool sawFourHead = false;
    scanRange(f, 0, tailStart, want, best, bestStr, sawFourHead);
    return bestStr;
}

std::string readEmbeddedVersion(const std::filesystem::path& exePath, const std::string& peVersion) {
    int want[4] = {0, 0, 0, 0};
    if (!parseFourSeg(peVersion.c_str(), peVersion.size(), want)) return "";

    // ① 独立 data.win 优先：它就是数据包本体（单文件打包时才塞进 exe），
    //    比 exe 小一个数量级（8~15MB vs 130~190MB），在机械盘上这就是几百 ms 与两三秒的差别。
    {
        const std::filesystem::path dw = exePath.parent_path() / L"data.win";
        std::error_code ec;
        const auto dwSize = std::filesystem::file_size(dw, ec);
        if (!ec && dwSize >= 1024 && dwSize <= (32u << 20)) {
            std::ifstream df(dw, std::ios::binary);
            if (df) {
                // data.win 本身就小，整份扫（尾部窗口对它没有意义）
                int best[4] = {0, 0, 0, 0};
                std::string bestStr;
                bool saw = false;
                scanRange(df, 0, (std::uint64_t)dwSize, want, best, bestStr, saw);
                if (!bestStr.empty()) return bestStr;
                // 池里明明有别的四段串却没有目标版本 → 这个包确实没写，别再去啃 exe
                if (saw) return "";
            }
        }
    }

    // ② 回退 exe 尾部 20%：实测 5 个包的版本串都落在距文件尾 12.5%~13.2% 处，
    //    20% 留了 7% 余量。尾部连一个四段串都没有才老实整份扫（那种布局我们没见过）。
    //    尾部有别的四段串（127.0.0.1 之类）却没有目标版本 —— 说明池就在这段里且确实没有，
    //    再扫前面 80% 是白读（1.18.5 / 1.20.1 就是这种：PE 末段是 0，数据包里没写四段版本）。
    std::ifstream f(exePath, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const std::uint64_t size = (std::uint64_t)f.tellg();
    if (size < 1024) return "";
    const std::uint64_t tail = (size >= (64u << 20)) ? (size / 5) : size;
    return probeWithTailWindow(f, size, want, tail);
}

} // namespace pxc
