### li_video_stream —— BW21 摄像头 → USB(CDC-ECM) → LiThermal 相机
###
### 复制自 li_thermal_gw.cmake（保留那段 -iquote，解决 usb_stack 头文件歧义），
### 加了多媒体例程需要的 include 路径。
###
### 注意：本例程【不需要】LI_THERMAL_GW 宏 —— 点对点静态 IP 直连，
###       不用代理 ARP / 主机路由 / IP 转发，所以不用给 lwIP 打任何补丁。

### add header files ###
list (
    APPEND app_example_inc_path
    "${sdk_root}/component/example/${EXAMPLE}"
    "${sdk_root}/project/realtek_amebapro2_v0_example/src/mmfv2_video_example"
)

### add source file ###
set(EXAMPLE_SOURCE_PATH)
file(GLOB EXAMPLE_SOURCE_PATH ${CMAKE_CURRENT_LIST_DIR}/*.c)
list(
	APPEND app_example_sources
    ${EXAMPLE_SOURCE_PATH}
)
message(STATUS "${EXAMPLE_SOURCE_PATH}")

# app_inc_path (application.cmake) 无条件包含 component/usb/host_new/core，
# 那里也有一个 usbh.h/usbh_config_t（旧驱动的，形状不同）——
# 不加这段的话，#include "usbh_cdc_ecm.h" 里的 #include "usbh.h" 会解析到旧的那个。
set_source_files_properties(
	${EXAMPLE_SOURCE_PATH}
	PROPERTIES COMPILE_OPTIONS
	"-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/hal;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/common;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/device/core;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/core;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/cdc_acm;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/cdc_ecm"
)

### add flags ###
# CONFIG_ETHERNET=1：让 LwIP_Init() 注册 xnetif[ETHERNET_IDX]（USB 网卡口）
list(
	APPEND app_example_flags
	CONFIG_ETHERNET=1
)
