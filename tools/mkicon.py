# -*- coding: utf-8 -*-
"""把「素材-启动器icon.png（256 RGBA）」+「素材-启动器icon-x16.ico（手调 16px）」合成一个
多尺寸 ICO，供 exe 图标资源用。

为什么要合成而不是直接把两张 ico 拼起来：
  - 两个源 ico 都是**单帧 24bpp**（无 alpha，只有 1bit AND 掩码），Windows 缩放到 20/24/32/40/48
    时会从 256 硬缩，边缘糊；
  - 48/32/24/20 这些尺寸必须自己缩好放进去，Explorer / 任务栏 / 标题栏才会各取所需。

缩放口径：**alpha 预乘 + 线性光 + 面积平均**（纯 box 滤波）。
  - 直接对 sRGB 值取平均会让橙色描边发暗（gamma 错），所以先转到线性光再平均；
  - 圆角处是透明像素，不预乘的话白色描边会被"拉"进透明区形成白边。

用法：python tmp/mkicon.py            # 写 launcher/res/launcher.ico
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_PNG = os.path.join(ROOT, "素材", "素材-启动器icon.png")
SRC_ICO16 = os.path.join(ROOT, "素材", "素材-启动器icon-x16.ico")
OUT_ICO = os.path.join(ROOT, "launcher", "res", "launcher.ico")

SIZES_BMP = [16, 20, 24, 32, 40, 48, 64, 128]   # 32bpp BMP 帧
SIZES_PNG = [256]                              # PNG 压缩帧（Vista+，省体积）


# ---------------- PNG 解码（只处理 8bit RGBA 非隔行） ----------------
def decode_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    i, idat = 8, b""
    w = h = 0
    while i < len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        body = d[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            assert (bd, ct, inter) == (8, 6, 0), "只支持 8bit RGBA 非隔行"
        elif typ == b"IDAT":
            idat += body
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 4
    out = []
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out.append(bytes(line))
        prev = line
    return w, h, out


# ---------------- PNG 编码 ----------------
def encode_png(w, h, rgba_rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\x00" + r for r in rgba_rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


# ---------------- 16px 手调帧：24bpp BMP + AND 掩码 -> RGBA ----------------
def load_ico_frame(path):
    d = open(path, "rb").read()
    cnt = struct.unpack("<H", d[4:6])[0]
    assert cnt >= 1, path
    w0, h0, cc, res, pl, bc, sz, off = struct.unpack("<BBBBHHII", d[6:22])
    W, H = w0 or 256, h0 or 256
    bmp = d[off:off + sz]
    bih = struct.unpack("<IiiHHIIiiII", bmp[:40])
    bpp = bih[4]
    assert bpp in (24, 32), "只处理 24/32bpp 帧"
    xor_row = ((W * (bpp // 8) + 3) // 4) * 4
    mask_row = ((W + 31) // 32) * 4
    mask_off = 40 + xor_row * H
    mask = bmp[mask_off:mask_off + mask_row * H]
    rows = []
    for y in range(H):
        srow = H - 1 - y                      # BMP 自下而上
        line = bytearray()
        for x in range(W):
            o = 40 + srow * xor_row + x * (bpp // 8)
            b, g, r = bmp[o], bmp[o + 1], bmp[o + 2]
            a = bmp[o + 3] if bpp == 32 else (
                0 if (mask[srow * mask_row + (x >> 3)] & (0x80 >> (x & 7))) else 255)
            line += bytes((r, g, b, a))
        rows.append(bytes(line))
    return W, H, rows


# ---------------- 面积平均缩放（线性光 + 预乘 alpha） ----------------
def _srgb_to_lin():
    lut = []
    for v in range(256):
        c = v / 255.0
        lut.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return lut


def _lin_to_srgb():
    lut = []
    for i in range(4097):
        c = i / 4096.0
        s = 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055
        lut.append(max(0, min(255, int(round(s * 255.0)))))
    return lut


S2L = _srgb_to_lin()
L2S = _lin_to_srgb()


def _weights(src, dst):
    """一维面积平均权重：返回 [[(src_index, weight), ...], ...]（每个目标像素一组）。"""
    scale = src / dst
    out = []
    for i in range(dst):
        lo, hi = i * scale, (i + 1) * scale
        acc = []
        j = int(lo)
        while j < hi and j < src:
            cover = min(hi, j + 1) - max(lo, j)
            if cover > 1e-9:
                acc.append((j, cover))
            j += 1
        out.append(acc)
    return out


def resize_rgba(w, h, rows, nw, nh):
    # 先横向
    wx = _weights(w, nw)
    tmp = []
    for y in range(h):
        row = rows[y]
        acc = []
        for group in wx:
            pr = pg = pb = pa = 0.0
            wsum = 0.0
            for j, wt in group:
                wsum += wt
                o = j * 4
                a = row[o + 3] / 255.0
                if a <= 0.0:
                    continue
                k = wt * a
                pr += S2L[row[o]] * k
                pg += S2L[row[o + 1]] * k
                pb += S2L[row[o + 2]] * k
                pa += k
            if pa <= 1e-9:
                acc.append((0.0, 0.0, 0.0, 0.0))
            else:
                # alpha 要除以**权重和**（=目标像素覆盖了多少个源像素），不是源像素个数：
                # 除以个数会在满覆盖处也 <1（256→44 时 ≈5.8/6≈0.97），整张图被压成半透明。
                acc.append((pr / pa, pg / pa, pb / pa, pa / wsum))
        tmp.append(acc)
    # 再纵向
    wy = _weights(h, nh)
    out = []
    for group in wy:
        line = bytearray()
        wsum = sum(wt for _, wt in group)
        for x in range(nw):
            pr = pg = pb = pa = 0.0
            for j, wt in group:
                r, g, b, a = tmp[j][x]
                if a <= 0.0:
                    continue
                k = wt * a
                pr += r * k
                pg += g * k
                pb += b * k
                pa += k
            if pa <= 1e-9:
                line += b"\x00\x00\x00\x00"
            else:
                a = pa / wsum
                line += bytes((L2S[int(min(1.0, pr / pa) * 4096)],
                               L2S[int(min(1.0, pg / pa) * 4096)],
                               L2S[int(min(1.0, pb / pa) * 4096)],
                               max(0, min(255, int(round(a * 255.0))))))
        out.append(bytes(line))
    return out


# ---------------- 帧编码 ----------------
def frame_bmp(w, h, rows):
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    px = bytearray()
    for y in range(h - 1, -1, -1):
        r = rows[y]
        for x in range(w):
            o = x * 4
            px += bytes((r[o + 2], r[o + 1], r[o], r[o + 3]))
    mask_row = ((w + 31) // 32) * 4
    mask = bytearray()
    for y in range(h - 1, -1, -1):
        r = rows[y]
        bits = bytearray(mask_row)
        for x in range(w):
            if r[x * 4 + 3] == 0:            # 掩码置 1 = 透明
                bits[x >> 3] |= 0x80 >> (x & 7)
        mask += bits
    return hdr + bytes(px) + bytes(mask)


def write_brand_png():
    w, h, rows = decode_png(SRC_PNG)
    rs = resize_rgba(w, h, rows, BRAND_PX, BRAND_PX)
    os.makedirs(os.path.dirname(OUT_BRAND_PNG), exist_ok=True)
    open(OUT_BRAND_PNG, "wb").write(encode_png(BRAND_PX, BRAND_PX, rs))
    print("\u5199 %s  %d\u00d7%d  %d 字节"
          % (OUT_BRAND_PNG, BRAND_PX, BRAND_PX, os.path.getsize(OUT_BRAND_PNG)))


def main():
    w, h, rows = decode_png(SRC_PNG)
    if (w, h) != (256, 256):
        print("警告：源 PNG 是 %dx%d，按 256 期望生成" % (w, h))
    frames = []                                # (size, bytes)
    fw, fh, frows = load_ico_frame(SRC_ICO16)
    assert (fw, fh) == (16, 16), (fw, fh)
    src16 = None
    for s in sorted(SIZES_BMP + SIZES_PNG):
        if s == 16:
            src16 = frows                       # 手调那版原样用（掩码已被转成 alpha）
            frames.append((16, frame_bmp(16, 16, frows)))
        elif s in SIZES_PNG:
            rs = resize_rgba(w, h, rows, s, s)
            frames.append((s, encode_png(s, s, rs)))
        else:
            rs = resize_rgba(w, h, rows, s, s)
            frames.append((s, frame_bmp(s, s, rs)))

    # 组装 ICO（目录项按尺寸从小到大）
    head = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    dirs, blobs = b"", b""
    for s, data in frames:
        dirs += struct.pack("<BBBBHHII", s & 0xFF, s & 0xFF, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    os.makedirs(os.path.dirname(OUT_ICO), exist_ok=True)
    open(OUT_ICO, "wb").write(head + dirs + blobs)
    print("写 %s  %d 帧  %d 字节" % (OUT_ICO, len(frames), os.path.getsize(OUT_ICO)))
    print("尺寸:", ", ".join(str(s) for s, _ in frames))
    if src16:
        print("16px 帧来自手调 ico（含圆角掩码）")



# ---------------- 顺带生成侧栏用的矢量图标 ----------------
# `素材/素材-启动器icon.svg` 是 Illustrator 导出的，填充写在 <style> 的 class 里；
# nanosvg **不解析 CSS**（docs/09 §9.3），直接喂会整块变黑。所以发布拷贝要把 class
# 换成显式 fill、去掉 <style> 块（与 icons/pxc.svg 的处理方式一致）。
SRC_SVG = os.path.join(ROOT, "\u7d20\u6750", "\u7d20\u6750-\u542f\u52a8\u5668icon.svg")
OUT_SVG = os.path.join(ROOT, "launcher", "data", "icons", "launcher.svg")
SVG_FILL = {"st0": "#FFFFFF", "st1": "#FF9267", "st2": "#272736"}

# 侧栏品牌图标：**按显示尺寸预渲染成位图**（44×44，与 app.cpp 的 kBrandLogo 一致）。
# 为什么不用 ui.svg：EUI 把任何 SVG 固定栅格化成 512×512（image_source.cpp 的
# kSvgRasterSize），再用 GL_LINEAR、**无 mipmap** 缩到 44 px —— 11 倍缩小只采样很小
# 的邻域，圆角/描边就成了硬锯齿。喂一张 1:1 的位图就没有缩小采样，边缘是这里用
# 「线性光 + 预乘 alpha 面积平均」缩出来的抗锯齿。改 kBrandLogo 时要同步改这里。
OUT_BRAND_PNG = os.path.join(ROOT, "launcher", "data", "icons", "launcher_44.png")
BRAND_PX = 44


def write_svg():
    import re
    txt = open(SRC_SVG, encoding="utf-8").read()
    txt = re.sub(r"<style[^>]*>.*?</style>", "", txt, flags=re.S)
    txt = re.sub(r'class="(st\d)"', lambda m: 'fill="%s"' % SVG_FILL[m.group(1)], txt)
    txt = re.sub(r"\n[ \t]*\n", "\n", txt)
    if not txt.endswith("\n"):
        txt += "\n"
    os.makedirs(os.path.dirname(OUT_SVG), exist_ok=True)
    open(OUT_SVG, "w", encoding="utf-8", newline="\n").write(txt)
    print("\u5199 %s  %d \u5b57\u8282" % (OUT_SVG, os.path.getsize(OUT_SVG)))


if __name__ == "__main__":
    main()
    write_svg()
    write_brand_png()
