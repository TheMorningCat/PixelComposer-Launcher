# 发布打包：把随程序分发的资源（对照表 / 内置字体 / 图标）与 exe 放到 launcher/bin。
# 资源源：launcher/data（仓库内），构建时用 CMake 后处理拷到 exe 输出目录；本脚本负责发布目录。
#
#   python tools/deploy.py
#
# 路径按本文件位置推导；字体缺省已随仓库提交（launcher/data/fonts），
# 只有在本机额外放了 `汉化文件/` 目录时才用它补齐。
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HF = os.path.join(ROOT, "汉化文件")          # 可选：本地汉化包目录（不随仓库分发）
DATA = os.path.join(ROOT, "launcher", "data")
BIN = os.path.join(ROOT, "launcher", "bin")


def copy_dir(src, dst):
    if not os.path.isdir(src):
        print("  [skip] 源不存在:", src)
        return 0
    n = 0
    for d, _, fs in os.walk(src):
        rel = os.path.relpath(d, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for f in fs:
            s = os.path.join(d, f)
            o = os.path.join(out, f)
            try:
                if os.path.isfile(o) and os.path.getsize(o) == os.path.getsize(s):
                    n += 1
                    continue
                shutil.copy2(s, o)
                n += 1
            except PermissionError:
                print("  [warn] 文件被占用，跳过:", o)
    return n


def main():
    # 1) 内置英文骨架已取消（docs/08 §8.1.4）：骨架永远取自用户已装的 PXC 版本，
    #    内置一份"最新版"在旧版本上会串端口，反而制造"看着正常其实错位"的问题。
    stale = os.path.join(DATA, "baseline")
    if os.path.isdir(stale):
        shutil.rmtree(stale, ignore_errors=True)
        print("清理已废弃的内置骨架 ->", stale)

    # 2) 内置字体：NotoSansSC 三档 + FiraCode + JetBrainsMono（约 7.8MB）。
    #    字体已随仓库提交，所以这里**只在缺失时**才从本地汉化包补 —— 没有汉化文件也不影响发布。
    fonts = os.path.join(DATA, "fonts")
    os.makedirs(fonts, exist_ok=True)
    pick = [
        ("PixelComposer_zh_汉化包_1.22.0.5/Locale/zh/fonts", "NotoSansSC-Medium.ttf"),
        ("PixelComposer_zh_汉化包_1.22.0.5/Locale/zh/fonts", "NotoSansSC-Bold.ttf"),
        ("PixelComposer_zh_汉化包_1.22.0.5/Locale/zh/fonts", "NotoSansSC-SemiBold.ttf"),
        ("PixelComposer_zh_汉化包_1.22.0.5/Locale/zh/fonts", "FiraCode-Medium.ttf"),
        ("1.18中英对照版汉化包/zh/fonts", "JetBrainsMono-Medium.ttf"),
    ]
    missing = 0
    for sub, fn in pick:
        dst = os.path.join(fonts, fn)
        if os.path.isfile(dst):
            continue                                   # 已在仓库里，不动
        src = os.path.join(HF, *sub.split("/"), fn)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            print("  补齐字体:", fn)
        else:
            missing += 1
            print("  [warn] 字体缺失（且本地汉化包里也没有）:", fn)
    print("内置字体 ->", fonts, f"(缺 {missing} 个)")

    # 3) 对照表是「源」不是「产物」：由 pxc_db_build 直接写进 launcher/data，
    #    这里只做存在性检查，方向永远是 launcher/data -> launcher/bin/data（下一步），绝不反向覆盖。
    db = os.path.join(DATA, "translation_db.json")
    if os.path.isfile(db):
        print("对照表 ->", db, f"({os.path.getsize(db)//1024} KB)")
    else:
        print("  [warn] launcher/data 下没有 translation_db.json")

    # 4) 发布到 launcher/bin
    os.makedirs(BIN, exist_ok=True)
    n3 = copy_dir(DATA, os.path.join(BIN, "data"))
    print("发布资源 ->", os.path.join(BIN, "data"), f"({n3} 个文件)")

    exe = None
    for d, _, fs in os.walk(os.path.join(ROOT, "tmp", "build")):
        if "pxc_launcher.exe" in fs:
            exe = os.path.join(d, "pxc_launcher.exe")
            break
    if not exe:
        print("  [warn] 未找到构建产物 pxc_launcher.exe（先跑 tools/run.py build pxc_launcher）")
        return 1
    dst = os.path.join(BIN, "pxc_launcher.exe")
    try:
        shutil.copy2(exe, dst)
        print("可执行文件 ->", dst)
    except PermissionError:
        alt = os.path.join(BIN, "pxc_launcher_new.exe")
        shutil.copy2(exe, alt)
        print("可执行文件被占用，已另存 ->", alt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
