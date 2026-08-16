#include <gio/gio.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * uroot - SUID root 命令包装器（带统信用户验证）
 * 用法: uroot <命令> [参数...]
 * 示例: uroot rm -rf /home/test/k.jpg
 *       uroot apt install xxx
 *
 * 编译: gcc -o uroot uroot.c $(pkg-config --cflags --libs gio-unix-2.0 glib-2.0)
 * 安装: chmod 4755 uroot && cp uroot /usr/bin/uroot
 */

/* ===== 统信 D-Bus 认证 ===== */

#define AUTH_SERVICE    "com.deepin.daemon.Authenticate"
#define AUTH_PATH       "/com/deepin/daemon/Authenticate"
#define AUTH_IFACE      "com.deepin.daemon.Authenticate"
#define SESSION_IFACE   "com.deepin.daemon.Authenticate.Session"

#define STATUS_SUCCESS   0
#define STATUS_FAILURE   1
#define STATUS_PROMPT    7
#define STATUS_ENDED    13
#define AUTH_FLAG_PASSWORD 1

typedef struct {
    GMainLoop *loop;
    GDBusConnection *bus;
    GDBusProxy *session;
    const char *password;
    int result;
    char msg[256];
} AuthData;

/* Status 信号回调 */
static void on_status(GDBusProxy *proxy, gchar *sender,
                      gchar *signal_name, GVariant *params,
                      gpointer user_data)
{
    AuthData *data = user_data;
    gint32 flag, status;
    const char *msg = NULL;

    if (g_strcmp0(signal_name, "Status") != 0)
        return;

    g_variant_get(params, "(ii&s)", &flag, &status, &msg);

    /* flag=-1 表示会话结束 */
    if (flag == -1) {
        if (data->result < 0) {
            data->result = 0;
            snprintf(data->msg, sizeof(data->msg), "ended: %s", msg ? msg : "");
        }
        g_main_loop_quit(data->loop);
        return;
    }

    if (status == STATUS_PROMPT) {
        /* 收到密码请求, 发送 SetToken */
        GVariant *bytes = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, data->password, strlen(data->password), 1);
        g_dbus_proxy_call(data->session, "SetToken",
                          g_variant_new("(i@ay)", AUTH_FLAG_PASSWORD, bytes),
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    } else if (status == STATUS_SUCCESS) {
        data->result = 1;
        snprintf(data->msg, sizeof(data->msg), "%s", msg ? msg : "success");
        g_main_loop_quit(data->loop);
    } else if (status == STATUS_FAILURE) {
        data->result = 0;
        snprintf(data->msg, sizeof(data->msg), "%s", msg ? msg : "failure");
        g_main_loop_quit(data->loop);
    } else if (status == STATUS_ENDED) {
        if (data->result < 0) {
            data->result = 0;
            snprintf(data->msg, sizeof(data->msg), "ended: %s", msg ? msg : "");
        }
        g_main_loop_quit(data->loop);
    }
}

/* Start 的异步回调 */
static void on_start_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    AuthData *data = user_data;
    GError *error = NULL;
    GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);

    if (!ret) {
        data->result = 0;
        snprintf(data->msg, sizeof(data->msg), "Start failed: %s",
                 error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_main_loop_quit(data->loop);
    } else {
        g_variant_unref(ret);
    }
}

/* Authenticate 的异步回调 */
static void on_auth_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    AuthData *data = user_data;
    GError *error = NULL;
    GVariant *ret = g_dbus_connection_call_finish(data->bus, res, &error);

    if (!ret) {
        fprintf(stderr, "[FAIL] 创建认证会话失败: %s\n",
                error ? error->message : "unknown");
        if (error) g_error_free(error);
        data->result = 0;
        snprintf(data->msg, sizeof(data->msg), "auth failed");
        g_main_loop_quit(data->loop);
        return;
    }

    const char *session_path = NULL;
    g_variant_get(ret, "(&s)", &session_path);

    /* 创建 Session 代理 */
    data->session = g_dbus_proxy_new_sync(
        data->bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
        AUTH_SERVICE, session_path, SESSION_IFACE, NULL, &error);
    g_variant_unref(ret);

    if (!data->session) {
        fprintf(stderr, "[FAIL] Session 代理失败: %s\n",
                error ? error->message : "unknown");
        if (error) g_error_free(error);
        data->result = 0;
        snprintf(data->msg, sizeof(data->msg), "proxy failed");
        g_main_loop_quit(data->loop);
        return;
    }

    /* 连接 Status 信号 */
    g_signal_connect(data->session, "g-signal", G_CALLBACK(on_status), data);

    /* 异步调用 Start */
    g_dbus_proxy_call(data->session, "Start",
                      g_variant_new("(ii)", AUTH_FLAG_PASSWORD, 0),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      on_start_ready, data);
}

