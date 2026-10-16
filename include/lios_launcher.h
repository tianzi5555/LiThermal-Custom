#pragma once
// LiOS 第二期：真正的桌面（方块网格 + 插件框架 + 双向模式切换）
//
// 和第一期（同一个程序里的两种顶层模式）的关系：
//   * 第一期：登录界面 +「相机 / LiOS」二选一，进了 LiOS 只能回相机、要再进得重启程序
//   * 第二期：桌面变成方块网格，能从相机切进 LiOS、也能从 LiOS 切回相机（双向、不用重启）
//
// 线程模型（重要）：
//   * `lios_enter()` 只负责**起一个 launcher 线程**，自己马上返回 ——
//     这样 HAL 线程（隐藏终端里按「进 LiOS」）不会被桌面阻塞。
//   * launcher 线程跑 `lios_launcher_screen()`（建桌面 + 等按键），退出后再调
//     `enter_camera()` 把相机线程拉起来，然后线程结束。
//   * 桌面里所有 LVGL 操作都在 LOCKLV()/UNLOCKLV() 里（HAL 线程也会碰 LVGL）。
//   * 插件跑不动/加载失败一律只写 /tmp/lios.log 并跳过，绝不让桌面崩。

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief 切到 LiOS 模式（起 launcher 线程；已经在 LiOS 里则什么都不做）
    void lios_enter(void);

    /// @brief 调试入口：`LiThermal --debug-lios` 直接进桌面（跳过登录），返回是否命中
    bool lios_debug_lios_requested(int argc, char **argv);

    /// @brief 当前是不是 LiOS 桌面在显示（= 相机线程应该待机）
    bool lios_mode_active(void);

    /// @brief main 用：UI 线程（HAL::lv_loop）是不是已经建出来了。
    /// LiOS 启动路径下 UI 线程是 launcher 线程建的，main 不能直接
    /// pthread_join(thread_ui)（有 join 到 0 的风险），要先等这个为真。
    bool lios_camera_ready(void);

    /// @brief 当前有没有应用占着屏幕（有的话滚轮/确认要转发给应用，桌面不吞）
    bool lios_launcher_app_active(void);

    /// @brief 每帧调用（HAL 线程，MODE_LIOS 分支里）：按键/滚轮转发 + 桌面导航
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
