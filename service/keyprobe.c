// 探测当前进程各 keyring 与 fscrypt keyring 的可见性/可搜索性/可链接性
// 用于对比 adb shell 与 Shizuku shell 环境下的 keyring 权限差异。
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long kc(long op, long a2, long a3, long a4, long a5) {
    return syscall(__NR_keyctl, op, a2, a3, a4, a5);
}

static void dump_one(const char *tag, long spec) {
    errno = 0;
    long id = kc(0 /*GET_KEYRING_ID*/, spec, 0, 0, 0);
    printf("[%s] spec=%ld id=%ld errno=%d\n", tag, spec, id, errno);
    if (id < 0) return;
    char d[192];
    memset(d, 0, sizeof d);
    errno = 0;
    long dr = kc(6 /*DESCRIBE*/, id, (long)d, sizeof d - 1, 0);
    printf("  desc r=%ld errno=%d '%s'\n", dr, errno, d);
    int32_t ids[64];
    memset(ids, 0, sizeof ids);
    errno = 0;
    long n = kc(11 /*READ*/, id, (long)ids, sizeof ids, 0);
    printf("  read n=%ld errno=%d\n", n, errno);
    if (n > 0 && (n % 4) == 0) {
        for (long i = 0; i < n / 4; i++) {
            char cd[192];
            memset(cd, 0, sizeof cd);
            errno = 0;
            long r = kc(6, ids[i], (long)cd, sizeof cd - 1, 0);
            printf("    child[%ld] ser=%d desc_r=%ld errno=%d '%s'\n",
                   i, ids[i], r, errno, cd);
        }
    }
}

int main(void) {
    printf("uid=%d gid=%d pid=%d\n", getuid(), getgid(), getpid());
    dump_one("thread", -1);
    dump_one("process", -2);
    dump_one("session", -3);
    dump_one("user", -4);
    dump_one("user-session", -5);

    long sess = kc(0, -3, 0, 0, 0);
    printf("session serial=%ld\n", sess);

    const char *specs[5] = {"thread", "process", "session", "user", "user-session"};
    long sv[5] = {-1, -2, -3, -4, -5};
    for (int i = 0; i < 5; i++) {
        errno = 0;
        long sr = kc(10 /*SEARCH*/, sv[i], (long)"keyring", (long)"fscrypt", 0);
        printf("search_nodest[%s]=%ld errno=%d\n", specs[i], sr, errno);
        errno = 0;
        long sl = kc(10, sv[i], (long)"keyring", (long)"fscrypt", sess);
        printf("search_dest_session[%s]=%ld errno=%d\n", specs[i], sl, errno);
    }

    errno = 0;
    long j = kc(1 /*JOIN*/, (long)"fscrypt", 0, 0, 0);
    printf("join(fscrypt)=%ld errno=%d\n", j, errno);
    long s2 = kc(0, -3, 0, 0, 0);
    printf("after_join session serial=%ld errno=%d\n", s2, errno);

    errno = 0;
    long j2 = kc(1, (long)"_uid_ses.2000", 0, 0, 0);
    printf("join(_uid_ses.2000)=%ld errno=%d\n", j2, errno);
    long s3 = kc(0, -3, 0, 0, 0);
    printf("after_join2 session serial=%ld errno=%d\n", s3, errno);
    dump_one("session_after", -3);
    printf("DONE\n");
    return 0;
}