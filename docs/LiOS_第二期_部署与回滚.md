# LiOS 第二期 部署记录 / 使用说明 / 回滚手册

> 第二期（真桌面 + 插件框架 + 双向模式切换）+ 两个实测 bug 的修复。
> 源码仓库 `tianzi5555/LiThermal-Custom`。
> 第一期文档见 `docs/LiOS_第一期_部署与回滚.md`。

## 〇、登录界面行为（用户最新要求）

* **只有输对密码才能离开登录界面**：
  * `camera` 密码（默认 `123456`）→ 进相机
  * `lios` 密码（默认 `5555`）→ 进 LiOS 桌面
  * 其它输入 → 「密码错误，请重输」，**绝不放行进相机**
* **菜单键 / 录像键在登录界面不做任何事**（按了只把提示行刷回「请输入密码」）
  * 原来的「菜单 → 录像 → 确认」三步退出登录手势**已按要求彻底删除**，底部提示小字也删了
* 唯一的豁免通道（防锁死兜底）：`/mnt/UDISK/lios.conf` **缺失/解析失败 → 直接进相机**
* 密码长度上限 16 位；输入框显示成圆点；连错 6 次多一句提示

## 一、两个实测 bug 的根因与修法

### Bug 1：「输 5555 也进相机」
真因**不在密码比对**，而在登录之后的模式切换链路崩了：

* `src/main.cpp`：LiOS 启动路径下，UI 线程（`HAL::lv_loop`）是 launcher 线程在
  `enter_camera()` 里创建的，而 main 在 `lios_enter()` 之后**立刻**
  `pthread_join(thread_ui)` —— 此时 `thread_ui` 还是 0（无效线程号）→
  musl 里解引用空指针 → **SIGSEGV，fault addr 0x1c**（crash.log 里的记录）
* 进程被 `thermalcamera.sh` 的降级循环拉起来时是**不带 `--login`** 的 → 直接进相机界面
  → 用户看到的现象就是「输 5555 也进相机」

修法：
* `src/main.cpp`：改成等 `lios_camera_ready()` 为真、且 `thread_ui != 0` 才 join
* `enter_camera()`：`pthread_create` 成功就立刻置位 `g_camera_ready` 并记日志

**设备上的证据**（`LiThermal --selftest-pw` 只读自检，不打印明文）：
```
配置 /mnt/UDISK/lios.conf: camera密码 len=6 sum=2565028831 / lios密码 len=4 sum=120065352
密码判定: camera（输入 len=6 sum=2565028831）
密码判定: lios（输入 len=4 sum=120065352）      ← 5555 判定为 lios
密码判定: reject（输入 len=4 sum=276376848）
```
运行期日志也证明 5555 走的是 lios 分支：
```
点确定：输入 len=4
密码判定: lios（输入 len=4 sum=120065352）
登录分流确定: lios -> LiOS 桌面
```

### Bug 2（严重）：「进 LiOS 后点任何方块整机卡死」
根因：**LVGL 事件回调里重复获取非递归互斥锁 → 自锁死**

* `src/lios/lios_launcher.cpp` 的 `tile_click_cb`（旧代码）里有两处 `LOCKLV()`
* 而 LVGL 的输入事件是在 `lv_timer_handler()` **内部**派发的
  （`lv_indev_read_timer_cb` → `indev_encoder_proc` → `lv_event_send(..., LV_EVENT_CLICKED)`），
  `lv_timer_handler()` 又是在 `HAL::lv_loop()` 的 `LOCKLV()` 里调的
* 也就是说：**事件回调执行时本线程已经持有 `lv_mutex`**；
  `lv_mutex` 是 `pthread_mutex_init(&lv_mutex, NULL)` 建的**非递归**锁 →
  回调里再 `LOCKLV()` 立刻自锁死：
  * UI 线程永久阻塞 → 屏幕不刷新、按键不处理
  * HAL 线程随后在 `refresh_poweroff_key → lv_ui_poweroff` 也要拿这把锁 → 一起卡死
  * → **长按电源键无反应，只能断电**（与实测现象完全一致）

修法（双保险）：
1. `src/lios/lios_launcher.cpp`：把「进应用 / 退应用」拆成两层 ——
   `tile_activate_locked()` / `app_exit_locked()`（调用者须已持锁）
   和 `tile_activate()` / `app_exit()`（自己加锁）。
   **LVGL 事件回调、LVGL 定时器回调一律只调 `*_locked`，绝不再 `LOCKLV`。**
2. `src/main.cpp`：`lv_mutex` 改成 **递归锁**（`PTHREAD_MUTEX_RECURSIVE`）——
   以后即使再有人在回调里误写 `LOCKLV()`，最多多记一次引用计数，也不会锁死整机。

顺带加固：
* 应用占屏期间 `lv_indev_enable(NULL, false)` 关掉输入，防止滚轮把 group 焦点改到桌面方块
* 删应用容器前先把焦点挪回还活着的方块，避免 group 里留野焦点
* 新增**看门狗**：UI 心跳（`HAL::lv_loop` 每圈的计数器）停 3 秒 →
  写 `/tmp/lios.log` + `_exit(99)`，让 `thermalcamera.sh` 的降级循环重启相机
  （宁可重启也不留死屏 / 电源键无反应）

