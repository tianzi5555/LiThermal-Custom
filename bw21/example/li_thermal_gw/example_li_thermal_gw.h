/*
 * Copyright (c) 2026 LiThermal (GPL-3.0)
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef _EXAMPLE_LI_THERMAL_GW_H_
#define _EXAMPLE_LI_THERMAL_GW_H_

/* WiFi 连接状态 */
enum {
	GW_STATE_IDLE = 0,
	GW_STATE_CONNECTING = 1,
	GW_STATE_CONNECTED = 2,
	GW_STATE_FAILED = 3,
};

void li_thermal_gw_start(void);

#endif /* _EXAMPLE_LI_THERMAL_GW_H_ */
