@echo off
setlocal
REM 构建识别内核：pxc_detect.cpp + test_main.cpp + zlib 1.3.1 源码（静态链入，免外部 lib）
REM 依赖：Visual Studio 2022（Community / Professional / Enterprise 均可）
REM
REM VS 安装位置用 vswhere 探测而不是写死路径 —— vswhere.exe 由 VS Installer 自带，
REM 写死 "C:\Program Files\...\Community\..." 在 Professional/Enterprise 或自定义安装目录上会失败。

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [error] 找不到 vswhere.exe，请确认已安装 Visual Studio 2022（含 C++ 桌面开发工作负载）
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [error] vswhere 未返回 VS 安装路径
    exit /b 1
)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat"

cl /MT /std:c++17 /EHsc /I "..\EUI-NEO-main\3rd\zlib-1.3.1" ^
 pxc_detect.cpp test_main.cpp ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\adler32.c" ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\crc32.c" ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\inftrees.c" ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\inflate.c" ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\inffast.c" ^
 "..\EUI-NEO-main\3rd\zlib-1.3.1\zutil.c" ^
 Version.lib /Fe:detect_test.exe
REM 用法：detect_test.exe <样本目录>  例：detect_test.exe "..\工程文件样本"
