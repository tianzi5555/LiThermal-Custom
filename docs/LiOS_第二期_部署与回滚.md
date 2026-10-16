# LiOS 第二期 部署记录 / 使用说明 / 回滚手册

> 本文对应第二期（真桌面 + 插件框架 + 双向模式切换）。
> 源码仓库 `tianzi5555/LiThermal-Custom`，第二期提交：`b773afc` → `cff714e` → `aa62ac4`。
> 第一期文档见 `docs/LiOS_第一期_部署与回滚.md`。

## 一、第二期做了什么

### 1. 登录界面「三步退出」（防误触绕过密码）
原来按一下菜单键就退出登录进相机，现在改成：
```
菜单键 → 录像键 → 波轮确认键（按下滚轮）
```
* 中间任何一步按错、或两步之间超过 **4 秒** → 作废回待机，底部提示「已取消，重来：菜单>录像>确认」
* 界面底部**常驻**一行小字：`退出登录：菜单 > 录像 > 确认（直接进相机）`
* 密码输入流程完全不变

### 2. 真桌面（替换原「建设中」占位桌面）
`src/lios/lios_launcher.cpp`（原 `lios_desktop.cpp` 已删除）

* **3 列 × 3 行方块网格**：一格一个应用，滚轮移动高亮、按下进入；超过 9 个可上下滚动
* 内置 5 个方块：

  | 方块 | 图标 | 行为 |
  |---|---|---|
  | 相机 | 相 | 切回相机界面 |
  | 文件管理 | 文 | 本期「建设中」占位页（下一期实现） |
  | 设置 | 设 | 同上 |
  | 权限管理 | 权 | 同上 |
  | 后台 | 后 | 同上 |

* 底部提示行：`滚轮选择  按下进入  菜单键回相机`

### 3. 插件框架（扫描 + dlopen）
* 扫描 `/mnt/UDISK/apps/*.so`，`dlopen(RTLD_NOW)` 后读 `lios_app_info`
  （也兼容函数式导出 `lios_app_info_get()`），校验 `lios_app_create` /
  `lios_app_destroy` / `lios_app_key` 三个符号是否齐全
* 按 `order` 排序（内置「相机」order=0 永远第一），与内置方块合并显示
* 选中插件：建全屏容器 → `lios_app_create(container)` → 按键转发 `lios_app_key(key,event)`
  （滚轮上/下、确认、录像、菜单）→ 退出时 `lios_app_destroy()` + `dlclose()`
* **任何失败都不会让桌面崩**：dlopen 失败 / 缺符号 / id 重复 / 超数量上限 →
  写 `/tmp/lios.log` 后跳过该应用，桌面照常显示其它方块
* `/mnt/UDISK/apps` 目录不存在 = 正常情况（只显示内置方块）
* 插件想自己退出（比如它自己画了个返回按钮），调 `lios_app_request_exit()` 即可，
  真正的清理由桌面线程做（避免插件在自己的回调里删自己的对象）
* 主程序已加 `-rdynamic`（`CMakeLists.txt` 里的 `CMAKE_EXE_LINKER_FLAGS`），
  否则插件 `dlopen` 会解析不到 `lv_*` 符号而失败

### 4. 双向模式切换（不用重启程序）
* **LiOS → 相机**：网格里的「相机」方块，或者菜单键
* **相机 → LiOS**：隐藏 SH 终端里的「进LiOS」按钮
  （系统设置 → 手动清除噪声 → 录像键 → 终端右上角第 4 个按钮）
  * 为此把终端顶部那一行改成 4 个 60px 按钮：`USB状态 / 恢复USB / 清除日志 / 进LiOS`
  * **现有操作习惯一个都没动**：终端入口手势、菜单键、相册、隐藏相册全照旧
* 相机线程在 LiOS 期间**待机**（不读图、不刷相机界面），回到相机自动恢复
* `enter_camera()` 幂等：只真正初始化一次

### 5. 字体
`_tools/gen_font.py` 的扫描表已包含 `src/lios/`，另外补了 `· ← ↑ → ↓` 这几个符号
（它们既不在 ASCII 也不在 CJK 区间，原来会漏）。桌面新增的汉字
（台/权/管/限/建/设/中/后…）都已在字体里，`_tools/lios_fontcheck.py` 检查通过。

