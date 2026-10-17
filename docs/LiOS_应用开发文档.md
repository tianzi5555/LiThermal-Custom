# LiThermal 相机 · 应用开发文档

> 适用机型：LiThermal 热成像相机（全志 **T113-S3** / Tina Linux / LVGL 8.4）
> 本文档描述如何为这台相机开发、编译、安装、调试应用（插件）。
> 文档中所有路径与端点都在真机上核实过；**无法核实的条目标了「未验证」**，不编。

---

## 目录

1. [平台速览](#1-平台速览)
2. [系统结构：桌面是"家"，相机只是应用](#2-系统结构桌面是家相机只是应用)
3. [应用（插件）ABI —— 核心](#3-应用插件abi--核心)
4. [写第一个应用（完整例子 + 编译）](#4-写第一个应用完整例子--编译)
5. [安装与调试](#5-安装与调试)
6. [权限系统（含 ROOT 权限）](#6-权限系统含-root-权限)
7. [能调用的驱动与系统能力](#7-能调用的驱动与系统能力)
8. [线程与内存纪律（血泪教训）](#8-线程与内存纪律血泪教训)
9. [字体与中文](#9-字体与中文)
10. [常见问题排查](#10-常见问题排查)
11. [对外 API 速查表](#11-对外-api-速查表)

---

## 1. 平台速览

| 项目 | 值 |
|---|---|
| SoC | 全志 **T113-S3**，双核 **Cortex-A7**（ARMv7，**有 MMU**），带 NEON |
| 系统 | **Tina Linux**，内核 **5.4.61**（已替换为自编译内核：带温度传感器驱动 THS + NVMEM/SID） |
| 屏幕 | **320×240**，**LVGL 8.4**，驱动为 sunxifb（`/dev/fb0`） |
| C 库 | **musl**（不是 glibc！闭源 glibc 二进制跑不了） |
| 内存 | **约 117MB**（很紧张，务必省着用） |
| 用户 | 整个系统都是 **root**（应用也在 root 下运行） |
| 输入 | 波轮编码器 + 3 个按键（菜单/录像/电源），共 4 类事件 |
| 网络 | 只有与热成像模组之间的 USB 网卡（`usb0`，192.168.64.x）。**没有 WiFi/以太网** |
| adb | 相机内置 adbd，序列号 **`20080411`** |

### 存储布局（TF 卡分区，来自相机自己的 `cmdline`）

```
mmcblk0p1  boot-resource  0.5MB
mmcblk0p2  env            252KB
mmcblk0p3  env-redund     252KB
mmcblk0p4  boot           8MB     ← 内核（Android boot 镜像格式，别乱动）
mmcblk0p5  rootfs         125MB   ← 只读系统
mmcblk0p6  private        5MB
mmcblk0p7  rootfs_data    5MB
mmcblk0p8  UDISK          约 29GB  ← ext4，**你的应用和用户数据放这里**
```
- `/mnt/UDISK` —— **可写**，放应用、数据、照片
- `/tmp` —— **tmpfs 约 57MB**，重启即失，适合放日志
- `/` —— 只读 rootfs；要改系统文件得重新烧 rootfs，不建议

---

## 2. 系统结构：桌面是"家"，相机只是应用

```
/mnt/UDISK/thermalcamera.sh   开机脚本：默认启动 lios；lios 起不来则兜底启动相机
/mnt/UDISK/lios               桌面程序（独立进程）= 这套系统的"家"
/mnt/UDISK/LiThermal          相机应用（独立进程）
/mnt/UDISK/apps/*.so          你开发的应用（插件，被 lios dlopen 加载）
/mnt/UDISK/liosdata/          桌面自己的数据（权限记录、设置）
/mnt/UDISK/<你想要的任何东西>
```

桌面（lios）做的事：
- 显示方块网格（**每页 3 列 × 3 行 = 9 个**，满页自动往右加一页，底部有页码）
- 内置 5 个方块：**相机 / 文件管理 / 设置 / 权限管理 / 后台**
- **扫描 `/mnt/UDISK/apps/*.so` 与 `/mnt/UDISK/*.so`**，把发现的应用排在内置方块之后
- 选中应用后：`dlopen` → 建全屏容器 → 调 `lios_app_create()` → 转发按键 → 退出时 `lios_app_destroy()` + `dlclose()`

**桌面按键**：滚轮移动高亮、波轮按下进入、**菜单键=回桌面**、**录像键=翻到下一页**。

---

## 3. 应用（插件）ABI —— 核心

### 3.1 一个应用就是一个 `.so`

放在 **`/mnt/UDISK/apps/`**（也支持直接放 `/mnt/UDISK/` 根目录，`apps/` 优先）。

### 3.2 必须导出的四个符号（C 链接）

```cpp
#include "lios_app.h"

extern "C" const lios_app_info_t lios_app_info = {
    "myapp",        // id：唯一标识（权限记录按 id 存）
    "我的应用",      // 桌面显示名
    "我",           // 图标（1~2 个汉字或符号）
    100,            // 排序（数字小的排前面）
    1               // need_fullscreen：1=独占整屏（游戏用）
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent);  // 建界面，parent 是全屏容器
extern "C" void      lios_app_destroy(void);              // 退出前清理
extern "C" void      lios_app_key(int key, int event);    // 按键事件
```

`lios_app_info_t` 定义（见 `include/lios_app.h`）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `const char*` | 唯一 id，**权限记录的文件名就是它** |
| `name` | `const char*` | 桌面方块上显示的名字（**字体里没有的字会显示成方块**，见第 9 节） |
| `icon` | `const char*` | 方块上的图标文字 |
| `order` | `int` | 排序，越小越靠前（内置应用占 0~4，建议你用 10 以上） |
| `need_fullscreen` | `int` | 1 = 需要独占整屏 |

> 也兼容 `lios_app_info_get()` 函数形式（返回 `const lios_app_info_t*`）。

### 3.3 按键事件（`lios_app_key`）

`key` 取值：

| 常量 | 对应硬件 | 桌面如何转发 |
|---|---|---|
| `LIOS_KEY_LEFT` / `LIOS_KEY_RIGHT` | **滚轮左转 / 右转** | **每转一格发一对「按下+抬起」**，一个 tick 内最多 8 格 |
| `LIOS_KEY_ENTER` | **波轮按下** | 按下/抬起，成对发送（**长按桌面上不会额外发 LONG，要自己按时间判断**） |
| `LIOS_KEY_REC` | **录像键** | 按下+抬起 |
| `LIOS_KEY_BACK` | **菜单键** | 按下+抬起；**注意：桌面收到菜单键后会直接关闭你的页面 = 退出应用** |
| `LIOS_KEY_POWER` | 电源键 | 长按由桌面处理成关机，一般不会转发给你 |
| `LIOS_KEY_UP` / `DOWN` | （无对应硬件，保留） | —— |

`event` 取值：`LIOS_EV_PRESS`（按下）、`LIOS_EV_LONG`（长按，桌面目前不主动发）、`LIOS_EV_RELEASE`（抬起）。

**实践建议**（弹球游戏就是这么做的）：
- 滚轮：收到 `LEFT/RIGHT` 的 **PRESS** 就累加一个计数器，在 `lv_timer` 里消费（避免一帧内丢事件）
- 按下：PRESS 时记时间、RELEASE 时若没超过阈值就算"短按"；要看长按就在 `lv_timer` 里判断"按住超过 N 毫秒"
- 退出：**菜单键由桌面负责关页面**，你只要不崩就行；想"回到自己的菜单"请用 `LIOS_KEY_REC`

### 3.4 生命周期与内存

```
桌面选中你的方块
   → dlopen(RTLD_NOW) → 校验 create/destroy/key 三个符号（缺一个就跳过并记日志）
   → lios_app_create(parent)            // 你在 parent 里建 LVGL 对象
   → 每帧 lios_desktop_tick() 转发按键
   → 退出时 lios_app_destroy() → dlclose()
```
- **LVGL 对象**：你建在 `parent` 里的对象会随容器一起被删，不用自己删（但指针要及时置 NULL，别在 destroy 里访问已删对象）
- **你自己 malloc 的内存**：必须在 `lios_app_destroy()` 里 `free`（内存只有 117MB）
- **`lv_timer`**：自己建的定时器要在 `destroy` 里 `lv_timer_del`，否则退出后还在跑 → 访问已释放内存会崩

### 3.5 加载失败会怎样

**只写 `/tmp/lios.log` 并跳过，桌面绝不崩。** 常见原因：
- 少了 `lios_app_create` / `destroy` / `key` 任一符号 → 直接跳过（**应用会"静默消失"，看不到报错弹窗**）
- 引用了 lios 没导出的 `lv_*` / `ui_font_*` / `lios_*` 符号 → `dlopen` 失败（**CI 有一道 nm 自检会提前拦下**，见第 4.4 节）

自查：`adb shell cat /tmp/lios.log | grep 插件`

---

## 4. 写第一个应用（完整例子 + 编译）

### 4.1 最小例子

```cpp
// myapp.cpp
#include "lios_app.h"
#include "my_main.h"          // ui_font_cn16 / ui_font_cn12
#include <stdio.h>

extern "C" void lios_log(const char *fmt, ...);   // 主程序导出（写 /tmp/lios.log）

static lv_obj_t *g_root = NULL;
static int g_cnt = 0;

extern "C" const lios_app_info_t lios_app_info = {
    "myapp", "我的应用", "我", 100, 1
};

extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent)
{
    g_root = parent;
    g_cnt = 0;
    lv_obj_set_style_bg_color(parent, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_style_text_font(lab, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lab, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(lab, "你好，相机");
    lv_obj_center(lab);

    lios_log("myapp: create");   // 日志写 /tmp/lios.log
    return parent;
}

extern "C" void lios_app_destroy(void)
{
    g_root = NULL;               // LVGL 对象由桌面删
    lios_log("myapp: destroy");
}

extern "C" void lios_app_key(int key, int event)
{
    if (event != LIOS_EV_PRESS) return;
    if (key == LIOS_KEY_ENTER) { g_cnt++; lios_log("myapp: 按下第 %d 次", g_cnt); }
    // 菜单键由桌面负责退出，这里不用管
}
```

### 4.2 CMake（在 `CMakeLists.txt` 里加）

```cmake
add_library(myapp SHARED src/apps/myapp.cpp)
set_target_properties(myapp PROPERTIES PREFIX "" OUTPUT_NAME "myapp")
# 注意：插件**不要链接 lvgl**！符号由主程序 lios 通过 -rdynamic 导出，
# 链接两份 LVGL 会导致状态不一致。
target_include_directories(myapp PRIVATE ${PROJECT_SOURCE_DIR} include libs/lvgl)
```
`build_custom.sh` 会把 `ball.so` / `tetris.so` 打进产物；你新增的插件要照它的样子加一份拷贝（或直接用桌面上的安装器推送，见第 5 节）。

### 4.3 交叉编译器

- 工具链：**`arm-openwrt-linux-muslgnueabi-gcc 6.4.1`**（Tina SDK 自带；CI 里从 `diylxy/LiThermal_Compiler` 取）
- **本机没有交叉编译器**；推荐用仓库的 **GitHub Actions** 编译（`.github/workflows/build.yml`，推代码即自动编译并产出 `UDISK.tar`）

### 4.4 CI 里的符号自检（很重要）

`build_custom.sh` 会用 `nm` 检查：**你的 `.so` 里所有未定义的 `lv_*` / `ui_font_*` / `lios_*` 符号，必须在 `lios` 二进制里有定义**，否则 CI 直接失败并打印缺失清单：

```
!! ball.so 里这些符号在 lios 里找不到（dlopen 会失败）：
```
这条自检就是为了防止"编译过了但装上去静默消失"。**踩过的坑**：`lios_log` 曾经是 C++ 链接（mangled 名 `_Z8lios_logPKcz`），插件写成 `extern "C"` 就对不上 → 现在已在 `lios.h` 里改成 C 链接。

---

## 5. 安装与调试

### 5.1 用桌面上的安装器（推荐）

桌面上的 **`LiThermal-Installer.exe`**（纯 Win32，无需运行库）：
1. 把 `.so` **拖进窗口**（或点「选择文件…」，支持多选）
2. 点 **「安装到相机」** → 自动 `adb push` 到 `/mnt/UDISK/apps/` + `chmod 755` + `sync`
3. 窗口里实时显示相机状态：**CPU 温度 / CPU 占用 / 内存 / 剩余存储 / 已装应用列表**
4. 装完在相机上：**后台 → 重新扫描插件目录**（或重启相机）→ 桌面就会出现你的方块

> 安装器会自动找 `adb.exe`（PATH 或 WinGet 的 platform-tools）。

### 5.2 手工安装（adb）

```bash
adb push myapp.so /mnt/UDISK/apps/
adb shell "chmod 755 /mnt/UDISK/apps/myapp.so; sync"
# 让桌面重新扫描：相机上打开【后台】→「重新扫描插件目录」，或
adb shell reboot
```

### 5.3 调试

| 目的 | 做法 |
|---|---|
| 看桌面日志 | `adb shell cat /tmp/lios.log` |
| 看你自己的日志 | 用 `lios_log(...)`（写 `/tmp/lios.log`），或自己 `fopen("/tmp/myapp.log","a")` |
| 抓文件 | `adb pull /tmp/myapp.log .` |
| 看进程 | `adb shell ps \| grep lios` |
| 看内存 | `adb shell cat /proc/meminfo`、`adb shell cat /proc/self/status` |
| 相机 adb 掉线 | **拔插一次 USB 线**；⚠️ **绝对不要去禁用/启用 Windows 设备管理器里的设备**（会把它弄坏，之前踩过） |

> **日志一律写 `/tmp`**（tmpfs，不伤卡）。别往 `/mnt/UDISK` 高频写日志 —— 慢卡上会拖死 UI。

---

## 6. 权限系统（含 ROOT 权限）

### 6.1 设计定位（重要，请先读这段）

**这台相机的权限是"运行时自检模型"，不是内核级强制沙箱。** 具体含义：

- **应用照常启动**：桌面**不会**因为权限没给就拒绝启动你的应用（这是刻意设计 —— 用户明确要求"哪有应用不给权限就不让启动的"）
- **门控点在运行时**：你的应用在**做某件事之前**，自己调用 `lios_perm_allowed(id, key)` 询问；没权限就不做（并写日志/提示用户）
- 之所以是自检而不是强制：插件是 `dlopen` 进同一个进程的，且系统只有 root、没有 systemd、libc 是 musl；真正的强制需要**进程隔离 + seccomp/能力位**，见 6.5 的路线

### 6.2 五项权限

| key | 名称（设置里显示） | 你应该在什么时候检查 |
|---|---|---|
| `storage` | 读写存储 | 要读写 `/mnt/UDISK` 下的文件时 |
| `camera` | 相机与视频 | 要访问热成像模组（4117）或视频流时 |
| `network` | 网络 | 要用 socket / 与模组 HTTP 通信时 |
| `bg` | 后台运行 | 应用退出后还想继续跑线程/定时任务时 |
| `root` | **ROOT 权限** | 要做高权限操作（挂载、改系统文件、动 `/dev/*`、切 USB 角色、关机等）时 |

**新装的应用，五项权限默认全部关闭。** 用户在【权限管理】里逐项打开。

### 6.3 记录位置与格式

```
/mnt/UDISK/liosdata/perm/<应用id>.conf
```
内容：
```
storage=1
camera=0
network=0
bg=0
root=0
```
（`0` = 关，`1` = 开；文件里有哪项就用哪项，没有的按"关"处理。**注意目录是 `liosdata`，不是 `lios`** —— `lios` 是桌面程序本体。）

### 6.4 查询接口

```cpp
#include "lios_perm.h"        // 或自己声明：extern "C" bool lios_perm_allowed(const char*, const char*);

if (!lios_perm_allowed("myapp", "storage")) {
    lios_log("myapp: 没有存储权限，跳过读文件");
    return;
}
```

行为说明：
- **记录尚未加载完成时返回 `true`（fail-open）** —— 宁可不管，也绝不因为"没数据"把所有应用锁死
- 应用 id 必须与你 `lios_app_info.id` 一致

**示例：启动时把自己各项权限打进日志**

```cpp
static const char *kPerms[5] = {"storage","camera","network","bg","root"};
for (int i = 0; i < 5; i++)
    lios_log("myapp: 权限 %s = %s", kPerms[i],
             lios_perm_allowed("myapp", kPerms[i]) ? "开" : "关");
```

### 6.5 想做"真·SELinux 那种强制"怎么办（诚实路线图）

现状：**做不到内核级强制**，原因与出路：

| 路线 | 需要什么 | 难点 |
|---|---|---|
| **A. 进程隔离 + seccomp** | 让每个插件跑在**独立进程**（不再 dlopen 进桌面），父进程用 `fork` + `prctl(PR_SET_NO_NEW_PRIVS)` + `seccomp` 白名单限制 syscall | 要重写 ABI（改成 IPC）；LVGL 要在子进程里初始化；内存开销变大（117MB 很紧） |
| **B. 能力位（capabilities）** | 内核 `CONFIG_SECURITY` + 用户态 `capset()` 丢弃 `CAP_SYS_ADMIN` 等 | 系统只有一个 root 用户，且插件与桌面同进程 → 必须配合 A |
| **C. SELinux / AppArmor** | 内核打开 `CONFIG_SECURITY_SELINUX`（或 `CONFIG_SECURITY_APPARMOR`）+ 策略加载 | **要重新编译内核并刷 boot 分区**（这台机器已经做过一次内核替换，流程见下方注意事项）；musl 下缺 `libselinux` 等用户态库，要自己交叉编译；没有 systemd，策略加载要自己写 init 脚本 |
| **D. 文件/设备层限制** | 让插件以不同 uid 运行 + `chmod`/`chown` 系统资源 | 要配合 A（同进程就没法区分 uid） |

**当前折中方案（已实现）**：桌面提供 `lios_perm_allowed()` 自检 API + 权限管理界面 + 权限记录持久化；**约定插件自觉遵守**。这是"应用可见、用户可控"的一步，后续要做强制就从路线 A 开始最现实。

> ⚠️ 改内核/刷 boot 分区的注意事项（已经踩过一遍）：
> - boot 分区是 `mmcblk0p4`（偏移 38789120 字节，大小 8388608），是 **Android boot 镜像**（`ANDROID!` 头 + zImage + 12 字节 ramdisk + 约 4MB 原厂尾部数据，**尾部必须原样保留**）
> - **先完整备份原分区**再动手；刷新前用 MD5 校验
> - 分块写入（2MB 一块）比一次推 8MB 可靠；写完必须**读回比 MD5**
> - `thermalcamera.sh` **不在 CI 产物里**（打包脚本从编译器仓库取），每次部署固件后都要重新推一次脚本

---

## 7. 能调用的驱动与系统能力

### 7.1 显示与背光

| 能力 | 路径 / 方式 |
|---|---|
| 帧缓冲 | `/dev/fb0`（LVGL 用 sunxifb 驱动，已在桌面/相机里初始化） |
| 背光（**正确方式**） | `/dev/disp` 的 ioctl：`DISP_LCD_GET_BRIGHTNESS` / `DISP_LCD_SET_BRIGHTNESS` |
| 背光（**不是** sysfs） | `/sys/class/backlight` **不存在**，别指望它 |

背光用法（照抄 `src/Backlight.cpp` 与 `src/lios/page_settings.cpp`）：

```cpp
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>
// sunxi_display2.h 用到这些类型，必须先定义
typedef uint32_t u32; typedef int32_t s32; typedef uint16_t u16; typedef uint8_t u8;
#include <sunxi_display2.h>

static int fd = open("/dev/disp", O_RDWR, 0);
static unsigned long args[4];
// 读：args[0]=0（fb0/lcd0），返回值是 raw，需要用 gamma 表反查成 0..255
args[0] = 0;
int raw = ioctl(fd, DISP_LCD_GET_BRIGHTNESS, args);
// 写：args[0]=0, args[1]=gamma_lut[v]（v 是 0..255）, args[2]=0
args[0] = 0; args[1] = gamma_lut[v]; args[2] = 0;
ioctl(fd, DISP_LCD_SET_BRIGHTNESS, args);
```
（`gamma_lut` 是一张 256 字节的表，直接抄 `src/Backlight.cpp` 里的。）

**撕裂与垂直同步（可选进阶）**：面板是 320×240 @117Hz，framebuffer 只有 1 页（`fbnum=1`，单缓冲）→ 会出现横条撕裂。`FBIO_WAITFORVSYNC` 在本内核是**空实现**。可用的真 vsync 是 `/dev/disp` 的 `DISP_VSYNC_EVENT_EN`(0x0B) + netlink group 1 收 `VSYNC0=<ns>` uevent —— 相机应用里有完整实现（`src/dispVsync.cpp`，`sunxifb_flush()` 里在整屏 memcpy 前等一次帧头）。

### 7.2 输入（波轮编码器）

- 设备：**`/dev/input/event0`**
- 解析示例：`src/lios/main.cpp` 的 `lios_thread_encoder()`（读 `struct input_event`；转动时 `ev.value` 非 0 表示方向，按下时置 pressed 标志）
- **建议**：不用自己读 —— 桌面已经把滚轮/按键翻译成 `lios_app_key()` 事件转发给你了

### 7.3 温度 / 内存 / 负载

| 数据 | 路径 | 说明 |
|---|---|---|
| CPU 温度 | `/sys/class/thermal/thermal_zone0/temp` | **毫摄氏度**（如 `41200` = 41.2°C）；`type` = `cpu_thermal_zone` |
| 内存 | `/proc/meminfo` | `MemTotal:` / `MemAvailable:`（单位 kB） |
| 负载 | `/proc/loadavg` | 1/5/15 分钟 |
| 自己占用 | `/proc/self/status` | `VmRSS:`（kB） |
| CPU 占用 | `/proc/stat` | 取 `cpu ` 那行两次求差 |
| 磁盘 | `df /mnt/UDISK` | busybox df |

### 7.4 USB（**危险，务必留退路**）

| 能力 | 方式 |
|---|---|
| OTG 角色 | `/sys/devices/platform/soc@3000000/soc@3000000:usbc0@0/otg_role`（写 `usb_device` / `usb_host`） |
| 切换 gadget 配置 | `/bin/setusbconfig adb,mtp`（或 `mtp,adb`） |

⚠️ **动 USB 可能把 adb 弄丢**（相机界面里那个隐藏 SH 终端有「恢复 USB」按钮可以救回来：`echo usb_device > otg_role; /bin/setusbconfig mtp,adb`）。任何动 USB 的功能都要：① 界面里能关；② 有回滚路径。

### 7.5 热成像模组（Hikvision 4117）

- 物理：模组通过 **USB NCM（cdc_ncm）** 接在相机的 USB 主机口上 → 相机侧出现网卡 **`usb0`**，IP **192.168.64.32**，模组是 **192.168.64.64**
- 接口：**HTTP ISAPI**，Basic 认证 **`admin` / `Ab123456`**
- 已核实可用的端点：

| 端点 | 内容 |
|---|---|
| `/ISAPI/Thermal/channels/1/thermometry/1/rulesTemperatureInfo?format=json` | **最高/最低/平均温度** + 最高/最低点坐标（`maxTemperature`/`minTemperature`/`averageTemperature`/`MaxTemperaturePoint{positionX,positionY}`）—— 相机应用约 25Hz 轮询它 |
| `/ISAPI/System/status` | 模组自己的 `CPUList.CPU[].cpuUtilization` 与 `MemoryList.Memory[].memoryUsage` |

- 取温度示例（`cameraUtils.cpp` 里有完整可跑版本）：

```cpp
#include "httplib.h"
httplib::Client cli("http://192.168.64.64");
cli.set_basic_auth("admin", "Ab123456");
auto res = cli.Get("/ISAPI/Thermal/channels/1/thermometry/1/rulesTemperatureInfo?format=json");
// 然后用 cJSON 解析 maxTemperature / minTemperature / averageTemperature
```
- ⚠️ **模组生成大响应时会不伺候视频流**：如果频繁请求大图（例如带测温矩阵的 JPEG，约 100KB），会让视频掉帧。要"中心点温度"这种需求，**优先找轻量端点**（未验证：`/ISAPI/Thermal/channels/1/thermometry/rules` 等是否可加"画面中心点测温规则"，如果可行就能省掉大图请求）。
- 模组画在视频流里的 OSD 会被数字变焦裁掉，所以相机应用改成自己画中心温度。

### 7.6 媒体解码（图片/视频）

- 桌面（lios）已链接 **ffmpeg**：`libavcodec / libavformat / libavutil / libswscale`
- 现成示例：`src/lios/viewer.cpp`（图片解码首帧 + 缩放显示、视频顺序解码刷帧、文本查看）
- ⚠️ 解码**必须在 worker 线程**，UI 线程只把准备好的 RGB 缓冲刷上去（见第 8 节）

### 7.7 网络

- 只有 **`usb0`**（与模组的内网 192.168.64.x）。**没有 WiFi、没有以太网**
- 所以 `network` 权限的语义是："允许使用网络栈（与模组通信等）"；不存在"连互联网"这回事（除非以后加 USB 网卡）

### 7.8 音频

**未验证** —— 需要你自己核实：`adb shell ls /dev/snd`、`aplay -l`。

### 7.9 adb / 调试通道

- 相机内置 **adbd**，序列号 **`20080411`**；`adb shell` 就是 root shell
- 掉了通常插拔 USB 线即可恢复

---

## 8. 线程与内存纪律（血泪教训）

### 8.1 两条铁律（违反会整机死锁，只能断电）

1. **LVGL 的事件回调 / 定时器回调里：绝不加锁、绝不做文件 I/O、绝不阻塞**
   原因：LVGL 的事件是在 `lv_timer_handler()` **内部**派发的，而调用方已经持有锁；在回调里再 `LOCKLV()` = **自己锁自己**（曾经因此整机卡死、连长按电源键都没反应）。文件 I/O 在慢卡上会卡住整个 UI。
2. **一切 I/O / 解码 / 网络都放 worker 线程**，UI 线程只取结果、只更新 LVGL。

> 桌面已经把 `lv_mutex` 改成**递归锁**作为兜底，但**别依赖它** —— 纪律照守。

### 8.2 内存

- 只有约 **117MB**：不要开大静态缓冲；`malloc` 后及时 `free`
- 退出应用（`lios_app_destroy`）要彻底释放：自己的内存、自己的 `lv_timer`
- 大数据（图片/视频帧）用双缓冲 + 丢帧策略，别无限堆积

### 8.3 时间

- 一律用 `lv_tick_get()`（毫秒）算**绝对差值**，不要假设定时器一定准时
- 需要"定时"就用 `lv_timer_create(cb, period_ms, NULL)`；周期要固定（**别在运行时改周期去当成"速度控制"** —— 俄罗斯方块就是这么坏的：周期设成 500ms 而回调只累加 20ms → 12.5 秒才落一格）

---

## 9. 字体与中文

可用字体（声明在 `include/lios.h` 与 `include/my_main.h`）：

```cpp
extern "C" const lv_font_t ui_font_cn16;   // 16px
extern "C" const lv_font_t ui_font_cn12;   // 12px
```

用法：
```cpp
lv_obj_set_style_text_font(label, &ui_font_cn16, LV_PART_MAIN | LV_STATE_DEFAULT);
```

**字体是子集化生成的，只包含源码里出现过的汉字。** 新写的汉字如果不在子集里，会显示成**空心方块**。加字流程：

1. 确保你的源码在扫描范围内（`_tools/gen_font.py` 已 glob 扫描 `src/lios/*` 与 `src/apps/*`；新目录要手动加进 `files_scan`）
2. 运行（需要 Node，脚本会调 `npx lv_font_conv`）：
   ```bash
   python _tools/gen_font.py
   ```
3. 校验缺字：
   ```bash
   python _tools/lios_fontcheck.py      # 输出 "cn16 missing: (none)" 才算干净
   ```
4. 重新编译部署（字体编进可执行文件，`lios` 与 `LiThermal` 各有一份）

> 当前字体约 780 个汉字、0 缺字。**如果桌面方块上的名字缺字，就会显示成方块** —— 这就是之前"弹球游戏"被迫叫"彩色块"的原因，现在字体已补齐，名字已改回正确的中文。

---

## 10. 常见问题排查

| 现象 | 先查什么 | 常见原因 |
|---|---|---|
| 装了 `.so`，桌面上看不到 | `adb shell cat /tmp/lios.log \| grep 插件` | ① 少了 `create/destroy/key` 符号；② `dlopen` 失败（引用未导出符号）；③ 没放到 `/mnt/UDISK/apps/`；④ 没让桌面重新扫描 |
| 点方块没反应 / 立刻回桌面 | 日志有没有 `桌面：打开插件应用` | ① `lios_app_create` 里崩了（看 `crash.log`）；② 你在按键回调里把 BACK 当"返回自己菜单"了（**菜单键=退出应用**） |
| 中文显示成方块 | `python _tools/lios_fontcheck.py` | 字体子集里没有这些字 → 重新生成字体 |
| 界面卡死 / 长按电源键无反应 | 回想是否在 LVGL 回调里加了锁或做了 I/O | **回调里绝不做 I/O、绝不加锁**（见第 8 节） |
| 界面白屏 / 花屏 | 是否在 `create` 里改了 `parent` 的尺寸/层级 | `parent` 是全屏容器，别把它删了或改成非全屏 |
| 视频/图片解码时 UI 顿 | 解码是否在 UI 线程 | 解码放 worker 线程，UI 只 blit |
| adb 掉线 | `adb devices` | 插拔 USB 线；**别动 Windows 设备管理器** |
| 权限不生效 | `adb shell cat /mnt/UDISK/liosdata/perm/<id>.conf` | ① 记录没写成功（目录是 `liosdata`）；② 你的 id 与 `lios_app_info.id` 不一致；③ 你根本没调 `lios_perm_allowed`（权限是**自检**模型，不是自动强制） |
| 相机起不来（黑屏/卡 loading kernel） | 屏幕 + `adb devices` | boot 分区被写坏 → 用备份镜像刷回 `mmcblk0p4`（见 6.5 注意事项） |

---

## 11. 对外 API 速查表

### 11.1 主程序（lios）导出的函数（插件可直接调用）

| 函数 | 作用 | 注意 |
|---|---|---|
| `void lios_log(const char *fmt, ...)` | 写日志到 `/tmp/lios.log` | **C 链接**（已在 `lios.h` 修正） |
| `bool lios_perm_allowed(const char *id, const char *key)` | 查询权限 | 未加载完时返回 `true`（fail-open）；key 见 6.2 |
| `int lios_apps_total(void)` | 应用总数（内置+插件） | —— |
| `const lios_app_def_t *lios_apps_get(int i)` | 取第 i 个应用定义 | 越界返回 NULL |
| `bool lios_apps_is_plugin(int i)` | 是否插件 | —— |
| `int lios_apps_revision(void)` | 应用清单版本号 | 变大使桌面重建网格 |
| `bool lios_child_running(void)` | 相机进程是否在跑 | —— |
| `void lios_launch_camera(void)` | 启动相机（fork+exec） | worker 线程里 waitpid |
| `void lios_page_set_hooks(const lios_page_hooks_t *h)` | 内置页面用（列表/表单类） | 插件一般不用 |
| `void lios_desktop_set_wheel_focus(bool on)` | 让滚轮走 LVGL 焦点还是给页面 | 列表类页面用 |

### 11.2 插件 ABI（`include/lios_app.h`）

```cpp
typedef struct {
    const char *id; const char *name; const char *icon;
    int order; int need_fullscreen;
} lios_app_info_t;

enum { LIOS_KEY_NONE=0, LIOS_KEY_ENTER, LIOS_KEY_BACK, LIOS_KEY_REC,
       LIOS_KEY_POWER, LIOS_KEY_UP, LIOS_KEY_DOWN, LIOS_KEY_LEFT, LIOS_KEY_RIGHT };
enum { LIOS_EV_PRESS=0, LIOS_EV_LONG, LIOS_EV_RELEASE };

extern "C" const lios_app_info_t lios_app_info;
extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent);
extern "C" void      lios_app_destroy(void);
extern "C" void      lios_app_key(int key, int event);
```

### 11.3 内置页面的钩子（`lios_page_hooks_t`）

```cpp
typedef struct {
    bool (*key)(int key);       // 返回 true = 这次按键我处理了（桌面不关页面）
    void (*tick)(void);         // 每帧（取 worker 结果、刷新界面）
    void (*destroy)(void);      // 页面关闭前
    void (*wheel)(int dir);     // 可选：滚轮自己收（配合 lios_desktop_set_wheel_focus(false)）
} lios_page_hooks_t;
```
> 这些钩子在 **UI 线程且已持锁**时被调用 → 里面**绝不加锁、绝不 I/O**。
> 参考实现：`src/lios/files.cpp`（文件管理器）、`page_perm.cpp`、`page_tasks.cpp`、`page_settings.cpp`。

---

## 附录 A：参考实现清单（照着抄最快）

| 想做什么 | 看哪个文件 |
|---|---|
| 最小插件（游戏骨架 + 键事件） | `src/apps/tetris.cpp`、`src/apps/ball.cpp` |
| 列表页 + worker 线程读目录 + 删除/重命名 | `src/lios/files.cpp` |
| 图片/视频/文本查看（ffmpeg 解码） | `src/lios/viewer.cpp` |
| 权限记录读写 + 开关 | `src/lios/page_perm.cpp` |
| 读 /proc、释放缓存、插件重扫 | `src/lios/page_tasks.cpp` |
| 背光 ioctl、设置持久化 | `src/lios/page_settings.cpp` |
| 垂直同步（消撕裂） | `src/dispVsync.cpp` + `sunxifb_flush()` |
| 与 4117 模组 HTTP 通信 | `src/cameraUtils.cpp` |
| 滚轮编码器读取 | `src/lios/main.cpp`（`lios_thread_encoder`） |

## 附录 B：部署三件套（LiThermal + lios + thermalcamera.sh）

```powershell
powershell -ExecutionPolicy Bypass -File "LiThermalNet\tools\deploy_lios.ps1"
```
它会：等 CI → 下载产物 → **备份** → 逐个推送（`.new` → 校验 MD5 → 改名）→ 全部成功才重启。
（`LiThermalNet\tools\installer\main.cpp` 是桌面安装器的源码，可用 MinGW 编译：
`g++ -O2 -municode -mwindows -static -std=c++17 -o LiThermal安装器.exe main.cpp -lshell32 -lcomdlg32`）

---

*文档基于真机核实编写。有「未验证」字样的条目请先自己上机确认再依赖。*