/* 通过统信 D-Bus 验证密码 */
static int verify_password(const char *username, const char *password)
{
    GError *error = NULL;
    AuthData data = {
        .loop = NULL, .bus = NULL, .session = NULL,
        .password = password, .result = -1, .msg = {0}
    };

    data.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!data.bus) {
        fprintf(stderr, "[FAIL] 系统总线连接失败: %s\n", error->message);
        g_error_free(error);
        return 0;
    }

    data.loop = g_main_loop_new(NULL, FALSE);

    /* 异步调用 Authenticate */
    g_dbus_connection_call(data.bus,
        AUTH_SERVICE, AUTH_PATH, AUTH_IFACE, "Authenticate",
        g_variant_new("(sii)", username, AUTH_FLAG_PASSWORD, 1),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_auth_ready, &data);

    /* 超时 30s */
    g_timeout_add_seconds(30, (GSourceFunc)g_main_loop_quit, data.loop);

    g_main_loop_run(data.loop);

    g_main_loop_unref(data.loop);
    if (data.session) g_object_unref(data.session);
    g_object_unref(data.bus);

    return data.result == 1;
}

/* 无回显读取密码 */
static char *read_password(const char *username)
{
    struct termios oldt, newt;
    char *line = NULL;
    size_t len = 0;

    printf("请输入 %s 的密码: ", username);
    fflush(stdout);

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    ssize_t n = getline(&line, &len, stdin);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    printf("\n");

    if (n <= 0) { free(line); return NULL; }
    if (line[n - 1] == '\n') line[n - 1] = '\0';
    return line;
}

/* 获取真实用户名（SUID 下 getuid 返回调用者 UID） */
static const char *get_real_username(void)
{
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : NULL;
}

/* 检查用户是否属于 sudo 组 */
static int is_user_in_sudo_group(const char *username)
{
    struct passwd *pw = getpwnam(username);
    if (!pw) return 0;

    struct group *gr = getgrnam("sudo");
    if (!gr) {
        fprintf(stderr, "[FAIL] 系统中不存在 sudo 组\n");
        return 0;
    }

    /* 检查主组 */
    if (pw->pw_gid == gr->gr_gid) return 1;

    /* 检查补充组 */
    int ngroups = 0;
    getgrouplist(username, pw->pw_gid, NULL, &ngroups);
    if (ngroups <= 0) return 0;

    gid_t *groups = malloc(sizeof(gid_t) * ngroups);
    if (!groups) return 0;

    if (getgrouplist(username, pw->pw_gid, groups, &ngroups) < 0) {
        free(groups);
        return 0;
    }

    int found = 0;
    for (int i = 0; i < ngroups; i++) {
        if (groups[i] == gr->gr_gid) {
            found = 1;
            break;
        }
    }

    free(groups);
    return found;
}
/* ===== 主流程 ===== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: uroot <命令> [参数...]\n");
        fprintf(stderr, "示例: uroot rm -rf /home/test/k.jpg\n");
        fprintf(stderr, "      uroot apt install xxx\n");
        return 1;
    }

    /* 1. 获取真实用户名 */
    const char *username = get_real_username();
    if (!username) {
        fprintf(stderr, "[FAIL] 无法获取当前用户名\n");
        return 1;
    }

    /* 2. 读取密码 */
    char *password = read_password(username);
    if (!password) {
        fprintf(stderr, "[FAIL] 无法读取密码\n");
        return 1;
    }

    /* 3. 统信 D-Bus 验证 */
    int ok = verify_password(username, password);

    /* 立即清除密码 */
    memset(password, 0, strlen(password));
    free(password);

    /* 4. 密码验证失败则退出 */
    if (!ok) {
        fprintf(stderr, "[FAIL] 用户验证失败，拒绝执行\n");
        return 1;
    }

    /* 5. 检查用户是否属于 sudo 组 */
    if (!is_user_in_sudo_group(username)) {
        fprintf(stderr, "[FAIL] 用户 '%s' 不属于 sudo 组，拒绝执行\n", username);
        return 1;
    }

    /* 6. 验证全部通过，提升到 root 权限 */
    if (setgid(0) != 0) {
        perror("uroot: setgid");
        return 1;
    }
    if (setuid(0) != 0) {
        perror("uroot: setuid");
        return 1;
    }

    /* 7. 以 root 身份执行传入的命令 */
    execvp(argv[1], &argv[1]);

    /* execvp 仅在出错时返回 */
    perror("uroot");
    return 1;
}