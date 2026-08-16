# D-Bus 认证命令过程

本文档记录 cxkj 中使用的统信（Deepin/UOS）密码验证的完整 D-Bus 调用过程。

## 概览

cxkj 通过系统 D-Bus 总线调用 `com.deepin.daemon.Authenticate` 服务，验证当前用户的密码。整个流程涉及一次方法调用、一个会话代理、以及多个信号交互。

```
用户 -> cxkj -> 系统D-Bus -> com.deepin.daemon.Authenticate 服务
```

## D-Bus 基本信息

| 项目 | 值 |
|------|------|
| 总线类型 | System Bus（系统总线） |
| 服务名 | `com.deepin.daemon.Authenticate` |
| 对象路径 | `/com/deepin/daemon/Authenticate` |
| 接口名 | `com.deepin.daemon.Authenticate` |
| 会话接口 | `com.deepin.daemon.Authenticate.Session` |

## 状态码定义

| 状态码 | 常量 | 含义 |
|--------|------|------|
| 0 | STATUS_SUCCESS | 验证成功 |
| 1 | STATUS_FAILURE | 验证失败 |
| 7 | STATUS_PROMPT | 请求输入密码 |
| 13 | STATUS_ENDED | 会话结束 |
| -1 (flag) | - | 会话结束标志 |

## 认证流程

### 第一步：连接系统总线

```c
GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
```

连接到 System Bus，获取 D-Bus 连接。

### 第二步：调用 Authenticate 方法

```c
g_dbus_connection_call(bus,
    "com.deepin.daemon.Authenticate",     // 服务名
    "/com/deepin/daemon/Authenticate",    // 对象路径
    "com.deepin.daemon.Authenticate",     // 接口名
    "Authenticate",                        // 方法名
    g_variant_new("(sii)", username, 1, 1),  // 参数：用户名, PASSWORD标志, 1
    G_VARIANT_TYPE("(s)"),               // 返回类型：会话路径
    G_DBUS_CALL_FLAGS_NONE, -1, NULL,
    on_auth_ready, &data);                // 异步回调
```

**参数说明：**

- 参数1（s）：用户名，如 `"daniel"`
- 参数2（i）：认证标志，`1` 表示 PASSWORD 认证
- 参数3（i）：`1`

**返回值：**

- 返回一个会话对象路径，如 `/com/deepin/daemon/Authenticate/Session/xxx`

### 第三步：创建会话代理

```c
GDBusProxy *session = g_dbus_proxy_new_sync(
    bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
    "com.deepin.daemon.Authenticate",     // 服务名
    session_path,                          // 上一步返回的会话路径
    "com.deepin.daemon.Authenticate.Session",  // 会话接口
    NULL, &error);
```

用返回的会话路径创建代理，后续通过这个代理与会话交互。

### 第四步：连接 Status 信号

```c
g_signal_connect(session, "g-signal", G_CALLBACK(on_status), &data);
```

监听会话的 `Status` 信号，认证服务通过这个信号推送状态变化。

信号参数格式：

```
(ii&s)
```

即：`(flag: int32, status: int32, message: string)`

### 第五步：调用 Start 方法

```c
g_dbus_proxy_call(session, "Start",
    g_variant_new("(ii)", 1, 0),          // 参数：PASSWORD标志, 0
    G_DBUS_CALL_FLAGS_NONE, -1, NULL,
    on_start_ready, &data);
```

启动认证会话。

### 第六步：收到 STATUS_PROMPT，发送密码

当 Status 信号返回 `status=7`（PROMPT）时，说明服务在请求密码：

```c
GVariant *bytes = g_variant_new_fixed_array(
    G_VARIANT_TYPE_BYTE, password, strlen(password), 1);

g_dbus_proxy_call(session, "SetToken",
    g_variant_new("(i@ay)", 1, bytes),    // 参数：PASSWORD标志, 密码字节数组
    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
```

将用户输入的密码作为字节数组（`ay` 类型）发送给认证服务。

### 第七步：等待验证结果

认证服务通过 Status 信号返回最终结果：

**验证成功：**

```
Status 信号: flag=任意, status=0 (SUCCESS), msg="..."
```

**验证失败：**

```
Status 信号: flag=任意, status=1 (FAILURE), msg="..."
```

**会话结束：**

```
Status 信号: flag=-1, status=任意, msg="..."
```

### 第八步：清理

```c
g_main_loop_quit(loop);       // 退出事件循环
g_object_unref(session);      // 释放会话代理
g_object_unref(bus);          // 释放总线连接
```

## 完整流程图

```
cxkj                        Authenticate 服务
 │                                │
 ├── Authenticate(username,1,1) ──>│
 │                                │
 │<── 返回 session_path ──────────│
 │                                │
 ├── 创建 Session 代理            │
 ├── 监听 Status 信号             │
 │                                │
 ├── Start(1, 0) ────────────────>│
 │                                │
 │<── Status(flag, 7, msg) ───────│  请求密码
 │                                │
 ├── SetToken(1, password) ──────>│  发送密码
 │                                │
 │<── Status(flag, 0/1, msg) ─────│  返回结果
 │                                │
 ├── 判断结果                      │
 │   status=0 → 成功              │
 │   status=1 → 失败              │
 │                                │
 ├── 清理资源                      │
 └── 返回                          │
```

