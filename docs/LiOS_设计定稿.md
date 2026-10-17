# LiOS 设计定稿（相机的"系统 + 应用平台"）

目标：在 LiThermal 相机上做一套可扩展的应用平台，**以后加应用不用改固件**。

## 一、开机流程（⚠️ 已废弃：密码界面已删除）

> **这一节已经不做了。** 用户后来决定：相机里既然已经有「隐藏 SH 终端 → 进LiOS」这个入口，
> 就不需要开机密码界面这一层。现在**开机直接进相机**（与最早的部署完全一致），
> LiOS 桌面的唯一入口是隐藏 SH 终端里的「进LiOS」按钮。
> 相关代码（`lios_login.cpp` / `lios_config.c` / `lios_user.h` / `MODE_LOGIN` / `--login`）
> 与相机上的 `/mnt/UDISK/lios.conf` **都已删除**。

```
thermalcamera.sh（现在直接跑 LiThermal，行为与最早一致）
  → /mnt/UDISK/LiThermal          # 开机直接进相机（含隐藏相册/隐藏终端）
      隐藏终端（系统设置→手动清除噪声→录像键）→ 右上角「进LiOS」
      → LiOS 桌面；桌面按菜单键 / 选「相机」方块 → 回相机
```

（下面这段是第一期的历史设计，保留仅作参考）
- ~~密码从 `/mnt/UDISK/lios.conf` 读；文件缺失/读不出 → 默认直接进相机~~（已删除）
- ~~login 崩溃/超时 → thermalcamera.sh 的循环会退化成直接跑相机~~（已删除）

## 二、应用平台（插件 ABI）

接口已定稿：`include/lios_app.h`
每个应用 = 一个 `.so`，放在 `/mnt/UDISK/apps/`，导出：
```c
extern "C" const lios_app_info_t lios_app_info;   // id / name / icon / order
extern "C" lv_obj_t *lios_app_create(lv_obj_t *parent);
extern "C" void      lios_app_destroy(void);
extern "C" void      lios_app_key(int key, int event);
```
桌面（launcher）负责：扫描 `apps/*.so` → dlopen 读 info → 显示方块图标 → 选中时创建全屏容器调 `lios_app_create` → 转发按键（滚轮/确认/菜单/录像/电源）→ 退出时 `lios_app_destroy` + dlclose（释放内存）。

现有可用的插件源码：`src/apps/ball.cpp`（弹球，1042 行）、`src/apps/tetris.cpp`（俄罗斯方块，923 行）。
⚠️ 它们导出名里有 `lios_app_loop`（多余的），接进 launcher 时统一去掉。
⚠️ 字体 `ui_font_cn16` 缺"得/分/等/级/弹/球/游/戏"等字 → 接入前把 `src/apps/` 加进 `_tools/gen_font.py` 的扫描表重新生成字体。

## 三、桌面要有的东西（分期）

1. 方块网格桌面（选中高亮，滚轮移动，确认进入）+ 右上角状态条沿用现有的
2. **文件管理器**（浏览 /mnt/UDISK，可参考 `ESP-S3/UI_SDManager.h` 的思路）
3. **设置**（仿安卓风格：显示/温度/按键/关于）
4. **权限管理器**（每个应用一份权限记录，存 `/mnt/UDISK/lios/perm/<id>.json`，桌面留入口）
5. **后台管理**（列出运行中的应用，一键结束释放内存）

## 四、构建

`CMakeLists.txt` 现在已把 `src/apps/` 排除出主程序 ✓。
接入时再加：
```cmake
add_executable(lios src/lios/*.cpp)          # 桌面本体
add_library(ball SHARED src/apps/ball.cpp)   # 插件
add_library(tetris SHARED src/apps/tetris.cpp)
set_target_properties(ball tetris PROPERTIES PREFIX "" OUTPUT_NAME "...")
```
插件要能解析到主程序里的 LVGL 符号 → 主程序链接时加 `-rdynamic`（`set_target_properties(lios PROPERTIES ENABLE_EXPORTS ON)` 或 `target_link_options(lios PRIVATE -rdynamic)`）。

