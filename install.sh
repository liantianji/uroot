#!/bin/bash
#
# install.sh - 编译并安装 uroot 到 /usr/bin/
# 用法: sudo bash install.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="/usr/bin/uroot"

# 检查 root 权限
if [ "$(id -u)" -ne 0 ]; then
    echo "请使用 sudo 运行此安装脚本: sudo bash install.sh"
    exit 1
fi

# 检查编译依赖
if ! command -v gcc >/dev/null 2>&1; then
    echo "错误: 未找到 gcc，请先安装: apt install gcc"
    exit 1
fi
if ! pkg-config --exists gio-unix-2.0 glib-2.0; then
    echo "错误: 缺少 GLib/GIO 开发库，请安装: apt install libglib2.0-dev"
    exit 1
fi

# 检查源文件
if [ ! -f "$SCRIPT_DIR/uroot.c" ]; then
    echo "错误: 未找到 $SCRIPT_DIR/uroot.c"
    exit 1
fi

echo "正在编译 uroot ..."
gcc -o "$TARGET" "$SCRIPT_DIR/uroot.c" $(pkg-config --cflags --libs gio-unix-2.0 glib-2.0)

# 设置 SUID 权限 (4755)
chmod 4755 "$TARGET"

# 验证安装
if [ -u "$TARGET" ]; then
    echo "安装成功!"
    echo "  位置: $TARGET"
    echo "  权限: SUID root (4755)"
    echo "  验证: 需输入当前用户密码（统信 D-Bus 认证）"
    echo "  用法: uroot <命令> [参数...]"
    echo "  示例: uroot rm -rf /home/test/k.jpg"
else
    echo "安装失败! SUID 位未设置"
    exit 1
fi