#!/bin/bash
#
# build.sh - 构建 uroot 的 deb 安装包
# 用法: bash build.sh
#

set -e

PACKAGE="uroot"
VERSION="1.0.2"
ARCH="arm64"
DEB="${PACKAGE}_${VERSION}_${ARCH}.deb"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 检查编译依赖
if ! command -v gcc >/dev/null 2>&1; then
    echo "错误: 未找到 gcc，请先安装: sudo apt install gcc"
    exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "错误: 未找到 pkg-config，请先安装: sudo apt install pkg-config"
    exit 1
fi
if ! pkg-config --exists gio-unix-2.0 glib-2.0; then
    echo "错误: 缺少 GLib/GIO 开发库，请安装: sudo apt install libglib2.0-dev"
    exit 1
fi

# 检查源文件
if [ ! -f "$ROOT/uroot.c" ]; then
    echo "错误: 未找到 $ROOT/uroot.c"
    exit 1
fi
if [ ! -f "$ROOT/DEBIAN/control" ]; then
    echo "错误: 未找到 $ROOT/DEBIAN/control"
    exit 1
fi

# 清理旧构建
BUILD="$ROOT/build"
rm -rf "$BUILD"
rm -f "$ROOT/$DEB"

# 组装目录结构
mkdir -p "$BUILD/DEBIAN"
mkdir -p "$BUILD/usr/bin"

# 编译 uroot（带 SUID）
gcc -o "$BUILD/usr/bin/uroot" "$ROOT/uroot.c" $(pkg-config --cflags --libs gio-unix-2.0 glib-2.0)
chmod 4755 "$BUILD/usr/bin/uroot"

cp "$ROOT/DEBIAN/control" "$BUILD/DEBIAN/control"

# 打包
dpkg-deb --build --root-owner-group "$BUILD" "$ROOT/$DEB"

# 清理
rm -rf "$BUILD"

echo ""
echo "构建完成: $DEB"
echo "安装: sudo dpkg -i $DEB"