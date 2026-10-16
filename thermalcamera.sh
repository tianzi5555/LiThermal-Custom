#!/bin/sh
# LiThermal 启动脚本
#
# LiOS 第一期：带 --login 启动 → 程序先出「开机密码」界面（LVGL）
#   相机密码 → 继续跑现在的相机（功能完全不变）
#   LiOS 密码 → 切到 LiOS 模式（本期的占位桌面）
#
# 兜底（绝不锁死用户）：
#   1) 密码配置文件缺失 → 程序内部直接进相机
#   2) 登录界面按菜单键 → 直接进相机
#   3) 只尝试一次带 --login；若它崩了/被杀了 → 立刻退化成直接跑相机
#   4) 相机模式下崩溃 → 走原来的 BSOD 重启逻辑
# 任何情况下都不动内核/分区/bootargs。

if [ -f /mnt/UDISK/LiThermal ]; then
	chmod +x /mnt/UDISK/LiThermal
	chmod +x /mnt/UDISK/BSOD

	# ---- 第一步：只跑一次带登录界面的启动 ----
	/mnt/UDISK/LiThermal --login
	rc=$?
	if [ $rc -eq 0 ]; then
		echo "[Process exited normally]"
		exit 0
	fi
	if [ -f /tmp/poweroff ]; then
		echo "[Power off]"
		exit 0
	fi
	if [ -f /tmp/debug ]; then
		echo "[Debug mode detected]"
		/mnt/UDISK/BSOD
		exit 0
	fi
	echo "[Login launch failed rc=$rc -> fall back to camera directly]"

	# ---- 第二步：退化成原来的循环（直接跑相机）----
	while true; do
		if /mnt/UDISK/LiThermal; then
			echo "[Process exited normally]"
			break
		elif [ -f /tmp/poweroff ]; then
			echo "[Power off]"
			break
		elif [ -f /tmp/debug ]; then
			echo "[Debug mode detected]"
			/mnt/UDISK/BSOD
			break
		else
			/mnt/UDISK/BSOD
			sleep 1
		fi
	done
fi