## 二、按键映射（LiOS 桌面）

| 位置 | 滚轮转 | 滚轮按下 | 菜单键 |
|---|---|---|---|
| 桌面（无应用） | 在方块间移动高亮 | 进入选中的方块 | 回相机 |
| 内置应用（建设中页） | 无 | 无 | 返回桌面 |
| 插件应用 | 转发给插件（`LIOS_KEY_UP/DOWN`） | 转发（`LIOS_KEY_ENTER`） | 转发 `LIOS_KEY_BACK`，然后退出该应用 |

录像键在插件里转发为 `LIOS_KEY_REC`；电源键长按仍是原来的关机逻辑。

## 三、需要验收的几下

1. 开机登录界面：底部应有两行小字（手势提示 + 状态），输 `123456` → 进相机（一切照旧）。
2. 开机输 `5555` → 进 LiOS 桌面：应看到 **3×2 共 5 个方块**（相机/文件管理/设置/权限管理/后台）。
3. 桌面上滚轮转 → 高亮跟着走；选「文件管理」按下 → 显示「建设中」占位页；菜单键 → 回桌面。
4. 选「相机」方块按下（或桌面上按菜单键）→ 回到相机界面，温度/状态条/相册/隐藏终端都正常。
5. 相机界面进隐藏终端（系统设置→手动清除噪声→录像键），按右上角 **「进LiOS」** → 应切回 LiOS 桌面（**不用重启**）。
6. 登录界面按 **菜单→录像→确认** → 应直接进相机；只按一步等 5 秒 → 应提示「已取消，重来：菜单>录像>确认」。
7. 插件链路（要等第三期把 ball/tetris 编成 .so 才能验）：把 .so 放进 `/mnt/UDISK/apps/`，
   重启程序 → 桌面应多出对应方块；日志 `/tmp/lios.log` 里能看到加载成功/跳过原因。

## 四、回滚

### 1. 回到第二期之前的固件（第一期那版）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios2 /mnt/UDISK/LiThermal; sync; reboot -f"
```
* `LiThermal.bak_lios2` = 第二期部署前的固件（= 第一期最终版，MD5 `557d54d1c4c7eeb1b6e82d063cdc44e5`）

### 2. 回到最初的相机固件（没有 LiOS）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios1 /mnt/UDISK/LiThermal; cp -f /mnt/UDISK/thermalcamera.sh.bak_lios1 /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
```
* `LiThermal.bak_lios1` MD5 `8d40a21beb5964aab6e9d4dc504bac63`（最初那版）
* `thermalcamera.sh.bak_lios1` = 最初的启动脚本（直接跑相机、不带 `--login`）

### 3. 只回滚启动脚本
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/thermalcamera.sh.bak_lios2 /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
```

### 4. 临时停用密码界面
```bash
adb -s 20080411 shell "mv /mnt/UDISK/lios.conf /mnt/UDISK/lios.conf.off; sync; reboot -f"
```

### 5. 卡在 LiOS 桌面出不来（极端情况）
* 菜单键 → 回相机；或用「相机」方块
* 都不行就重启相机（长按电源键）

相机上已有的备份一个都没删：`bak_lios1` / `bak_lios2` / `bak_nodoublebuffer` /
`bak_perfprobe` / `bak_probe1`，脚本备份 `thermalcamera.sh.bak_lios1` / `.bak_lios2`。

## 五、部署时踩到的坑（留给以后）

* **`thermalcamera.sh` 不在 CI 产物里**：`build_custom.sh` 打包用的是编译器仓库
  `lxydiy/LiThermal_Compiler` 根目录的那份，而当前 token 对它没有写权限。
  以后每次重新部署固件后都要单独把本仓库根目录的 `thermalcamera.sh` 推到相机，
  否则新固件会以「不带 --login」的方式启动（功能正常，但没有密码界面）。
* 第二期部署时相机的 adb 掉线过（Windows 里设备在、adb 只认到 offline，
  `kill-server`/`start-server`/`reconnect offline` 都救不回来），
  最后是**拔插一次 USB 线**恢复的。部署脚本：
  `LiThermalNet\tools\deploy_lios2_wait.ps1`（会等设备上线再自动部署）。
* 插件的 `.so` 必须能解析到主程序的 LVGL 符号 → 主程序编译时要有 `-rdynamic`。