## 五、当前已完成（与本项目相关的基础）

- 相机内核已换成带温度传感器的版本（`/sys/class/thermal/thermal_zone0/temp` 可读）✓
- 应用侧：主循环 HTTP 已搬到独立线程（23.8Hz）✓、`/dev/disp` vsync 帧头对齐消撕裂 ✓
- 状态条：右上角，CPU 温度 + 占用，`LV_OPA_50`，菜单/终端打开时隐藏 ✓
- 隐藏 SH 终端（系统设置→手动清除噪声→录像键）✓ 含「USB 状态/恢复 USB/清除日志」✓
- 隐藏相册偶发崩溃已修（HAL 线程建 LVGL 对象必须加锁）✓

## 六、下一步（按顺序）

1. ~~`lios-login`（密码界面）+ 改 `thermalcamera.sh`~~ → **已废弃**（用户决定不要密码界面）
2. ✅ 桌面骨架：扫描 `apps/` + 方块网格 + dlopen + 按键转发
3. 把 ball/tetris 编成 .so 并验证安装/退出/内存回收
4. 文件管理器 / 设置 / 权限 / 后台 逐个补

## 七、给未来会话的提醒
- 仓库：`C:\Users\Administrator\Desktop\Deepseek Harness EAC\LiThermal_clean`（公开仓库 `tianzi5555/LiThermal-Custom`）
- **git push 必须绕开坏代理**：`git -c http.proxy= -c https.proxy= push origin HEAD`
- 部署：`LiThermalNet\tools\deploy_bg.ps1`（自动等 CI → 下载 → 推 `.new` → 校验 MD5 → 改名 → 重启）
- 相机 adb 序列号 `20080411`；adb 掉线只 kill/start-server，**绝不动 Windows 设备管理器**；
  `kill-server`/`start-server`/`reconnect offline` 都救不回来时让用户**拔插一次 USB 线**
- 磁盘"卡住/No Media"先试 `diskpart` 的 `rescan`（一条命令救回来）

## 八、当前实现（第三批之后）

* **没有两个二进制**：桌面是同一个 `LiThermal` 程序里的一个模式，
  入口 = 相机里的隐藏 SH 终端「进LiOS」按钮
* **开机直接进相机**（密码界面、`--login`、`lios.conf` 全部删除，不留死代码）
* 代码：`src/lios/lios_launcher.cpp`（桌面 + 插件框架 + 双向切换）、
  `include/lios_launcher.h`、`include/lios_app.h`（插件 ABI）
* **返回规则**：应用页按 菜单/录像/确认 都回桌面；桌面按菜单键回相机
* 看门狗：UI 心跳停 3 秒 → 写日志 + `_exit(99)`，让脚本重启相机（不留死屏）
* 使用说明 / 回滚手册 / 踩坑清单见 **`docs/LiOS_第二期_部署与回滚.md`**

### 必须守住的两条纪律（都踩过坑）
1. **LVGL 事件回调 / 定时器回调里绝不 `LOCKLV()`、绝不做文件 I/O**
   —— 事件是在 `lv_timer_handler()` 内部派发的，那时锁已经在手里；
   非递归锁会自锁死整机（`lv_mutex` 现已改递归锁兜底），写文件实测会卡住
2. **UI 线程（`HAL::lv_loop`）跑起来之前创建的 LVGL 定时器不会被触发**

### 部署注意
`build_custom.sh` 打包用的 `thermalcamera.sh` 来自编译器仓库 `lxydiy/LiThermal_Compiler`，
当前 token 对它**没有写权限** → 每次重新部署固件后要把本仓库根目录的
`thermalcamera.sh` 单独推回相机（部署脚本只推 LiThermal 本体）。

