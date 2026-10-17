# 开机脚本（boot-first-camera 版）：
#   默认直接进相机 /mnt/UDISK/LiThermal；
#   桌面 lios 从相机里进：相机隐藏终端按「回桌面」会 exec lios（替换本进程），
#   lios 退出（关机/结束）后，本循环把相机拉回来。
#   兜底：相机缺失/启动失败 -> 进 lios 桌面（防黑屏）。

chmod +x /mnt/UDISK/LiThermal 2>/dev/null
chmod +x /mnt/UDISK/BSOD 2>/dev/null
[ -f /mnt/UDISK/lios ] && chmod +x /mnt/UDISK/lios 2>/dev/null

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

# 相机缺失：进 lios 桌面（防黑屏）
if [ ! -f /mnt/UDISK/LiThermal ]; then
	echo "[No camera -> desktop fallback]"
	if [ -f /mnt/UDISK/lios ]; then
		/mnt/UDISK/lios
		echo "[lios exited rc=$?]"
	fi
	exit 0
fi

# 默认：直接进相机
echo "[Boot camera first]"
run_camera
exit 0
