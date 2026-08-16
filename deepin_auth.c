/*
 * deepin_auth.c - Deepin D-Bus 密码验证工具
 *
 * 编译: gcc -o deepin-auth deepin_auth.c $(pkg-config --cflags --libs gio-unix-2.0 glib-2.0)
 */

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

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
    fprintf(stderr, "[DEBUG] Status: flag=%d status=%d msg=%s\n", flag, status, msg ? msg : "");

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
        fprintf(stderr, "[DEBUG] SUCCESS: flag=%d status=%d msg=%s\n", flag, status, msg ? msg : "");
        data->result = 1;
        snprintf(data->msg, sizeof(data->msg), "%s", msg ? msg : "success");
        g_main_loop_quit(data->loop);
    } else if (status == STATUS_FAILURE) {
        fprintf(stderr, "[DEBUG] FAILURE: flag=%d status=%d msg=%s\n", flag, status, msg ? msg : "");
        data->result = 0;
        snprintf(data->msg, sizeof(data->msg), "%s", msg ? msg : "failure");
        g_main_loop_quit(data->loop);
    } else if (status == STATUS_ENDED) {
        fprintf(stderr, "[DEBUG] ENDED: flag=%d status=%d msg=%s\n", flag, status, msg ? msg : "");
        if (data->result < 0) {
            data->result = 0;
            snprintf(data->msg, sizeof(data->msg), "ended: %s", msg ? msg : "");
        }
        g_main_loop_quit(data->loop);
    } else {
        fprintf(stderr, "[DEBUG] OTHER: flag=%d status=%d msg=%s\n", flag, status, msg ? msg : "");
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
    fprintf(stderr, "[INFO] 认证会话: %s\n", session_path);

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

    if (data.result == 1) {
        printf("[OK] 密码验证成功: %s\n", data.msg);
        return 1;
    }
    printf("[FAIL] 密码验证失败: %s\n", data.msg);
    return 0;
}

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

int main(int argc, char *argv[])
{
    const char *username = g_get_user_name();
    const char *password = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0)
            && i + 1 < argc) {
            username = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--password") == 0)
                   && i + 1 < argc) {
            password = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("用法: %s [-u 用户名] [-p 密码]\n", argv[0]);
            return 0;
        }
    }

    char *pw_buf = NULL;
    if (!password) {
        pw_buf = read_password(username);
        if (!pw_buf) { fprintf(stderr, "[FAIL] 无法读取密码\n"); return 1; }
        password = pw_buf;
    }

    printf("[INFO] 正在验证用户 '%s' 的密码...\n", username);

    int ok = verify_password(username, password);

    if (pw_buf) {
        memset(pw_buf, 0, strlen(pw_buf));
        free(pw_buf);
    }
    return ok ? 0 : 1;
}
