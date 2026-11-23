// root 服务端（glsrv，多连接版）
// 由 ghost1 的 serve 命令 fork 出来的子进程 exec 链启动：
//   fork(T, 继承 fake root creds) -> setcon adbd 建抽象 socket(标签 adbd)
//   -> exec /system/bin/sh （adbd->shell 过渡，shell 域 permissive）
//   -> exec 本程序（shell 域，uid0，继承 fscrypt session keyring）
// 每个 accept 连接 fork 一个 handler 进程处理，支持 MT 常驻 shell 与管理器并发。
// 协议：
//   请求  { u32 magic='GLS1', u32 len, char cmd[len] }
//   响应  子进程 stdout/stderr 原样流回 + 尾包 { u32 magic='GLD1', i32 status }
//   尾包后 close；客户端读到 EOF 后按尾包取退出码。
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQ_MAGIC  0x314c5347u   /* 小端 "GLS1" */
#define DONE_MAGIC 0x314c4447u   /* 小端 "GLD1" */
#define MAX_CMD    65536
#define ALLOW_PATH "/data/adb/gl_allow"   /* 每行一个 uid，或 "*" 允许所有 */

static int g_lf = -1;

static void slog(const char *fmt, ...) {
    if (g_lf < 0) {
        g_lf = open("/data/local/tmp/glsrv.log",
                    O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666);
        if (g_lf < 0) return;
    }
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) { if (write(g_lf, b, n) < 0) {} }
}

/* 极简授权：文件缺失时只允许 shell(2000)（便于用 gl-su 自己追加 uid） */
static int uid_allowed(int uid) {
    int fd = open(ALLOW_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return uid == 2000;
    char b[1024];
    int n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *p = b;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!strcmp(p, "*")) return 1;
        if (atoi(p) == uid) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static ssize_t read_full(int fd, void *p, size_t n) {
    size_t o = 0;
    while (o < n) {
        ssize_t r = read(fd, (char *)p + o, n - o);
        if (r == 0) return (ssize_t)o;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        o += (size_t)r;
    }
    return (ssize_t)o;
}

static void handle_conn(int c) {
    struct ucred cr;
    socklen_t cl = sizeof cr;
    memset(&cr, 0, sizeof cr);
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0) {
        slog("[srv] peercred err=%d\n", errno);
        close(c);
        return;
    }
    if (!uid_allowed((int)cr.uid)) {
        slog("[srv] DENY uid=%d pid=%d\n", cr.uid, cr.pid);
        char msg[96];
        int mn = snprintf(msg, sizeof msg,
                          "gl-su: uid %d is not authorized\n", (int)cr.uid);
        if (mn > 0) { if (write(c, msg, mn) < 0) {} }
        struct { uint32_t magic; int32_t status; } dtr;
        dtr.magic = DONE_MAGIC;
        dtr.status = 126;
        if (write(c, &dtr, sizeof dtr) < 0) {}
        close(c);
        return;
    }
    struct { uint32_t magic; uint32_t len; } rq;
    if (read_full(c, &rq, sizeof rq) != (ssize_t)sizeof rq ||
        rq.magic != REQ_MAGIC || rq.len == 0 || rq.len > MAX_CMD) {
        slog("[srv] bad req uid=%d pid=%d magic=%08x len=%u\n",
             cr.uid, cr.pid, rq.magic, rq.len);
        close(c);
        return;
    }
    char *cmd = malloc(rq.len + 1);
    if (!cmd || read_full(c, cmd, rq.len) != (ssize_t)rq.len) {
        free(cmd);
        close(c);
        return;
    }
    cmd[rq.len] = 0;
    slog("[srv] conn uid=%d pid=%d cmd=%.300s\n", cr.uid, cr.pid, cmd);
    pid_t p = fork();
    if (p == 0) {
        dup2(c, 0);
        dup2(c, 1);
        dup2(c, 2);
        execl("/system/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }
    int st = 0, status = 127;
    if (p > 0) {
        if (waitpid(p, &st, 0) == p)
            status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    } else {
        slog("[srv] fork err=%d\n", errno);
    }
    struct { uint32_t magic; int32_t status; } tr;
    tr.magic = DONE_MAGIC;
    tr.status = status;
    if (write(c, &tr, sizeof tr) < 0) {}
    slog("[srv] done uid=%d status=%d\n", cr.uid, status);
    close(c);
    free(cmd);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    int lf = (argc > 1) ? atoi(argv[1]) : 3;
    slog("[srv] start lf=%d uid=%d gid=%d pid=%d\n", lf, getuid(), getgid(), getpid());
    for (;;) {
        int c = accept(lf, NULL, NULL);
        if (c < 0) {
            if (errno != EINTR) usleep(100000);
            continue;
        }
        pid_t hp = fork();
        if (hp == 0) {
            /* handler：独占该连接；命令子进程和尾包都在这里处理 */
            close(lf);
            handle_conn(c);
            _exit(0);
        }
        if (hp < 0) slog("[srv] handler fork err=%d\n", errno);
        close(c);
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
}