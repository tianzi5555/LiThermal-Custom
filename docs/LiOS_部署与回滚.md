# LiOS 部署 / 使用 / 回滚手册（现行）

> 本手册对应当前架构：**两个平级程序**。
> 相机固件 MD5 `9ad5c292ff5d900cd05472785bbda3af` + 桌面 `lios` MD5 `630f5c6730e53a17f3351dc68939a8a0`
> （第 3 轮第 1 批部署，已在相机上跑通；后续批次会更新这两个值）。

## 一、架构

```
Linux 开机
 └── /mnt/UDISK/thermalcamera.sh
       ├── 有 /mnt/UDISK/lios  → 直接跑 lios（桌面 = 系统的「家」）
       └── lios 缺失 / 启动失败 → 兜底跑原来的相机循环（防黑屏）

lios（独立可执行，src/lios/）
 ├── 方块网格 + 插件 .so 扫描（/mnt/UDISK/apps/*.so）
 ├── 【相机】方块 → fork + exec /mnt/UDISK/LiThermal
 │       · waitpid 在 worker 线程里（绝不在 LVGL 线程等）
 │       · 相机在前台时桌面暂停渲染（不抢 fb0）
 │       · 相机退出后整屏 invalidate + 重绘，回到桌面
 ├── 文件管理器 / 设置 / 权限管理 / 后台（批 2/3/4 逐个换成真页面）
 └── 看门狗：UI 心跳停 3 秒 → 写日志 + _exit(99)；相机在跑时不误判

LiThermal（相机，保持原版）
 └── 唯一改动：隐藏 SH 终端里的「回桌面」按钮 = 保存设置 + 断流 + exit(0)
```

**为什么这么改**：之前把桌面塞进相机进程里当「模式」用，出过两次严重问题
（`pthread_join(0)` 崩溃、LVGL 回调里重复加锁导致整机卡死）。
现在两个程序**进程隔离**：相机崩了不影响桌面，桌面崩了不影响相机，插件 `.so` 只被 `lios` dlopen。

## 二、按键与操作

### 桌面（lios）

| 操作 | 效果 |
|---|---|
| 滚轮转 | 在方块间移动高亮 |
| 滚轮按下 | 进入选中的方块 |
| 菜单键 | 已经在桌面，只给提示（没有上一级） |
| 长按电源键 1.5 秒 | 关机（写 `/tmp/poweroff` + `poweroff`） |

方块顺序：**相机 / 文件管理器 / 设置 / 权限管理 / 后台**（后四个本批是「建设中」占位页）。

### 应用页（占位页或以后的真页面）

| 操作 | 效果 |
|---|---|
| **菜单键 / 录像键 / 波轮按下** | 都回桌面（三条路，怎么按都能出来） |

### 相机（LiThermal，和以前一样）

* 温度 / 状态条 / 隐藏相册 / 隐藏终端 / 最高最低温度 **全部照旧**
* 回桌面：**系统设置 → 手动清除噪声 → 录像键**（进隐藏 SH 终端）→ 右上角 **「回桌面」**

## 三、排查用的日志（都在 /tmp，不写 SD 卡）

| 文件 | 内容 |
|---|---|
| `/tmp/lios.log` | 桌面自身日志：启动、建了多少方块、打开了哪个应用、返回键、相机 fork/exec/waitpid 结果、看门狗 |
| `/tmp/perf.log` | （相机）性能探针 |
| `/mnt/UDISK/ui.log` | （相机）uiTrace 分步日志 |
| `/mnt/UDISK/crash.log` | （相机）崩溃记录 |

`lios` 启动不起来时：`thermalcamera.sh` 会先跑相机（不会黑屏），`/tmp/lios.log` 里能看到停在哪一步。

## 四、回滚

### 回到「只有相机」的老固件（最彻底）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios4 /mnt/UDISK/LiThermal; cp -f /mnt/UDISK/thermalcamera.sh.bak_lios4 /mnt/UDISK/thermalcamera.sh; rm -f /mnt/UDISK/lios; sync; reboot -f"
```
`thermalcamera.sh.bak_lios4` 是最初的启动脚本（直接跑相机），`lios` 删掉后脚本会自动走「只跑相机」分支。

### 只回滚启动脚本（保留两个程序，但开机直接进相机）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/thermalcamera.sh.bak_lios4 /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
```

### 回滚到更早的版本
```bash
# 第一期（双模式 + 密码界面，已废弃，仅应急）
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios2 /mnt/UDISK/LiThermal; sync; reboot -f"
# 最初的相机固件
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios1 /mnt/UDISK/LiThermal; sync; reboot -f"
```

### 忘记/误操作导致桌面起不来
相机里的备份一个都没删：`LiThermal.bak_lios1/2/3/4`、`thermalcamera.sh.bak_lios1/2/4`、
`bak_nodoublebuffer`、`bak_perfprobe`、`bak_probe1`。

## 五、必须守住的纪律（都踩过坑）

1. **LVGL 事件回调 / 定时器回调里绝不加锁、绝不做文件 I/O**
   —— 事件是在 `lv_timer_handler()` 内部派发的，那时锁已经在手里；
   非递归锁会自锁死整机（两个程序现在都用**递归锁**兜底），写文件实测会卡住。
2. **UI 线程（`HAL::lv_loop` / `lios` 的 UI 线程）跑起来之前创建的 LVGL 定时器不会被触发**
   —— 要建定时器就等 UI 线程跑起来之后。
3. **相机 fork/exec 后，`waitpid` 必须在 worker 线程里等**，绝不能在 LVGL 线程里阻塞。
4. **相机在前台时桌面不能画**（两个进程抢 `/dev/fb0` 会花屏）——`lios` 用 `child_running`
   标志直接暂停渲染。
5. **`thermalcamera.sh` 不在编译器的 CI 产物里**（编译器仓库无写权限），
   但本仓库的 `build_custom.sh` 现在会优先用本仓库根目录那份，并把 `build/lios` 一起打进 UDISK。

## 六、构建产物（CI 打的 UDISK.tar 里应该有 4 个文件）

```
LiThermal        相机（原版 + 「回桌面」按钮）
lios             桌面（独立程序）
BSOD             蓝屏提示
thermalcamera.sh 启动脚本（开机先进桌面）
```
如果 `build/lios` 不存在，`build_custom.sh` 会直接报错退出（不会悄悄漏打）。
