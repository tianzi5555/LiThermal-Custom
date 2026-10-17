# LiOS 第一期（已废弃）

> ⚠️ **本文档对应的功能已经被删除，仅作历史记录保留。**

第一期做的是「开机密码界面 + 相机/LiOS 二选一」，具体是：

* `thermalcamera.sh` 用 `LiThermal --login` 启动 → 先出 LVGL 密码界面
* `camera=123456` → 进相机；`lios=5555` → 进 LiOS 模式
* 密码从 `/mnt/UDISK/lios.conf` 读，文件缺失/解析失败 → 直接进相机

**为什么废弃（用户决定）**：相机里已经有「隐藏 SH 终端 → 右上角『进LiOS』」这个入口，
开机密码界面属于多余的一层；而且它本身还带来过一条崩溃链路
（`main` 在 LiOS 启动路径下 `pthread_join(thread_ui=0)` → SIGSEGV → 脚本降级重启
→ 用户看到的现象是「输 5555 也进相机」）。

**现在（第三批之后）的行为**：
* 开机**直接进相机**，与最早的部署完全一致
* LiOS 桌面的唯一入口 = 隐藏 SH 终端（系统设置 → 手动清除噪声 → 录像键）→ 右上角「进LiOS」
* 桌面按菜单键 / 选「相机」方块 → 回相机
* 相机上的 `/mnt/UDISK/lios.conf` 已删除；相关代码 `lios_login.cpp` / `lios_config.c` /
  `lios_user.h` / `MODE_LOGIN` / `--login` 全部删除，不留死代码

👉 现行说明、按键映射、回滚手册一律看 **[`LiOS_第二期_部署与回滚.md`](LiOS_第二期_部署与回滚.md)**。
