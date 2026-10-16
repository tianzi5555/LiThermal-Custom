// LiOS 第一期：读取 /mnt/UDISK/lios.conf（开机密码配置）
//
// 文件格式（自己定的，键=值，一行一个，# 开头是注释）：
//     # camera=进相机的密码  lios=进 LiOS 的密码
//     camera=123456
//     lios=5555
//
// 硬要求：**文件缺失或解析失败 → 两个密码都为空 → 直接进相机，绝不锁死用户。**
// 所以这里所有出错路径都只是返回空串，不报错、不退出、不写回任何文件。

#include "lios_user.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 日志固定写 /tmp（红线要求：日志不写 SD 卡）
#define LIOS_LOG_PATH "/tmp/lios.log"

void lios_log(const char *fmt, ...)
{
    FILE *f = fopen(LIOS_LOG_PATH, "a");
    if (f == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    fflush(NULL);
}

#define LIOS_PW_MAX 32 // 密码最长长度（含结尾 0）

static char pw_camera[LIOS_PW_MAX] = {0};
static char pw_lios[LIOS_PW_MAX] = {0};
static bool conf_loaded = false;

// 给日志用的简单校验和（不泄露密码，但能区分不同密码 / 看出两个密码是不是同一个）
static unsigned pw_sum(const char *s)
{
    unsigned h = 0;
    if (s == NULL)
        return 0;
    while (*s)
        h = h * 131u + (unsigned char)(*s++);
    return h;
}

// 去掉首尾空白（空格/Tab/换行）
static char *trim(char *s)
{
    if (s == NULL)
        return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *end = s + strlen(s);
    while (end > s)
    {
        char c = *(end - 1);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#')
            end--;
        else
            break;
    }
    *end = '\0';
    return s;
}

void lios_config_load(void)
{
    pw_camera[0] = '\0';
    pw_lios[0] = '\0';
    conf_loaded = false;

    FILE *f = fopen(LIOS_CONF_PATH, "r");
    if (f == NULL)
    {
        // 配置文件不存在：不锁死用户，密码一律为空
        printf("[lios] %s 不存在，跳过登录（直接进相机）\n", LIOS_CONF_PATH);
        return;
    }

    char line[256];
    int line_no = 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line_no++;
        char *s = trim(line);
        if (s == NULL || s[0] == '\0' || s[0] == '#' || s[0] == ';')
            continue;

        char *eq = strchr(s, '=');
        if (eq == NULL)
            continue; // 没有 = 的行直接忽略（不报错）
        *eq = '\0';

        char *key = trim(s);
        char *val = trim(eq + 1);
        if (key == NULL || val == NULL || key[0] == '\0')
            continue;
        if (strlen(val) >= LIOS_PW_MAX)
        {
            printf("[lios] %s 第 %d 行密码太长，忽略\n", LIOS_CONF_PATH, line_no);
            continue;
        }

        if (strcmp(key, "camera") == 0)
        {
            snprintf(pw_camera, sizeof(pw_camera), "%s", val);
            conf_loaded = true;
        }
        else if (strcmp(key, "lios") == 0)
        {
            snprintf(pw_lios, sizeof(pw_lios), "%s", val);
            conf_loaded = true;
        }
    }
    fclose(f);
    // 只打日志，不打印密码本身（打印长度 + 简单校验和，方便一眼看出两个密码是否一样/串了）
    lios_log("配置 %s: camera密码 len=%d sum=%u / lios密码 len=%d sum=%u",
             LIOS_CONF_PATH,
             (int)strlen(pw_camera), pw_sum(pw_camera),
             (int)strlen(pw_lios), pw_sum(pw_lios));
    printf("[lios] 读 %s: camera密码%s(len=%d) / lios密码%s(len=%d)\n", LIOS_CONF_PATH,
           pw_camera[0] ? "已设置" : "未设置", (int)strlen(pw_camera),
           pw_lios[0] ? "已设置" : "未设置", (int)strlen(pw_lios));
}

bool lios_login_has_password(int mode)
{
    if (mode == LIOS_ENTER_LIOS)
        return pw_lios[0] != '\0';
    return pw_camera[0] != '\0';
}

int lios_login_check(const char *input)
{
    if (input == NULL || input[0] == '\0')
    {
        lios_log("密码判定: reject（空输入）");
        return -1;
    }
    // 顺序：先判相机密码（默认 123456）；两个密码配成一样时相机优先，不是安全问题
    if (pw_camera[0] != '\0' && strcmp(input, pw_camera) == 0)
    {
        lios_log("密码判定: camera（输入 len=%d sum=%u）", (int)strlen(input), pw_sum(input));
        return LIOS_ENTER_CAMERA;
    }
    if (pw_lios[0] != '\0' && strcmp(input, pw_lios) == 0)
    {
        lios_log("密码判定: lios（输入 len=%d sum=%u）", (int)strlen(input), pw_sum(input));
        return LIOS_ENTER_LIOS;
    }
    // 只记长度/校验和 + 跟两个密码的长度对比，不打印明文
    lios_log("密码判定: reject（输入 len=%d sum=%u；camera len=%d，lios len=%d）",
             (int)strlen(input), pw_sum(input), (int)strlen(pw_camera), (int)strlen(pw_lios));
    return -1;
}

// ---------------------------------------------------------------- 自检（只读，不改任何东西）

// 把配置内容和三条判定结果写进 /tmp/lios.log；main 里遇到 --selftest-pw 会调它。
// 只输出长度/校验和，不输出明文。返回值恒为 0。
int lios_selftest_pw(void)
{
    lios_log("=== 密码自检开始（只读，不显示界面）===");
    lios_config_load();
    lios_log("自检: camera len=%d sum=%u / lios len=%d sum=%u",
             (int)strlen(pw_camera), pw_sum(pw_camera),
             (int)strlen(pw_lios), pw_sum(pw_lios));
    lios_login_check(pw_camera); // 应当判成 camera
    lios_login_check(pw_lios);   // 应当判成 lios
    lios_login_check("zzzz");    // 应当 reject
    lios_log("=== 密码自检结束 ===");
    return 0;
}
