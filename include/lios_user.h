#pragma once
// LiOS 第一期：开机密码 + 两种顶层模式（相机 / LiOS 桌面）
//
// 架构：**不是两个二进制**，而是同一个 LiThermal 程序里的两种顶层模式。
//   thermalcamera.sh 用 `LiThermal --login` 启动
//     → 先出密码界面（LVGL）→ 密码正确后：
//         camera 密码（默认 123456）→ 继续跑现在的相机（代码路径完全不变）
//         lios   密码（默认 5555）  → 切到 LiOS 模式
//   不带 --login 启动 → 完全不进登录流程，行为与以前一模一样（绝不破坏现有部署）
//
// 安全兜底（硬要求）：
//   * /mnt/UDISK/lios.conf 缺失/读不出/解析失败 → 直接进相机，绝不锁死用户
//   * 密码界面退出登录要「菜单键 → 录像键 → 波轮确认键」三步（第二期加，防误触）
//   * 脚本那层还有超时兜底（thermalcamera.sh 里）
//
// 第二期的桌面本体在 src/lios/lios_launcher.cpp（声明见 include/lios_launcher.h）。

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

// 密码配置文件（自己定义的格式，键=值，一行一个，支持 # 注释）：
//     camera=123456
//     lios=5555
#define LIOS_CONF_PATH "/mnt/UDISK/lios.conf"

    /// @brief LiOS 自己的日志（固定写 /tmp/lios.log，不写 SD 卡），实现见 lios_config.c
    void lios_log(const char *fmt, ...);

    typedef enum
    {
        LIOS_ENTER_CAMERA = 0, // 进相机（默认/兜底/菜单键退出登录）
        LIOS_ENTER_LIOS = 1,   // 进 LiOS 模式
    } lios_mode_t;

    /// @brief 读取 /mnt/UDISK/lios.conf 里的两个密码
    /// 文件缺失或解析失败 → 两个密码都为空串（此时 lios_login_check 一律返回 0，
    /// 密码界面会直接进相机；正常的密码界面也会一按就退出，绝不会锁死）
    void lios_config_load(void);

    /// @brief 判定输入的密码属于哪种模式
    /// @return LIOS_ENTER_CAMERA / LIOS_ENTER_LIOS；都不匹配返回 -1
    int lios_login_check(const char *input);

    /// @brief 该模式（相机/桌面）的密码是否可用（非空）
    bool lios_login_has_password(int mode);

    /// @brief 是否带 --login 启动（lios_user_prepare 里初始化）
    bool lios_login_requested(void);

    /// @brief 解析命令行参数，决定这次启动要不要先走登录界面
    /// @return true = 需要走登录流程
    bool lios_user_prepare(int argc, char **argv);

    /// @brief 跑一次登录流程（自己跑 lv_timer_handler，此时 UI 线程还没起来）
    /// @return LIOS_ENTER_CAMERA（相机密码 / 三步退出 / 任何异常兜底）或 LIOS_ENTER_LIOS
    lios_mode_t lios_login_show_and_run(void);

#ifdef __cplusplus
}
#endif
