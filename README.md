# uroot

统信 UOS / Deepin 系统下的 SUID root 命令包装器。先通过 D-Bus 验证当前用户密码，再检查 sudo 组成员资格，两步都通过后才以 root 权限执行命令。

## 功能

- 用户密码验证（调用统信 `com.deepin.daemon.Authenticate` D-Bus 服务）
- sudo 组成员检查（不属于 sudo 组直接拒绝）
- SUID root 提权执行
- 密码无回显输入，用完立即从内存清除

## 工作流程

```
uroot <命令> [参数...]
    │
    ├─ 1. 获取当前真实用户名（getuid）
    ├─ 2. 提示输入密码（无回显）
    ├─ 3. D-Bus 验证密码 ──失败──> 退出
    ├─ 4. 检查 sudo 组 ──不在组──> 退出
    └─ 5. 全部通过 -> root 权限执行命令
```

## 构建依赖

在统信 UOS / Deepin 系统上安装：

```bash
sudo apt install gcc pkg-config libglib2.0-dev
```

## 构建

```bash
bash build.sh
```

生成 `uroot_1.0.2_arm64.deb`。

如需构建 amd64 版本，修改 `build.sh` 中的 `ARCH="arm64"` 为 `ARCH="amd64"`。

## 安装

### 方式一：deb 包安装（推荐）

```bash
sudo dpkg -i uroot_1.0.2_arm64.deb
```

### 方式二：手动编译安装

```bash
sudo bash install.sh
```

## 使用

```bash
# 删除文件
uroot rm -rf /home/test/k.jpg

# 安装软件包
uroot apt install nginx

# 重启服务
uroot systemctl restart nginx

# 卸载软件包
uroot dpkg -r xxx
```

运行后会提示输入当前用户的密码，验证通过即以 root 执行。

## 卸载

```bash
sudo dpkg -r uroot
```

## 安全说明

- `/usr/bin/uroot` 设置了 SUID 位（4755），意味着任何用户执行它时都会先以 root 身份运行程序本身
- 程序内部的验证流程（D-Bus 密码验证 + sudo 组检查）在提权之前完成，验证不通过不会获得 root 权限
- 用户名通过 `getuid()` 获取，无法伪造
- 密码输入后立即 `memset` 清零，不在内存中驻留
- 仍然建议仅在可信环境中使用，SUID root 程序本身是高权限攻击面

## 项目结构

```
uroot.c              # 主源码（SUID + D-Bus 认证 + sudo 组检查）
build.sh             # 构建 deb 包
install.sh           # 手动编译安装
DEBIAN/control       # deb 包元数据
deepin_auth.c        # D-Bus 认证独立参考实现
dbus认证命令.md       # D-Bus 认证过程文档
```

## 许可

仅供内部使用