# LiOS 设计定稿（相机的"系统 + 应用平台"）

目标：在 LiThermal 相机上做一套可扩展的应用平台，**以后加应用不用改固件**。

## 一、开机流程（两套密码）

```
thermalcamera.sh（现在直接跑 LiThermal）
  → 改为先跑一个小程序 lios-login（LVGL 密码界面，复用 src/ui/keyboard.cpp 的键盘）
      密码 123456  → exec /mnt/UDISK/LiThermal   （现在的相机界面，功能完全不变，含隐藏相册）
      密码 5555    → exec /mnt/UDISK/lios         （新的桌面系统）
```
安全兜底：
- 密码从 `/mnt/UDISK/lios.conf` 读；**文件缺失/读不出 → 默认直接进相机**（绝不锁死用户）
- login 崩溃/超时 → thermalcamera.sh 的循环会退化成直接跑相机

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

1. `lios-login`（密码界面）+ 改 `thermalcamera.sh`
2. `lios` 桌面骨架：扫描 `apps/` + 方块网格 + dlopen + 按键转发
3. 把 ball/tetris 编成 .so 并验证安装/退出/内存回收
4. 文件管理器 / 设置 / 权限 / 后台 逐个补

## 七、给未来会话的提醒
- 仓库：`C:\Users\Administrator\Desktop\Deepseek Harness EAC\LiThermal_clean`（公开仓库 `tianzi5555/LiThermal-Custom`）
- **git push 必须绕开坏代理**：`git -c http.proxy= -c https.proxy= push origin HEAD`
- 部署：`LiThermalNet\tools\deploy_bg.ps1`（自动等 CI → 下载 → 推 `.new` → 校验 MD5 → 改名 → 重启）
- 相机 adb 序列号 `20080411`；adb 掉线只 kill/start-server，**绝不动 Windows 设备管理器**
- 磁盘"卡住/No Media"先试 `diskpart` 的 `rescan`（一条命令救回来）

## 八、第一期实际实现（与上面设计稿的差别）

**没有做两个二进制。** 改成同一个 `LiThermal` 程序里的两种顶层模式：

- `thermalcamera.sh` → `LiThermal --login` → 密码界面（LVGL）→
  `camera` 密码跑原相机 / `lios` 密码进 LiOS 模式（本期占位桌面）/ 菜单键退出登录直接进相机
- 不带 `--login` 启动 = 行为与改动前完全一致
- 代码：`src/lios/lios_config.c`（读 `/mnt/UDISK/lios.conf`）、`lios_login.cpp`（密码界面）、
  `lios_desktop.cpp`（占位桌面）；入口在 `src/main.cpp`；声明在 `include/lios_user.h`
- 细节、使用说明、**回滚手册**见 `docs/LiOS_第一期_部署与回滚.md`

第二期接桌面时要注意的两点：
1. 现在「从 LiOS 回相机」走的是原相机初始化 + 重启线程，不是模式切换；
   第二期应该做成真正的模式切换（否则没法回到桌面）。
2. `build_custom.sh` 打包用的 `thermalcamera.sh` 来自编译器仓库 `lxydiy/LiThermal_Compiler`，
   当前 token 对它**没有写权限** → 每次重新部署固件后要把本仓库根目录的
   `thermalcamera.sh` 单独推回相机（部署脚本只推 LiThermal 本体）。

