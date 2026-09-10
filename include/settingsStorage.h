#pragma once
#include <stdint.h>

typedef struct settingsStorage_t
{
    uint32_t __head;

    uint32_t brightness;
    uint32_t colorPalette;
    uint32_t enableGraph;
    uint32_t graphPos;
    uint32_t graphSize;
    uint32_t graphRefreshInterval;
    uint32_t enableMaxValueDisplay;
    uint32_t enableMinValueDisplay;
    uint32_t enableCenterValueDisplay;
    uint32_t preserveUI;
    uint32_t useBlackFlashBang;
    uint32_t use4117Cursors;
    uint32_t digitalZoom;     // 数码变焦倍率，100=1.0x，400=4.0x
    uint32_t autoContrast;    // 自动对比度：1=开启，0=手动
    uint32_t contrast;        // 手动对比度 0..100，50为中性
    uint32_t crosshairColor;  // 内置准星颜色：0=白 1=绿 2=红 3=黄 4=蓝
    uint32_t crosshairLength; // 内置准星四条线长度（像素）
    uint32_t crosshairThickness; // 内置准星线粗细（像素）
    uint32_t __tail;
} settingsStorage_t;
extern settingsStorage_t globalSettings;
#define GRAPH_DATA_SOURCE_MAX 0
#define GRAPH_DATA_SOURCE_MIN 1

void settings_default();
void settings_load();
void settings_save();
