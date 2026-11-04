#pragma once
#include <string>
#include <cstdint>
#include <filesystem>

namespace pxc {

enum class Format { JSON, ZLIB, PXCX, UNKNOWN };
enum class Confidence { HIGH, MEDIUM, LOW, NONE };

struct DetectionResult {
    Format       format     = Format::UNKNOWN;
    std::string  version    = "";   // product version, empty if unknown
    Confidence   confidence = Confidence::NONE;
    std::string  rawValue   = "";   // integer (as text) or version string found
    std::string  fingerprint= "";   // node-type / key signature (for LOW/unknown)
    std::string  note       = "";
};

// Detect PXC file version from its bytes (format-agnostic).
// 使用 std::filesystem::path（宽路径），避免窄字符串按 ANSI 代码页（GBK）解码的编码坑。
DetectionResult detectPxc(const std::filesystem::path& path);

// Read ProductVersion string from a PXC exe (PE version resource).
// Returns "" on failure.
std::string readExeVersion(const std::filesystem::path& exePath);

// 在 exe 内嵌的 GM 数据包里找"四段版本串"，返回**与 peVersion 主次版本一致**的候选中最大的那个。
//
// 为什么需要：PE 版本资源不可靠。实测 PXC 1.21.0.2 那个包把 ProductVersion 写成了 1.21.0.0，
// 而数据包的字符串池里两个串都在（1.21.0.2 / 1.21.0.0）。1.19.0.11、1.22.0.5 的 PE 值是对的。
//
// 规则（不猜）：
//   · peVersion 为空或不是纯四段 → 返回 ""（没有参照物就不猜）；
//   · 候选必须四段、每段 1~5 位、无空段，且主、次版本与 peVersion 完全相同；
//   · 多个候选取数值最大者（1.21.0.2 > 1.21.0.0）。
// 成本：只读文件尾部 20%（实测命中点都在距尾 12.5%~13.2%），尾部一个四段串都没有才回退整份。
std::string readEmbeddedVersion(const std::filesystem::path& exePath, const std::string& peVersion);

} // namespace pxc
