// gl-su：给 MT 管理器“自定义 su 路径”用的客户端原型
// - 运行在调用者（MT/应用/shell）的域与 uid 下，连接 abstract socket "ghostlock"
//   把命令交给 root 服务执行，并把输出/退出码带回。
// - 用法：gl-su -c <cmd>；兼容 su 语法 "gl-su [user] -c <cmd>"；
//   -v/--version 打印版本（有些 root 检测会先探这个）。
// - 需要执行权限：应用域只能 exec 标签为 shell_exec 的文件，broker 的 setlabel
//   命令负责把本文件临时改成 shell_exec（运行期操作，不落盘到系统分区）。
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define REQ_MAGIC  0x314c5347u
#define DONE_MAGIC 0x314c4447u

static int dial(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    static const char *nm = "ghostlock";
    sa.sun_path[0] = 0;
    strncpy(sa.sun_path + 1, nm, sizeof(sa.sun_path) - 2);
    socklen_t sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(nm));
    if (connect(s, (struct sockaddr *)&sa, sl) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int write_full(int fd, const void *p, size_t n) {
    size_t o = 0;
    while (o < n) {
        ssize_t r = write(fd, (const char *)p + o, n - o);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        o += (size_t)r;
    }
    return 0;
}

/* 交互模式：MT 会拉一个常驻 shell，用 stdin 发命令、stdout 收输出（sentinel
   "MT - End of command" 分隔）。这里把本地 stdio 与 socket 双向转发；不做
   尾包剥离（尾部 8 字节只在会话结束时出现，不能扣住中间输出）。 */
static int relay_interactive(int s) {
    signal(SIGPIPE, SIG_IGN);
    struct pollfd pf[2];
    uint8_t buf[4096];
    int in_open = 1;
    for (;;) {
        pf[0].fd = in_open ? 0 : -1;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = s;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        int pr = poll(pf, 2, -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n > 0) {
                if (write_full(s, buf, (size_t)n) < 0) break;
            } else {
                in_open = 0;
                shutdown(s, SHUT_WR);
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(s, buf, sizeof buf);
            if (n == 0) break;
            if (n < 0) { if (errno == EINTR) continue; break; }
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("ghostlock-su 0.1\n");
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: gl-su [-c] <command>\n");
            return 0;
        }
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cmd = argv[i + 1];
            break;
        }
    }
    if (!cmd) {
        /* 无 -c：MT 的常驻交互 shell 模式（也兼容带未知 flag 的调用） */
        int s = dial();
        if (s < 0) {
            fprintf(stderr, "gl-su: cannot connect to ghostlock service (%s)\n", strerror(errno));
            return 126;
        }
        static const char *icmd = "exec /system/bin/sh";
        struct { uint32_t magic; uint32_t len; } rq;
        rq.magic = REQ_MAGIC;
        rq.len = (uint32_t)strlen(icmd);
        if (write_full(s, &rq, sizeof rq) < 0 || write_full(s, icmd, rq.len) < 0) {
            fprintf(stderr, "gl-su: send failed (%s)\n", strerror(errno));
            close(s);
            return 126;
        }
        int rc = relay_interactive(s);
        close(s);
        return rc;
    }
    int s = dial();
    if (s < 0) {
        fprintf(stderr, "gl-su: cannot connect to ghostlock service (%s)\n", strerror(errno));
        return 126;
    }
    struct { uint32_t magic; uint32_t len; } rq;
    rq.magic = REQ_MAGIC;
    rq.len = (uint32_t)strlen(cmd);
    if (write_full(s, &rq, sizeof rq) < 0 || write_full(s, cmd, rq.len) < 0) {
        fprintf(stderr, "gl-su: send failed (%s)\n", strerror(errno));
        close(s);
        return 126;
    }
    /* 流式透传；保留最后 8 字节，若为 GLD1 尾包则只取 status 不打印。 */
    uint8_t buf[4096], tail[8];
    int tn = 0;
    for (;;) {
        ssize_t n = read(s, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (ssize_t k = 0; k < n; k++) {
            if (tn < 8) {
                tail[tn++] = buf[k];
            } else {
                fwrite(tail, 1, 1, stdout);
                memmove(tail, tail + 1, 7);
                tail[7] = buf[k];
            }
        }
    }
    fflush(stdout);
    int32_t status = 0;
    if (tn == 8) {
        uint32_t m;
        memcpy(&m, tail, 4);
        if (m == DONE_MAGIC) {
            memcpy(&status, tail + 4, 4);
            close(s);
            return status;
        }
    }
    if (tn > 0) fwrite(tail, 1, (size_t)tn, stdout);
    close(s);
    return 0;
}