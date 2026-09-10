#!/bin/bash
set -e

ROOTPATH="$(pwd)/compiler"
SRC="$(pwd)/LiThermal"

# 用我们自己的源码替换编译器仓库里的 LiThermal 子模块目录
if [ -d "$ROOTPATH/LiThermal" ]; then
    rm -rf "$ROOTPATH/LiThermal"
fi
cp -r "$SRC" "$ROOTPATH/LiThermal"

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
cp "$ROOTPATH/thermalcamera.sh" "$GITHUB_WORKSPACE/UDISK"

echo "===== UDISK contents ====="
ls -la "$GITHUB_WORKSPACE/UDISK"
