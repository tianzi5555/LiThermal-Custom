// LiOS 文件查看器（图片 / 视频 / 文本）—— 接口
//
// 由文件管理器（files.cpp）调用：点开一个文件 -> viewer_open() 换成查看器页；
// 查看器开着时，按键/滚轮先给查看器，菜单键退出回列表（原来那个目录）。
#pragma once

#include <lvgl.h>

// 查看器类型
enum
{
    VIEW_OTHER = 0, // 不认识 -> 还是显示「信息卡片」（文件管理器负责）
    VIEW_IMAGE = 1, // .jpg .jpeg .png .bmp（ffmpeg 解第一帧）
    VIEW_VIDEO = 2, // .mp4 .avi .mkv .mov .flv（ffmpeg 顺序解码 + 双缓冲丢帧）
    VIEW_TEXT = 3   // .txt .log .conf .sh .json .md .csv，或 <64KB 的未知类型
};

// 按扩展名判断（未知类型小于 64KB 当文本）
int viewer_kind_of(const char *name, unsigned long long size);

// 建查看器页（parent = 文件管理器页面容器）+ 起 worker 线程；返回查看器页面对象
lv_obj_t *viewer_open(lv_obj_t *parent, const char *path, int kind);

bool viewer_is_open(void);
bool viewer_key(int key);      // UI 线程、已持锁；返回 true = 已处理
void viewer_tick(void);        // UI 线程、已持锁（取 worker 结果、更新 LVGL）
void viewer_wheel(int diff);   // 文本滚动
void viewer_close(void);       // 停线程 / 释放解码器与缓冲 / 删页面
