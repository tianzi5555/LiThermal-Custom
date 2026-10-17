// LiOS 权限查询接口
//
// 主程序（lios）用 -rdynamic 导出，插件 .so 可以直接调用：
//     extern "C" bool lios_perm_allowed(const char *id, const char *key);
// 其中 id 用插件自己的 lios_app_info.id，key 取 "storage" / "camera" / "network" / "bg"。
//
// 第一版语义：这里只返回用户在该应用的权限页里设置的开/关状态，
// **不是内核级沙箱**（musl、无 namespace/seccomp）。插件应自觉遵守。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // 启动时调一次：起 worker 并把所有应用的权限记录读进来
    void lios_perm_init(void);

    bool lios_perm_allowed(const char *id, const char *key);

#ifdef __cplusplus
}
#endif
