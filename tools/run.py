# 统一构建入口：PowerShell 在本机看不到 stdout，改用 python subprocess 捕获并写日志。
#
#   python tools/run.py cfg                    # 配置（生成 tmp/build）
#   python tools/run.py build pxc_launcher     # 构建产品
#   python tools/run.py build pxc_project_test # 构建纯逻辑回归（零外部依赖）
#
# 路径一律按**本文件位置**推导，不写死任何本机目录；换台机器 clone 下来直接能用。
import io
import os
import shutil
import subprocess
import sys

# 仓库根 = 本文件所在目录（tools/）的上一级
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "tmp", "build")


def find_cmake():
    """找 cmake：显式环境变量 → PATH → Visual Studio 自带的那份。

    Windows 上装了 VS 2022 就必然有一份 CMake，所以按 vswhere 找安装位置比写死
    `C:\\Program Files\\...\\Community\\...` 靠谱（Community/Professional/Enterprise 都能命中）。
    """
    p = os.environ.get("CMAKE_EXE") or shutil.which("cmake")
    if p and os.path.isfile(p):
        return p
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.isfile(vswhere):
        try:
            out = subprocess.run([vswhere, "-latest", "-property", "installationPath"],
                                 capture_output=True, text=True, timeout=30).stdout.strip()
        except Exception:
            out = ""
        if out:
            cand = os.path.join(out, "Common7", "IDE", "CommonExtensions",
                                "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
            if os.path.isfile(cand):
                return cand
    return "cmake"          # 交给 PATH，找不到时 cmake 自己会报错


CMAKE = find_cmake()


def run(args, log, timeout=1800):
    p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    out = (p.stdout or "") + ("\n[STDERR]\n" + p.stderr if p.stderr else "")
    io.open(log, "w", encoding="utf-8", errors="replace").write(f"[exit={p.returncode}]\n{out}")
    print(f"[exit={p.returncode}] -> {log}")
    return p.returncode


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    mode = sys.argv[1]
    if mode == "cfg":
        rc = run([CMAKE, "-S", ROOT, "-B", BUILD,
                  "-G", "Visual Studio 17 2022", "-A", "x64",
                  "-DEUI_ENABLE_TRAY=OFF",
                  # 静态 CRT（/MT）：单 exe 分发，不要求目标机装 VC++ 运行库
                  "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
                  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"],
                 os.path.join(ROOT, "tmp", "cfg.log"))
    elif mode == "build":
        if len(sys.argv) < 3:
            print("usage: run.py build <target> [Release|Debug]")
            sys.exit(2)
        target = sys.argv[2]
        cfg = sys.argv[3] if len(sys.argv) > 3 else "Release"
        rc = run([CMAKE, "--build", BUILD, "--target", target, "--config", cfg],
                 os.path.join(ROOT, "tmp", f"build_{target}.log"))
    else:
        print(__doc__)
        rc = 1
    sys.exit(rc)
