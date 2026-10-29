/*
 * Copyright (c) 2026 LiThermal (GPL-3.0)
 * SPDX-License-Identifier: GPL-3.0
 *
 * li_video_stream：把 BW21 板载摄像头（1080p30 sensor）的 JPEG 画面
 * 通过 USB-C 线（BW21 = USB Host + CDC-ECM，相机 = ECM gadget）用 TCP 发给 LiThermal 相机。
 */

#ifndef _EXAMPLE_LI_VIDEO_STREAM_H_
#define _EXAMPLE_LI_VIDEO_STREAM_H_

void li_video_stream_start(void);

#endif /* _EXAMPLE_LI_VIDEO_STREAM_H_ */
