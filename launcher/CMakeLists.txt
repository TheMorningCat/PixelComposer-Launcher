# PXC 启动器：EUI-NEO 窗口程序 + pxc_detect 识别内核 + 版本库/备份/启动/汉化后端。
add_executable(pxc_launcher
    app.cpp
    log.cpp
    native_ui.cpp
    store.cpp
    version_registry.cpp
    version_probe.cpp
    steam_locator.cpp
    backup_manager.cpp
    launcher.cpp
    locale_manager.cpp
    expr.cpp
    translation_db.cpp
    baseline_scanner.cpp
    locale_zip.cpp
    gen_autolaunch.cpp
    locale_generator.cpp
    override_io.cpp
    db_editor.cpp
    pxc_prefs.cpp
    version_advisor.cpp
    project_launcher.cpp
    project_index.cpp
    assoc.cpp
    single_instance.cpp
    pxc_reuse.cpp
    launch_stage.cpp
    launch_toast.cpp
    launch_runner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect/pxc_detect.cpp
)

# 源码含中文（UI 文本/日志），强制 UTF-8 源码编码，规避 MSVC C4819。
target_compile_options(pxc_launcher PRIVATE /utf-8)
# 使用 _wfopen 等 intentional 的"不安全"CRT 函数，关闭其弃用警告。
target_compile_definitions(pxc_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(pxc_launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)

# 接入 EUI-NEO 框架（自动加入应用入口 main + 链接 eui::neo + 设为 Windows 子系统 GUI）
target_link_libraries(pxc_launcher PRIVATE eui::neo ZLIB::ZLIB)
if(WIN32)
    # GetFileVersionInfo / CreateProcess 等所在
    target_link_libraries(pxc_launcher PRIVATE Version.lib)
    # AlphaBlend：提示窗把 SVG 光栅化成的 32bpp 预乘位图贴到 GDI 上（launch_toast）。
    target_link_libraries(pxc_launcher PRIVATE Msimg32.lib)
endif()

if(WIN32)
    # EUI-NEO 的 `eui_neo_configure_app` 默认给每个 app 挂一张它自带的
    # `EUI-NEO-main/assets/icon.ico`（资源名 IDI_APP_ICON）。但我们自己的 app.rc 也要图标，
    # 而 rc.exe 会**给每个 ICON 语句里的各帧重新从 1 起编号 RT_ICON** —— 两张 ico 一起编，
    # 第二个 ICON 组的帧号必然撞车：CVT1100「资源重复：ICON 1」+ LNK1123。
    # 所以先把那个全局属性清空（= 不挂框架那张），换我们自己的。
    set_property(GLOBAL PROPERTY EUI_NEO_APP_ICON_RESOURCE "")
endif()
eui_neo_configure_app(pxc_launcher)

if(WIN32)
    # 程序图标：res/app.rc 里一条 `GLFW_ICON ICON "launcher.ico"`，既给 exe 外观，
    # 也给 GLFW 的窗口类（按资源名加载，见文件头注释）。ico 由 tmp/mkicon.py 生成。
    target_sources(pxc_launcher PRIVATE res/app.rc)
    # .rc 里的注释是 UTF-8 中文，rc.exe 默认按 ANSI(GBK) 读会乱码 —— 显式指定 UTF-8 代码页。
    set_source_files_properties(res/app.rc PROPERTIES COMPILE_FLAGS "/c65001")
endif()

# 随程序分发的资源（对照表 / 内置英文骨架 / 内置字体）：仓库内 launcher/data -> exe 所在目录/data。
# 运行期按 <exe目录>/data 定位，故每次构建后同步一份，开发与发布行为一致。
add_custom_command(TARGET pxc_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:pxc_launcher>/data
    COMMENT "同步随程序分发的资源到输出目录"
)

# ---------------------------------------------------------------------------
# 以下四个目标是**开发期本地诊断工具**，默认不构建：
#   · 它们的源码在 tmp/ 下，属于本地未提交的临时物（.gitignore 掉），
#     公开仓库里没有这些 .cpp —— 打开这个开关会因找不到源文件而配置失败；
#   · 而且它们依赖本机才有的样本（PXC 安装目录 / 工程文件样本 / 汉化包），
#     换台机器跑不了，不适合放进 CI。
# 维护者本地需要它们时：
#   cmake -DPXC_DEV_TOOLS=ON ...
# ---------------------------------------------------------------------------
option(PXC_DEV_TOOLS "构建依赖本地样本的开发期诊断工具（需 tmp/ 下的源码）" OFF)

if(PXC_DEV_TOOLS)

# 无界面的后端验证程序（不依赖 EUI-NEO），用于 CI/沙箱内对真实样本做 headless 测试。
add_executable(pxc_backend_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../tmp/backend_test.cpp
    log.cpp
    store.cpp
    version_registry.cpp
    version_probe.cpp
    steam_locator.cpp
    backup_manager.cpp
    locale_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect/pxc_detect.cpp
)
target_compile_options(pxc_backend_test PRIVATE /utf-8)
target_compile_definitions(pxc_backend_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(pxc_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)
target_link_libraries(pxc_backend_test PRIVATE ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(pxc_backend_test PRIVATE Version.lib)
endif()

# 开发期：离线产出随程序分发的 data/translation_db.json（与启动器内构建器同一套算法）。
add_executable(pxc_db_build
    ${CMAKE_CURRENT_SOURCE_DIR}/../tmp/build_db.cpp
    store.cpp
    translation_db.cpp
)
target_compile_options(pxc_db_build PRIVATE /utf-8)
target_compile_definitions(pxc_db_build PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(pxc_db_build PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)

# v2 生成管线 headless 验证：1.22 英文原版骨架 + 各汉化包 → 构建对照表 → 生成语言文件。
add_executable(pxc_gen_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../tmp/gen_test.cpp
    log.cpp
    store.cpp
    expr.cpp
    translation_db.cpp
    version_registry.cpp
    version_probe.cpp
    steam_locator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect/pxc_detect.cpp
    baseline_scanner.cpp
    locale_zip.cpp
    gen_autolaunch.cpp
    locale_generator.cpp
    override_io.cpp
    db_editor.cpp
)
target_compile_options(pxc_gen_test PRIVATE /utf-8)
target_compile_definitions(pxc_gen_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(pxc_gen_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)
if(WIN32)
    target_link_libraries(pxc_gen_test PRIVATE Version.lib)
endif()
# Steam 版的骨架在 pack/locale.zip 里（locale_zip.cpp），读它要用 zlib。
target_link_libraries(pxc_gen_test PRIVATE ZLIB::ZLIB)

endif()   # PXC_DEV_TOOLS（前半：backend_test / db_build / gen_test）

# v3 工程文件工作台 P0 的 headless 回归：版本顾问（结构族/距离/排序）+ 启动入口失败分支。
# 不链接 EUI（要能在任意机器上无人值守跑），只测纯逻辑 —— 零外部依赖，任何人 clone 下来都能跑：
#   ctest / 或直接执行 pxc_project_test
add_executable(pxc_project_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/project_test.cpp
    log.cpp
    store.cpp
    version_registry.cpp
    version_probe.cpp
    steam_locator.cpp
    backup_manager.cpp
    launcher.cpp
    version_advisor.cpp
    project_launcher.cpp
    project_index.cpp
    launch_stage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect/pxc_detect.cpp
)
target_compile_options(pxc_project_test PRIVATE /utf-8)
target_compile_definitions(pxc_project_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(pxc_project_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)
if(WIN32)
    target_link_libraries(pxc_project_test PRIVATE Version.lib)
endif()
target_link_libraries(pxc_project_test PRIVATE ZLIB::ZLIB)

if(PXC_DEV_TOOLS)   # 后半：gen_cli（同样依赖本地临时源码）

# 离线复现：用真实用户目录（可传中文路径）跑一次完整生成，定位崩溃阶段。
add_executable(pxc_gen_cli
    ${CMAKE_CURRENT_SOURCE_DIR}/../tmp/gen_cli.cpp
    log.cpp
    store.cpp
    expr.cpp
    translation_db.cpp
    version_registry.cpp
    version_probe.cpp
    baseline_scanner.cpp
    locale_zip.cpp
    gen_autolaunch.cpp
    locale_generator.cpp
    override_io.cpp
)
target_compile_options(pxc_gen_cli PRIVATE /utf-8)
target_compile_definitions(pxc_gen_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(pxc_gen_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pxc_detect
)
if(WIN32)
    target_link_libraries(pxc_gen_cli PRIVATE Version.lib)
endif()
target_link_libraries(pxc_gen_cli PRIVATE ZLIB::ZLIB)

endif()   # PXC_DEV_TOOLS
