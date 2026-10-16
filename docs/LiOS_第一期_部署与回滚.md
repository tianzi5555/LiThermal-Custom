# LiOS 第一期 部署记录 / 使用说明 / 回滚手册

> 本文对应固件 MD5 `557d54d1c4c7eeb1b6e82d063cdc44e5`（最终版，已部署到相机并验证开机出登录界面）。
> 源码仓库：`tianzi5555/LiThermal-Custom`，提交 `34bd7f8` → `53e48b7` → `18fe7a3` → `91be17f`。
> 早期中间版本 `f74bc4da8f713cc783f48ce1c687507b`（只差底部提示文案）也已在相机上验证过。

## 一、这一期做了什么（和设计稿的差别）

**没有做两个二进制**，按任务要求改成**同一个 LiThermal 程序里的两种顶层模式**：

```
thermalcamera.sh
  → /mnt/UDISK/LiThermal --login
      → LVGL 密码界面（相机初始化之前就显示，不用等 4117 模组 ~15 秒）
          camera 密码（123456） → 继续跑原来的相机（代码路径没动，隐藏相册/隐藏终端照旧）
          lios   密码（5555）   → 切到 LiOS 模式（本期只有一个占位桌面）
          菜单键                → 退出登录，直接进相机（不锁死）
```

不带 `--login` 启动时：整段登录逻辑跳过，行为与改动前**完全一致**（防止影响现有部署）。

## 二、密码配置 `/mnt/UDISK/lios.conf`

```
# LiOS 开机密码配置
camera=123456
lios=5555
```

* 格式：一行一个 `键=值`，`#` 或 `;` 开头是注释，键只认 `camera` / `lios`。
* 改完**不用重新编译**，下次开机生效（也可以直接重启程序）。
* **兜底**：文件不存在 / 读不出 / 全解析失败 → 两个密码都为空 → 直接进相机（绝不锁死用户）。
* 密码最长 16 位；输入框显示成圆点。

## 三、按键说明

| 密码界面 | 操作 |
|---|---|
| 滚轮 | 12 个键之间移动（1-9 / 退回 / 0 / 确定，行优先） |
| 滚轮按下（确认） | 输入当前高亮的键；高亮「确定」时提交密码 |
| 菜单键 | 退出登录 → 直接进相机 |
| 电源键长按 | 正常关机（沿用原逻辑） |

> 输密码的实际动作：滚轮转到数字 → 按下（输入）→ 重复；
> 输完后滚轮移到右下角绿色「确定」→ 按下提交。底部提示行会写这句。
> 密码框显示成圆点，最多 16 位。

| LiOS 占位桌面 | 操作 |
|---|---|
| 滚轮 | 选择方块（本期只有一个方块，第二期换网格） |
| 确认 | 进方块 = 回相机 |
| 菜单键 | 回相机 |

## 四、出问题怎么一键回滚

固件和脚本都有备份，**两条命令，任选**：

### 1. 回滚固件（回到改动前的相机固件）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/LiThermal.bak_lios1 /mnt/UDISK/LiThermal; sync; reboot -f"
```
* `LiThermal.bak_lios1` = 改动前那版固件，MD5 `8d40a21beb5964aab6e9d4dc504bac63`
* 相机上已有的其它备份（`bak_nodoublebuffer` / `bak_perfprobe` / `bak_probe1`）都没动过
### 2. 只回滚启动脚本（保留新固件，但开机不再出密码界面）
```bash
adb -s 20080411 shell "cp -f /mnt/UDISK/thermalcamera.sh.bak_lios1 /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
```
* `thermalcamera.sh.bak_lios1` = 原来的脚本（直接跑相机，不带 `--login`）
* 新固件不带 `--login` 时行为与旧固件一致，所以这条也等于「临时关掉登录界面」

### 3. 彻底停用密码（最轻）
```bash
adb -s 20080411 shell "mv /mnt/UDISK/lios.conf /mnt/UDISK/lios.conf.off; sync; reboot -f"
```
配置文件缺失 → 密码界面直接放行进相机。

### 4. 自动兜底（不用管也会生效）
* 登录启动那一次如果崩了/被杀了 → `thermalcamera.sh` 立刻退化成「直接跑相机」的循环
* 相机模式下持续崩溃 → 原来的 BSOD 重启逻辑，和以前一样

## 五、需要人工确认的点

### 已经自动验证过的（部署时实测）
* 开机脚本确实用 `--login` 启动（`ps` 里能看到 `/mnt/UDISK/LiThermal --login`）
* 密码界面确实被创建出来了（`/tmp/lios.log` 里有「密码界面已显示」，
  这行只有在 LVGL 对象建完、主循环跑起来之后才会写）
* 实例能稳定停在密码界面不退出、不崩溃（连续观察数分钟，`crash.log` 没有新增记录）
* `lios.conf` 能被读到（camera / lios 两个密码都识别为「已设置」）

### 需要你手动按一下确认的（滚轮/按键我这边没法模拟）
1. 开机应看到「LiThermal 开机密码」界面 + 12 键数字键盘。
2. 输入 `123456` 按「确定」→ 进相机，温度/状态条/隐藏相册/隐藏终端都要正常。
3. 再次开机输入 `5555` → 进 LiOS 占位桌面（黑底 + 「LiOS 桌面（建设中）」+ 方块）；
   滚轮选择方块、按确认或菜单键 → 回相机界面。
4. 密码界面直接按菜单键 → 应直接进相机。
5. 故意输错几次（比如 `9999`）→ 只提示「密码错误，请重输」，清空重输，不能崩溃。
6. 电源键长按 → 正常关机提示/关机。

## 六、已知限制 / 给第二期的提醒

* **带 LIOS 密码进来后没有办法再回 LiOS**：因为「回相机」走的是原来的相机初始化，
  重新登录需要重启程序。第二期做桌面时应该把「回桌面」做成切换而不是重启。
* 设了 `lios.conf` 之后，**每次开机都要输密码**（这是设计稿要的行为）。
* 编译系统那层有个坑：`build_custom.sh` 是把**编译器仓库** `lxydiy/LiThermal_Compiler`
  根目录里的 `thermalcamera.sh` 打进 UDISK 产物的，而那个仓库当前 token **没有写权限**
  （`Permission to lxydiy/LiThermal_Compiler.git denied to tianzi5555`）。
  所以：**以后重新部署固件后，要记得把本仓库根目录的 `thermalcamera.sh` 再推到相机**
  （`LiThermalNet/tools/deploy_bg.ps1` 只推 LiThermal 本体）：
  ```powershell
  adb -s 20080411 push .\thermalcamera.sh /mnt/UDISK/thermalcamera.sh.new
  adb -s 20080411 shell "chmod 755 /mnt/UDISK/thermalcamera.sh.new; sync; cp -f /mnt/UDISK/thermalcamera.sh /mnt/UDISK/thermalcamera.sh.bak_lios1; mv -f /mnt/UDISK/thermalcamera.sh.new /mnt/UDISK/thermalcamera.sh; sync; reboot -f"
  ```
  否则新固件会以「不带 --login」的方式启动 —— 功能正常，但开机密码界面不会出来。
* 日志：程序日志在 `/tmp/lios.log`（登录/LiOS 相关）和 `/tmp/perf.log`（性能探针），
  按红线要求**不写 SD 卡**；`/mnt/UDISK` 上只有原有的 `crash.log` / `ui.log` 机制。
