/**
 * @file dispVsync.cpp
 *
 * /dev/disp + DISP_VSYNC_EVENT_EN 的 vsync（帧头）等待，见 dispVsync.h 的说明。
 *
 * 安全性设计（很重要，错了会卡住 UI）：
 *  - 每次等待都用 poll() + 超时（默认 30ms），最坏情况只是多花 30ms，绝不会永久卡住；
 *  - 等待前先把 netlink 里积压的消息全部抽干（否则拿到的是「上一次」vsync 的旧消息，
 *    等于没同步）——VSYNC 消息约 100Hz，而整屏 flush 约 25Hz，必然有积压；
 *  - 连续 40 次「等到的耗时 < 500µs」或累计 3 次超时 → 判定这条通道不可用，
 *    永久停用（disp_vsync_wait_us 立刻返回 -1），并在 /tmp/perf.log 里写明原因。
 */

#include "dispVsync.h"
#include "perfProbe.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/* sunxi_display2.h: enum tag_DISP_CMD { ... DISP_VSYNC_EVENT_EN = 0x0B ... }
   内核 dev_disp.c:3708 用它取 ubuffer[0]=disp, ubuffer[1]=enable（一次 copy 4 个 unsigned long） */
#define DISP_VSYNC_EVENT_EN 0x0B

#define DISP_PATH "/dev/disp"
#define VSYNC_MSG_MAX 1024
#define VSYNC_TOO_SHORT_US 500   /* 小于这个值就认为「没真的等」 */
#define VSYNC_SHORT_LIMIT 40     /* 连续这么多次短等待就停用 */
#define VSYNC_TIMEOUT_LIMIT 5    /* 连续这么多次超时就停用（偶发错过不算） */
#define VSYNC_RECV_TRIES 50      /* 单次等待里最多处理这么多条消息 */

static int s_disp_fd = -1;
static int s_net_fd = -1;
static int s_available = 0;
static int s_inited = 0;
static int s_short_streak = 0;
static int s_timeout_streak = 0;
static unsigned int s_wait_cnt = 0;
static unsigned int s_wait_max_us = 0;

int disp_vsync_is_available(void) { return s_available; }

void disp_vsync_disable(const char *why)
{
    if (s_available) {
        perf_probe_gauge(PERF_G_DISPVSYNC_OK, 0);
        perf_probe_note("disp_vsync: DISABLED (%s) after %u waits, max=%uus",
                why ? why : "?", s_wait_cnt, s_wait_max_us);
    }
    s_available = 0;
}

/* 非阻塞抽干 netlink 队列 */
void disp_vsync_drain(void)
{
    char buf[VSYNC_MSG_MAX];
    int guard = 0;

    if (s_net_fd < 0)
        return;
    while (guard++ < 512) {
        ssize_t n = recv(s_net_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0)
            break;
    }
}

/* 从 uevent 原始报文（NUL 分隔）里找 "VSYNC0=" */
static int msg_has_vsync(const char *buf, ssize_t len)
{
    static const char key[] = "VSYNC0=";
    ssize_t i;

    if (len < (ssize_t) sizeof(key))
        return 0;
    for (i = 0; i + (ssize_t) sizeof(key) <= len; i++) {
        if (memcmp(buf + i, key, sizeof(key) - 1) == 0)
            return 1;
    }
    return 0;
}

int disp_vsync_init(void)
{
    struct sockaddr_nl sa;
    int rcvbuf = 1 << 20; /* 1MB，防止 ~100Hz 的消息把缓冲塞满 */
    unsigned long args[4];
    int ret;

    if (s_inited)
        return s_available ? 0 : -1;
    s_inited = 1;

    s_disp_fd = open(DISP_PATH, O_RDWR);
    if (s_disp_fd < 0) {
        perf_probe_note("disp_vsync: open(" DISP_PATH ") failed errno=%d", errno);
        return -1;
    }

    s_net_fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (s_net_fd < 0) {
        perf_probe_note("disp_vsync: netlink socket failed errno=%d", errno);
        return -1;
    }
    setsockopt(s_net_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; /* kernel uevents */
    if (bind(s_net_fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        perf_probe_note("disp_vsync: netlink bind(group1) failed errno=%d", errno);
        return -1;
    }

    /* 打开 vsync 事件（内核 dev_disp.c:3708 -> bsp_disp_vsync_event_enable） */
    args[0] = 0; /* disp index 0 */
    args[1] = 1; /* enable */
    args[2] = 0;
    args[3] = 0;
    ret = ioctl(s_disp_fd, DISP_VSYNC_EVENT_EN, args);

    s_available = (ret == 0) ? 1 : 0;
    perf_probe_gauge(PERF_G_DISPVSYNC_OK, (unsigned int) s_available);
    perf_probe_note("disp_vsync: DISP_VSYNC_EVENT_EN ret=%d errno=%d available=%d",
            ret, errno, s_available);

    if (s_available) {
        /* 一次性自检：等一次，把实测耗时写进日志（117Hz 面板应为 0..8500µs） */
        int w = disp_vsync_wait_us(50);
        perf_probe_note("disp_vsync: selftest wait_us=%d (0..~8500 才说明真的等到了帧头)", w);
        if (w < 0) {
            /* 自检超时不一定代表彻底没用（可能刚好错过），交给后面的自动降级判断 */
            perf_probe_note("disp_vsync: selftest timed out, 仍继续观察");
        }
    }
    return s_available ? 0 : -1;
}

int disp_vsync_wait_us(int timeout_ms)
{
    char buf[VSYNC_MSG_MAX];
    uint32_t t0;
    uint32_t deadline_us;
    int tries;

    if (!s_available || s_net_fd < 0)
        return -1;

    /* 关键：先抽干积压，保证等到的是「下一次」边沿 */
    disp_vsync_drain();

    t0 = perf_probe_now_us();
    /* 总超时是硬上限：整条循环最多花 timeout_ms，绝不会把持锁的调用方卡住更久 */
    deadline_us = t0 + (uint32_t) timeout_ms * 1000u;

    for (tries = 0; tries < VSYNC_RECV_TRIES; tries++) {
        struct pollfd pfd;
        uint32_t now;
        int remain_ms;
        int pr;
        ssize_t n;

        now = perf_probe_now_us();
        if ((int32_t)(deadline_us - now) <= 0)
            remain_ms = -1; /* 已经超时 */
        else
            remain_ms = (int)((deadline_us - now) / 1000u) + 1;

        if (remain_ms < 0) {
            if (++s_timeout_streak >= VSYNC_TIMEOUT_LIMIT)
                disp_vsync_disable("poll timeout");
            return -1;
        }

        pfd.fd = s_net_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, remain_ms);
        if (pr == 0) {
            if (++s_timeout_streak >= VSYNC_TIMEOUT_LIMIT)
                disp_vsync_disable("poll timeout");
            return -1;
        }
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            disp_vsync_disable("poll error");
            return -1;
        }

        n = recv(s_net_fd, buf, sizeof(buf), 0);
        if (n <= 0)
            continue;
        if (!msg_has_vsync(buf, n))
            continue; /* 别的 uevent，继续等 */

        {
            uint32_t us = perf_probe_now_us() - t0;
            s_wait_cnt++;
            s_timeout_streak = 0;
            if (us > s_wait_max_us)
                s_wait_max_us = us;
            if (us < VSYNC_TOO_SHORT_US) {
                if (++s_short_streak >= VSYNC_SHORT_LIMIT)
                    disp_vsync_disable("waits are never blocking (stale/no vsync)");
            } else {
                s_short_streak = 0;
            }
            return (int) us;
        }
    }
    return -1;
}
