#!/bin/bash
set -e

ROOTPATH="$(pwd)/compiler"
SRC="$(pwd)"

# 用仓库根目录里的 LiThermal 源码替换编译器仓库里的 LiThermal 子模块目录
if [ -d "$ROOTPATH/LiThermal" ]; then
    rm -rf "$ROOTPATH/LiThermal"
fi
mkdir -p "$ROOTPATH/LiThermal"
cp -r "$SRC/include" "$SRC/src" "$SRC/libs" "$SRC/linux_include" "$SRC/rlottie_inc" "$SRC/tools" "$ROOTPATH/LiThermal/"
cp "$SRC/CMakeLists.txt" "$SRC/toolchain.cmake" "$ROOTPATH/LiThermal/"

cd "$ROOTPATH"
export STAGING_DIR="$ROOTPATH/target"

mkdir -p build
cd build
cmake "$ROOTPATH/LiThermal" \
    -DROOTPATH="$ROOTPATH" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOTPATH/LiThermal/toolchain.cmake"
make -j"$(nproc)"

# 编译 BSOD（蓝屏/错误提示小程序）
"$ROOTPATH/toolchain-sunxi-musl/toolchain/bin/arm-openwrt-linux-gcc" \
    -o "$ROOTPATH/build/BSOD" "$ROOTPATH/LiThermal/tools/BSOD.c"

# 打包 UDISK 目录
mkdir -p "$GITHUB_WORKSPACE/UDISK"
cp "$ROOTPATH/build/LiThermal" "$GITHUB_WORKSPACE/UDISK"
cp "$ROOTPATH/build/BSOD" "$GITHUB_WORKSPACE/UDISK"
# LiOS 桌面是独立程序（开机由 thermalcamera.sh 直接启动它，相机是桌面上一个方块）
if [ -f "$ROOTPATH/build/lios" ]; then
    cp "$ROOTPATH/build/lios" "$GITHUB_WORKSPACE/UDISK"
else
    echo "!! build/lios 不存在，桌面不会被打包（检查 CMakeLists 的 lios target）"
    exit 1
fi
# 启动脚本：优先用本仓库根目录这份（编译器仓库里那份可能没同步）
if [ -f "$SRC/thermalcamera.sh" ]; then
    cp "$SRC/thermalcamera.sh" "$GITHUB_WORKSPACE/UDISK"
else
    cp "$ROOTPATH/thermalcamera.sh" "$GITHUB_WORKSPACE/UDISK"
fi

# ---- 插件 .so（弹球 / 俄罗斯方块）：打进 UDISK/apps，部署时放到 /mnt/UDISK/apps/ ----
mkdir -p "$GITHUB_WORKSPACE/UDISK/apps"
for plug in ball tetris calc unitconv stopwatch sysmon logclean procmon terminal bw21cam; do
    if [ -f "$ROOTPATH/build/$plug.so" ]; then
        cp "$ROOTPATH/build/$plug.so" "$GITHUB_WORKSPACE/UDISK/apps/"
    else
        echo "!! build/$plug.so 不存在（检查 CMakeLists 里的插件 target）"
        exit 1
    fi
done

# ---- 插件符号自检：插件里用到的 lv_* / ui_font_* / lios_* 必须都能从 lios 解析到 ----
# 插件不链 lvgl（避免两份 LVGL 状态），全靠主程序 -rdynamic 导出。
# 少了符号的话 dlopen(RTLD_NOW) 会失败、插件在桌面上直接消失 —— 所以这里提前拦下来。
NM="$ROOTPATH/toolchain-sunxi-musl/toolchain/bin/arm-openwrt-linux-nm"
if [ -x "$NM" ]; then
    "$NM" -D --defined-only "$ROOTPATH/build/lios" | awk '{print $NF}' | sort -u > /tmp/lios_defined.txt
    for plug in ball tetris calc unitconv stopwatch sysmon logclean procmon terminal bw21cam; do
        "$NM" -D --undefined-only "$ROOTPATH/build/$plug.so" | awk '{print $NF}' \
            | grep -E '^(lv_|ui_font_|lios_)' | sort -u > "/tmp/need_$plug.txt" || true
        MISSING=$(comm -23 "/tmp/need_$plug.txt" /tmp/lios_defined.txt || true)
        if [ -n "$MISSING" ]; then
            echo "!! $plug.so 里这些符号在 lios 里找不到（dlopen 会失败）："
            echo "$MISSING"
            exit 1
        fi
        echo "OK $plug.so：$(wc -l < /tmp/need_$plug.txt) 个 lv_/ui_font_/lios_ 符号全部能从 lios 解析"
    done
else
    echo "!! 没找到 $NM，跳过插件符号自检"
fi

echo "===== UDISK contents ====="
ls -la "$GITHUB_WORKSPACE/UDISK" "$GITHUB_WORKSPACE/UDISK/apps"
