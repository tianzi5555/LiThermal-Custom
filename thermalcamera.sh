# 开机脚本：默认直接进 LiOS 桌面；lios 缺失/启动失败则兜底跑相机（防黑屏）
#
#   lios（桌面）→ 点「相机」方块 → fork+exec /mnt/UDISK/LiThermal
#                → 相机退出后桌面自己接管显示（不需要这里来回切）
if [ -f /mnt/UDISK/LiThermal ]; then
	chmod +x /mnt/UDISK/LiThermal
	chmod +x /mnt/UDISK/BSOD
	[ -f /mnt/UDISK/lios ] && chmod +x /mnt/UDISK/lios

	# 原来的相机循环（也是 lios 缺失 / 启动失败时的兜底）
	run_camera() {
		while true; do
			if /mnt/UDISK/LiThermal; then
				echo "[camera exited normally]"
				return 0
			fi
			if [ -f /tmp/poweroff ]; then
				echo "[Power off]"
				return 0
			fi
			if [ -f /tmp/debug ]; then
				echo "[Debug mode detected]"
				/mnt/UDISK/BSOD
				return 0
			fi
			/mnt/UDISK/BSOD
			sleep 1
		done
	}

	# 没有 lios：老老实实跑相机（和以前完全一样）
	if [ ! -f /mnt/UDISK/lios ]; then
		echo "[No lios -> camera only]"
		run_camera
		exit 0
	fi

	# 有 lios：开机直接进桌面
	while true; do
		if [ -f /tmp/poweroff ]; then
			echo "[Power off]"
			exit 0
		fi
		if [ -f /tmp/debug ]; then
			echo "[Debug mode detected]"
			/mnt/UDISK/BSOD
			exit 0
		fi

		echo "[Run lios]"
		/mnt/UDISK/lios
		rc=$?
		echo "[lios exited rc=$rc]"

		if [ $rc -eq 0 ]; then
			# 正常退出（关机流程会先建 /tmp/poweroff）
			exit 0
		fi

		# lios 起不来 / 崩了：先兜底跑相机，别留黑屏
		echo "[lios failed -> fallback to camera]"
		run_camera
		if [ -f /tmp/poweroff ]; then
			exit 0
		fi
		sleep 1
	done
fi
