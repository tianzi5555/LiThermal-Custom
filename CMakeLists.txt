cmake_minimum_required(VERSION 3.7.4)

project(LiThermal)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ============================================================================
#  本项目现在编译**两个独立程序**：
#    LiThermal —— 相机应用（保持原样，只保留「隐藏终端 → 退出到桌面」这一个改动）
#    lios      —— 桌面系统（独立可执行；开机直接启动它，相机是桌面上一个方块）
#  两者互不依赖、进程隔离；插件 .so 只被 lios dlopen。
# ============================================================================

file(GLOB_RECURSE SRC_DIR_LIST "src/*.cpp" "src/*.c")
# 插件应用（src/apps/*.cpp）是独立的 .so，不编进主程序
list(FILTER SRC_DIR_LIST EXCLUDE REGEX "/src/apps/")
# LiOS 桌面是独立程序（src/lios/*），不编进相机
list(FILTER SRC_DIR_LIST EXCLUDE REGEX "/src/lios/")

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
add_compile_definitions(USE_SUNXIFB_DOUBLE_BUFFER)
add_compile_definitions(USE_SUNXIFB_CACHE)
add_compile_definitions(USE_SUNXIFB_G2D)
add_compile_definitions(USE_SUNXIFB_G2D_ROTATE)
add_compile_definitions(CONF_G2D_VERSION_NEW)
add_compile_options(-O2)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -Wno-psabi -O2")
set(LV_CONF_PATH "../../include/lv_conf.h")

# 必须在 add_subdirectory(libs/lvgl) 之前：lvgl 里的 rlottie 模块要这个头文件路径
include_directories(rlottie_inc)

add_subdirectory(libs/lvgl)
add_subdirectory(libs/lv_drivers)

# ---------------------------------------------------------------- 相机应用
add_executable(${PROJECT_NAME} ${SRC_DIR_LIST})
target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC include)
target_include_directories(${PROJECT_NAME} PUBLIC linux_include)
target_include_directories(${PROJECT_NAME} PUBLIC libs)
target_include_directories(${PROJECT_NAME} PUBLIC libs/lvgl)
target_include_directories(${PROJECT_NAME} PUBLIC libs/lv_drivers)
target_link_libraries(${PROJECT_NAME} lvgl lv_drivers uapi avcodec avdevice avformat avfilter avutil swscale pthread rlottie m)

# ---------------------------------------------------------------- LiOS 桌面（独立程序）
file(GLOB_RECURSE LIOS_SRC "src/lios/*.cpp" "src/lios/*.c")
add_executable(lios
    ${LIOS_SRC}
    # 和相机共用的小工具（各自编一份，互不链接）
    src/customtick.c      # LVGL 的自定义 tick（lv_conf.h 里 LV_TICK_CUSTOM 用到）
    src/perfProbe.cpp     # sunxifb.c 里的性能探针会调用
    src/dispVsync.cpp     # sunxifb.c 里的 vsync 等待会调用
    src/PowerManager.cpp  # 桌面长按电源键关机（复用相机那份串口实现）
    src/fonts/ui_font_cn16.c
    src/fonts/ui_font_cn12.c
)
target_include_directories(lios PUBLIC ${PROJECT_SOURCE_DIR})
target_include_directories(lios PUBLIC include)
target_include_directories(lios PUBLIC linux_include)
target_include_directories(lios PUBLIC libs)
target_include_directories(lios PUBLIC libs/lvgl)
target_include_directories(lios PUBLIC libs/lv_drivers)
target_include_directories(lios PUBLIC src/lios)
# lvgl 里带 ffmpeg / rlottie 模块，即使 lios 用不到也要链接（否则 undefined reference）
target_link_libraries(lios lvgl lv_drivers uapi avcodec avdevice avformat avfilter avutil swscale pthread rlottie m)

# 插件 .so 要能解析到 lios 里的 LVGL / lios_log 符号 → 必须导出动态符号表
# （-rdynamic / -Wl,--export-dynamic）。用 CMAKE_EXE_LINKER_FLAGS 兼容老版本 cmake。
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rdynamic")