**设备上的自动测试证据**（`--debug-lios` + `/tmp/lios_autotest`，无人值守自己点方块）：
```
autotest: 已启动（3 秒后自动打开第一个方块，6 秒后退出应用）
看门狗: ui_beat=575  step=0 tile_open=0 app_exit=0
看门狗: ui_beat=802  step=1 tile_open=1 app_exit=0    ← 打开方块成功
看门狗: ui_beat=1254 step=2 tile_open=1 app_exit=1    ← 退出应用成功
看门狗: ui_beat=1478 step=3 tile_open=1 app_exit=1
看门狗: ui_beat=1923 step=3 tile_open=1 app_exit=1    ← UI 心跳一直在涨 = 没死锁
```
`ui_beat` 每次采样都在大幅增长，说明整个「进方块 → 退出」过程中 UI 线程始终在跑；
进程没崩、看门狗没触发。

## 二、桌面怎么用（按键映射）

| 位置 | 滚轮转 | 滚轮按下 | 菜单键 |
|---|---|---|---|
| 桌面（无应用） | 在方块间移动高亮 | 进入选中的方块 | 回相机 |
| 内置应用（「建设中」占位页） | 无 | 无 | 返回桌面 |
| 插件应用 | 转发 `LIOS_KEY_UP/DOWN` | 转发 `LIOS_KEY_ENTER` | 转发 `LIOS_KEY_BACK` 后退出 |

* 网格 3 列，内置 5 个方块：**相机 / 文件管理 / 设置 / 权限管理 / 后台**
* **LiOS → 相机**：「相机」方块 或 菜单键
* **相机 → LiOS**：系统设置 → 手动清除噪声 → 录像键（进隐藏 SH 终端）→ 右上角 **「进LiOS」**
* 插件：`/mnt/UDISK/apps/*.so`，失败/缺符号只写 `/tmp/lios.log` 并跳过，桌面不崩

## 三、部署与回滚

### 回滚命令
```bash
# 回本轮修复之前的版本
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios3 /mnt/UDISK/LiThermal; sync; reboot -f"
# 回第一期（有登录界面、无真桌面）
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios2 /mnt/UDISK/LiThermal; sync; reboot -f"
# 回最初的纯相机固件
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios1 /mnt/UDISK/LiThermal; cp -f /mnt/UDISK/thermalcamera.sh.bak_lios1 /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
# 忘记密码时的救援：把配置移走，开机直接进相机
adb -s 20080411 shell "mv /mnt/UDISK/lios.conf /mnt/UDISK/lios.conf.off; sync; reboot -f"
```

## 四、排查用的自检入口（默认不影响正常使用）

| 入口 | 用法 | 作用 |
|---|---|---|
| `--selftest-pw` | `adb shell /mnt/UDISK/LiThermal --selftest-pw` | 只读自检：把 lios.conf 解析结果和三条密码判定写进 `/tmp/lios.log`（只输出长度/校验和，不显示界面、不改文件） |
| `--debug-lios` | 手动启动时加 | 跳过登录直接进桌面（调试用） |
| `/tmp/lios_autotest` | 建了这个空文件再启动 | 自动测试：3 秒后自动打开第一个方块、6 秒后退出应用；配合看门狗每 2 秒报进度（默认关闭，不建文件就不跑） |

日志都在 `/tmp`（`lios.log` / `perf.log`），按红线要求**不写 SD 卡**。

## 五、踩过的坑（给以后的会话）

1. **LVGL 事件/定时器回调里绝对不要 `LOCKLV()`** —— 回调是在 `lv_timer_handler()` 内部
   被派发的，那时锁已经在手里；非递归锁会自锁死整机。现在锁已改成递归锁兜底，但仍应遵守。
2. **不要在定时器回调里做文件 I/O**（`fopen/fprintf`）—— 实测写不出来，会让「日志死了」
   看起来像「线程死了」。要报进度就用普通线程（比如看门狗）去打日志。
3. **UI 线程（`HAL::lv_loop`）跑起来之前创建的 LVGL 定时器不会被触发** ——
   要建定时器就等 UI 线程起来之后（本项目现在是在 HAL 线程的桌面轮询里建）。
4. `main` 不要在 LiOS 启动路径下直接 `pthread_join(thread_ui)`：那是 launcher 线程建的，
   可能还是 0。等 `lios_camera_ready()`。
5. `thermalcamera.sh` 不在 CI 产物里（编译器仓库无写权限），每次部署固件后要单独推脚本。
6. adb 偶发掉线（`offline`）：`kill-server`/`start-server`/`reconnect offline` 都救不回来时，
   让用户**拔插一次 USB 线**即可；`LiThermalNet\tools\deploy_lios2_fast.ps1` 会等设备上线自动部署。
