#pragma once
// LiOS 桌面：方块网格 + 插件框架（dlopen）+ 相机双向切换
//
// 入口：相机 → 隐藏 SH 终端（系统设置 → 手动清除噪声 → 录像键）→ 右上角「进LiOS」
// 退出：桌面按菜单键 / 选「相机」方块 → 回相机；应用页按 菜单/录像/确认 → 回桌面
//
// 线程模型（重要）：
//   * `lios_enter()` 只负责**起一个 launcher 线程**，自己马上返回 ——
//     这样 HAL 线程（隐藏终端里按「进LiOS」）不会被桌面阻塞。
//   * launcher 线程跑 `lios_launcher_screen()`（建桌面 + 等按键），退出后再调
//     `enter_camera()` 把相机线程拉起来，然后线程结束。
//   * 桌面里所有 LVGL 操作都在 LOCKLV()/UNLOCKLV() 里（HAL 线程也会碰 LVGL）。
//   * 插件跑不动/加载失败一律只写 /tmp/lios.log 并跳过，绝不让桌面崩。
//
// 两个必须守住的纪律（都踩过坑）：
//   1) LVGL 事件回调 / LVGL 定时器回调里**绝不 LOCKLV、绝不做文件 I/O**
//      （事件是在 lv_timer_handler 内部派发的，那时锁已在手里；写文件实测会卡住）
//   2) UI 线程（HAL::lv_loop）跑起来之前创建的 LVGL 定时器不会被触发

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief LiOS 日志（固定写 /tmp/lios.log，不写 SD 卡）
    void lios_log(const char *fmt, ...);

    /// @brief 切到 LiOS 模式（起 launcher 线程；已经在 LiOS 里则什么都不做）
    void lios_enter(void);

    /// @brief 启动自动测试（只有 /tmp/lios_autotest 存在时才真的跑；调试钩子）
    void lios_autotest_start(void);

    /// @brief 启动看门狗：UI 心跳停 3 秒 → 写日志 + _exit(99)，让脚本重启相机
    void lios_watchdog_start(void);

    /// @brief UI 线程每转一圈调一次（HAL::lv_loop 里），给看门狗喂心跳。
    /// 不用 LVGL 定时器做心跳是故意的：实测在 UI 线程起来之前建的 LVGL 定时器不会触发，
    /// 纯计数器没有任何这种坑。
    void lios_ui_beat(void);

    /// @brief 当前是不是 LiOS 桌面在显示（= 相机线程应该待机）
    bool lios_mode_active(void);

    /// @brief main 用：UI 线程（HAL::lv_loop）是不是已经建出来了（避免 pthread_join(0)）
    bool lios_camera_ready(void);

    /// @brief 当前有没有应用占着屏幕（有的话滚轮/按键要转发给应用，桌面不吞）
    bool lios_launcher_app_active(void);

    /// @brief 每帧调用（HAL 线程，MODE_LIOS 分支里）：按键/滚轮转发 + 返回上一级
    void lios_launcher_poll(void);

    /// @brief 应用想自己退出（插件调用）：只置个标记，真正的退出由桌面线程做，
    /// 这样插件不会在自己的事件回调里删自己的对象
    void lios_app_request_exit(void);

    /// @brief 桌面本体（launcher 线程里跑）：建网格 → 等按键 → 可能进应用 → 退出时清理
    void lios_launcher_screen(void);

    /// @brief 启动相机（幂等：只会真正初始化一次；LiOS 模式下先不动）
    void enter_camera(void);

#ifdef __cplusplus
}
#endif