## 超时处理

代码中设置了 30 秒超时：

```c
g_timeout_add_seconds(30, (GSourceFunc)g_main_loop_quit, loop);
```

如果 30 秒内没有收到结果，强制退出事件循环，按失败处理。

## 对应代码

以上流程对应 `cxkj.c` 中的以下函数：

| 函数 | 作用 |
|------|------|
| `on_status()` | Status 信号回调，处理各状态码 |
| `on_start_ready()` | Start 方法回调 |
| `on_auth_ready()` | Authenticate 方法回调，创建会话代理 |
| `verify_password()` | 主验证函数，串联整个流程 |
| `read_password()` | 无回显读取用户输入的密码 |

## 调试命令

手动测试 D-Bus 认证（在统信系统上）：

```bash
# 查看 Authenticate 服务是否在运行
busctl --system list | grep Authenticate

# 查看服务详细信息
busctl --system introspect com.deepin.daemon.Authenticate /com/deepin/daemon/Authenticate

# 手动调用 Authenticate 方法
busctl --system call com.deepin.daemon.Authenticate \
    /com/deepin/daemon/Authenticate \
    com.deepin.daemon.Authenticate \
    Authenticate "sii" "用户名" 1 1
```

## 以 uos 用户为例的完整 Bash 操作过程

以下以用户名 `uos` 为例，在统信系统终端中逐步执行，模拟 cxkj 的 D-Bus 认证全过程。

### 准备：确认认证服务在运行

```bash
busctl --system list | grep Authenticate
```

预期输出：

```
com.deepin.daemon.Authenticate     - -       -        - -
```

如果没有输出，说明认证服务未启动。

### 第一步：调用 Authenticate 创建认证会话

```bash
SESSION_PATH=$(busctl --system call \
    com.deepin.daemon.Authenticate \
    /com/deepin/daemon/Authenticate \
    com.deepin.daemon.Authenticate \
    Authenticate \
    "sii" "uos" 1 1 \
    | awk -F'"' '{print $2}')

echo "会话路径: $SESSION_PATH"
```

预期输出：

```
会话路径: /com/deepin/daemon/Authenticate/Session/uos_xxx
```

**命令解析：**

```bash
# 完整命令展开（不用变量捕获时直接执行）
busctl --system call \
    com.deepin.daemon.Authenticate \        # 服务名
    /com/deepin/daemon/Authenticate \       # 对象路径
    com.deepin.daemon.Authenticate \        # 接口名
    Authenticate \                           # 方法名
    "sii" "uos" 1 1                          # 签名：字符串+整数+整数
```

- `"uos"` -> 用户名
- `1` -> AUTH_FLAG_PASSWORD（密码认证）
- `1` -> 固定参数

返回值格式：

```
s "/com/deepin/daemon/Authenticate/Session/uos_xxx"
```

### 第二步：后台监听 Status 信号

另开一个终端（或用后台进程）监听该会话的信号：

```bash
# 方式一：用 dbus-monitor
dbus-monitor --system \
    "type='signal',interface='com.deepin.daemon.Authenticate.Session',path='$SESSION_PATH'"

# 方式二：用 busctl monitor
busctl --system monitor com.deepin.daemon.Authenticate
```

### 第三步：调用 Start 启动认证

```bash
busctl --system call \
    com.deepin.daemon.Authenticate \
    "$SESSION_PATH" \
    com.deepin.daemon.Authenticate.Session \
    Start \
    "ii" 1 0
```

**命令解析：**

- `1` -> AUTH_FLAG_PASSWORD
- `0` -> 固定参数

执行后，监听端会收到第一个 Status 信号：

```
signal  sender=:1.xxx -> destination=(null)
        path=/com/deepin/daemon/Authenticate/Session/uos_xxx
        interface=com.deepin.daemon.Authenticate.Session
        member=Status
        int32 0
        int32 7
        string "Password"
```

其中 `status=7` 表示 STATUS_PROMPT，服务在请求密码。

### 第四步：发送密码（SetToken）

假设 uos 用户的密码是 `123456`。

```bash
# 密码转换为字节数组
PASSWORD="123456"
# busctl 的 ay 类型需要逐字节传入，以十进制 ASCII 码表示
# "123456" -> 49 50 51 52 53 54

busctl --system call \
    com.deepin.daemon.Authenticate \
    "$SESSION_PATH" \
    com.deepin.daemon.Authenticate.Session \
    SetToken \
    "iay" 1 49 50 51 52 53 54
```

**命令解析：**

- 签名 `"iay"` = 整数 + 字节数组
- `1` -> AUTH_FLAG_PASSWORD
- `49 50 51 52 53 54` -> 密码 "123456" 每个字符的 ASCII 十进制值

常用字符 ASCII 对照：

