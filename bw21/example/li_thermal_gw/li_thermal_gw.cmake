### LiThermal BW21 网关例程
### 复制自 SDK 的 usbh_cdc_ecm_new.cmake，加了 LI_THERMAL_GW 宏
### （打了补丁的 lwIP etharp.c/ip4.c 里用 #ifdef LI_THERMAL_GW 包住代理 ARP / 主机路由钩子）

### add header files ###
list (
    APPEND app_example_inc_path
    "${sdk_root}/component/example/${EXAMPLE}"
    "${sdk_root}/component/network/dhcp"
    "${sdk_root}/component/network/httpd"
    "${sdk_root}/component/wifi/api"
)

### add source file ###
set(EXAMPLE_SOURCE_PATH)
file(GLOB EXAMPLE_SOURCE_PATH ${CMAKE_CURRENT_LIST_DIR}/*.c)
list(
	APPEND app_example_sources
    ${EXAMPLE_SOURCE_PATH}
)
message(STATUS "${EXAMPLE_SOURCE_PATH}")

# app_inc_path (application.cmake) unconditionally includes
# component/usb/host_new/core, which also has a usbh.h/usbh_config_t (the
# legacy driver's, different shape) - without this, #include "usbh_cdc_ecm.h"
# would resolve its own #include "usbh.h" to that legacy one.
set_source_files_properties(
	${EXAMPLE_SOURCE_PATH}
	PROPERTIES COMPILE_OPTIONS
	"-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/hal;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/common;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/device/core;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/core;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/cdc_acm;-iquote;${sdk_root}/component/usb/usb_stack/${usb_stack_ver}/host/cdc_ecm"
)

### add flags ###
# CONFIG_ETHERNET=1：让 LwIP_Init() 注册 xnetif[ETHERNET_IDX]（USB 网卡口）
# LI_THERMAL_GW=1  ：启用打补丁的 lwIP 里的代理 ARP / 主机路由钩子
#   （app_example_flags 是 target-wide 的，lwIP 源码也会带上这个宏）
list(
	APPEND app_example_flags
	CONFIG_ETHERNET=1
	LI_THERMAL_GW=1
)
