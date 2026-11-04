#include "override_io.h"

#include "io_util.h"
#include "store.h"

namespace pxc_ovio {

const char* const kFormat = "pxc-launcher-overrides";
constexpr int kFormatVersion = 1;
const char* const kSlotKeys[6] = {"node_name", "node_tip", "in_name", "out_name", "dropdown", "text"};

namespace {

void putStr(pxc_store::JsonValue& obj, const std::string& k, const std::string& v) {
    pxc_store::JsonValue s;
    s.type = pxc_store::JsonValue::Str;
    s.str = v;
    obj.obj.emplace_back(k, s);
}

void putNum(pxc_store::JsonValue& obj, const std::string& k, double v) {
    pxc_store::JsonValue n;
    n.type = pxc_store::JsonValue::Num;
    n.num = v;
    obj.obj.emplace_back(k, n);
}

} // namespace

bool save(const std::filesystem::path& p, const Bundle& b) {
    pxc_store::JsonValue root;
    root.type = pxc_store::JsonValue::Obj;
    putStr(root, "format", kFormat);
    putNum(root, "format_version", (double)kFormatVersion);
    putStr(root, "db_version", b.db_version);
    putStr(root, "lang_dir", b.lang_dir);
    if (b.preset >= 0) putNum(root, "preset", (double)b.preset);

    pxc_store::JsonValue slots;
    slots.type = pxc_store::JsonValue::Obj;
    for (const auto& kv : b.slots) putStr(slots, kv.first, kv.second);
    root.obj.emplace_back("slots", slots);

    pxc_store::JsonValue ov;
    ov.type = pxc_store::JsonValue::Obj;
    for (const auto& kv : b.overrides) {
        if (kv.second.empty()) continue;  // 空值不算覆盖
        putStr(ov, kv.first, kv.second);
    }
    root.obj.emplace_back("overrides", ov);

    return pxc_io::writeJson(p, root);
}

bool load(const std::filesystem::path& p, Bundle& out, std::string& err) {
    out = Bundle{};
    auto [root, ok] = pxc_io::readJson(p);
    if (!ok || !root.isObj()) {
        err = "无法解析该文件（不是有效的 JSON）。";
        return false;
    }
    const pxc_store::JsonValue* fmt = root.find("format");
    if (!fmt || !fmt->isStr() || fmt->str != kFormat) {
        err = "这不是本程序的偏好文件（缺少 format 标记）。";
        return false;
    }
    if (const auto* v = root.find("format_version"); v && v->type == pxc_store::JsonValue::Num) {
        if ((int)v->num > kFormatVersion) {
            err = "文件来自更新版本的启动器（format_version " + std::to_string((int)v->num) +
                  "），请升级后再导入。";
            return false;
        }
    }
    if (const auto* v = root.find("db_version"); v && v->isStr()) out.db_version = v->str;
    if (const auto* v = root.find("lang_dir"); v && v->isStr()) out.lang_dir = v->str;
    if (const auto* v = root.find("preset"); v && v->type == pxc_store::JsonValue::Num)
        out.preset = (int)v->num;
    if (const auto* s = root.find("slots"); s && s->isObj())
        for (const auto& kv : s->obj)
            if (kv.second.isStr()) out.slots[kv.first] = kv.second.str;

    const pxc_store::JsonValue* ov = root.find("overrides");
    if (!ov || !ov->isObj()) {
        err = "文件里没有 overrides 段。";
        return false;
    }
    for (const auto& kv : ov->obj)
        if (kv.second.isStr() && !kv.second.str.empty()) out.overrides[kv.first] = kv.second.str;
    if (out.overrides.empty()) {
        err = "文件里没有任何覆盖条目。";
        return false;
    }
    return true;
}

MergeStats merge(const Bundle& b, std::map<std::string, std::string>& dst) {
    MergeStats st;
    for (const auto& kv : b.overrides) {
        auto it = dst.find(kv.first);
        if (it == dst.end()) {
            dst[kv.first] = kv.second;
            ++st.added;
        } else if (it->second != kv.second) {
            it->second = kv.second;
            ++st.updated;
        } else {
            ++st.same;
        }
    }
    return st;
}

} // namespace pxc_ovio