| 字符 | 十进制 |
|------|--------|
| 0-9 | 48-57 |
| a-z | 97-122 |
| A-Z | 65-90 |
| ! | 33 |
| @ | 64 |

可以用以下命令自动转换任意密码：

```bash
PASSWORD="123456"
BYTES=$(echo -n "$PASSWORD" | od -An -tu1 | tr -s ' ' | sed 's/^ //;s/ $//')
echo "密码字节数组: $BYTES"
# 输出: 49 50 51 52 53 54

busctl --system call \
    com.deepin.daemon.Authenticate \
    "$SESSION_PATH" \
    com.deepin.daemon.Authenticate.Session \
    SetToken \
    "iay" 1 $BYTES
```

### 第五步：查看验证结果

发送密码后，监听端会收到最终的 Status 信号。

**密码正确（验证成功）：**

```
signal  sender=:1.xxx -> destination=(null)
        path=/com/deepin/daemon/Authenticate/Session/uos_xxx
        interface=com.deepin.daemon.Authenticate.Session
        member=Status
        int32 0
        int32 0
        string "success"
```

关键：`status=0`（STATUS_SUCCESS）。

**密码错误（验证失败）：**

```
signal  sender=:1.xxx -> destination=(null)
        path=/com/deepin/daemon/Authenticate/Session/uos_xxx
        interface=com.deepin.daemon.Authenticate.Session
        member=Status
        int32 0
        int32 1
        string "verify failed"
```

关键：`status=1`（STATUS_FAILURE）。

### 第六步：会话结束

无论成功或失败，服务最终会发送结束信号：

```
signal  sender=:1.xxx -> destination=(null)
        path=/com/deepin/daemon/Authenticate/Session/uos_xxx
        interface=com.deepin.daemon.Authenticate.Session
        member=Status
        int32 -1
        int32 13
        string ""
```

其中 `flag=-1` 表示会话已结束。

### 完整一键测试脚本

将以下内容保存为 `test_dbus_auth.sh`，可直接测试 uos 用户认证：

```bash
#!/bin/bash
#
# test_dbus_auth.sh - D-Bus 认证测试脚本
# 用法: bash test_dbus_auth.sh 用户名 密码
#

USERNAME="${1:-uos}"
PASSWORD="${2:-123456}"

echo "===== 统信 D-Bus 认证测试 ====="
echo "用户名: $USERNAME"
echo ""

# 第一步：创建认证会话
echo "[步骤1] 调用 Authenticate 创建会话..."
SESSION_PATH=$(busctl --system call \
    com.deepin.daemon.Authenticate \
    /com/deepin/daemon/Authenticate \
    com.deepin.daemon.Authenticate \
    Authenticate \
    "sii" "$USERNAME" 1 1 \
    | awk -F'"' '{print $2}')

if [ -z "$SESSION_PATH" ]; then
    echo "[FAIL] 创建会话失败"
    exit 1
fi
echo "  会话路径: $SESSION_PATH"
echo ""

# 第二步：密码转字节数组
echo "[步骤2] 转换密码为字节数组..."
BYTES=$(echo -n "$PASSWORD" | od -An -tu1 | tr -s ' ' | sed 's/^ //;s/ $//')
echo "  字节数组: $BYTES"
echo ""

# 第三步：启动认证并后台监听
echo "[步骤3] 启动认证，后台监听信号..."

# 将监听输出写入临时文件
TMPFILE=$(mktemp)
dbus-monitor --system \
    "type='signal',interface='com.deepin.daemon.Authenticate.Session',path='$SESSION_PATH'" \
    > "$TMPFILE" &
MONITOR_PID=$!
sleep 0.5

# 调用 Start
busctl --system call \
    com.deepin.daemon.Authenticate \
    "$SESSION_PATH" \
    com.deepin.daemon.Authenticate.Session \
    Start \
    "ii" 1 0 > /dev/null 2>&1

sleep 0.5

# 第四步：发送密码
echo "[步骤4] 发送密码 (SetToken)..."
busctl --system call \
    com.deepin.daemon.Authenticate \
    "$SESSION_PATH" \
    com.deepin.daemon.Authenticate.Session \
    SetToken \
    "iay" 1 $BYTES > /dev/null 2>&1

# 第五步：等待结果
echo "[步骤5] 等待验证结果..."
sleep 2

# 停止监听
kill $MONITOR_PID 2>/dev/null
wait $MONITOR_PID 2>/dev/null

# 判断结果
if grep -q "int32 0$" "$TMPFILE" && grep -q "string \"success\"" "$TMPFILE"; then
    echo ""
    echo "===== 验证成功 ====="
    rm -f "$TMPFILE"
    exit 0
elif grep -q "int32 1$" "$TMPFILE"; then
    echo ""
    echo "===== 验证失败（密码错误）====="
    rm -f "$TMPFILE"
    exit 1
else
    echo ""
    echo "===== 未知结果，原始输出如下 ====="
    cat "$TMPFILE"
    rm -f "$TMPFILE"
    exit 2
fi