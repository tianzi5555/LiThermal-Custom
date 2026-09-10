#include <my_main.h>
#include <string.h>
#define SETTINGS_HEAD 0x80000001
#define SETTINGS_TAIL 0x1715600D

// 旧版固件(增加数码变焦/对比度之前)的配置结构，用于兼容升级
typedef struct settingsStorage_legacy_t
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
    uint32_t __tail;
} settingsStorage_legacy_t;

settingsStorage_t globalSettings;

void settings_default()
{
    globalSettings.__head = SETTINGS_HEAD;
    globalSettings.brightness = 170;
    globalSettings.colorPalette = IR_COLOR_PALETTE_DEFAULT;
    globalSettings.enableGraph = false;
    globalSettings.graphPos = 0;
    globalSettings.graphRefreshInterval = 0;
    globalSettings.graphSize = 0;
    globalSettings.enableMaxValueDisplay = false;
    globalSettings.enableMinValueDisplay = false;
    globalSettings.enableCenterValueDisplay = true;
    globalSettings.preserveUI = false;
    globalSettings.useBlackFlashBang = false;
    globalSettings.use4117Cursors = false;
    globalSettings.digitalZoom = 100;
    globalSettings.autoContrast = true;
    globalSettings.contrast = 50;
    globalSettings.__tail = SETTINGS_TAIL;
}

void settings_load()
{
    FILE *fp = fopen(SETTINGS_PATH, "rb");
    if (fp == NULL)
    {
        printf("[Warning] No settings file\n");
        settings_default();
        settings_save();
        return;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    memset(&globalSettings, 0, sizeof(globalSettings));

    if (file_size >= (long)sizeof(globalSettings))
    {
        fread(&globalSettings, sizeof(globalSettings), 1, fp);
    }
    else if (file_size == (long)sizeof(settingsStorage_legacy_t))
    {
        // 旧版配置文件：先读旧字段，再补新字段默认值
        settingsStorage_legacy_t legacy;
        memset(&legacy, 0, sizeof(legacy));
        fread(&legacy, sizeof(legacy), 1, fp);
        memcpy(&globalSettings, &legacy, sizeof(legacy));
        globalSettings.digitalZoom = 100;
        globalSettings.autoContrast = true;
        globalSettings.contrast = 50;
        globalSettings.__tail = SETTINGS_TAIL;
        if (globalSettings.__head == SETTINGS_HEAD && legacy.__tail == SETTINGS_TAIL)
        {
            printf("[Info] Migrated legacy settings\n");
            fclose(fp);
            settings_save();
            return;
        }
    }

    fclose(fp);

    if (globalSettings.__tail != SETTINGS_TAIL || globalSettings.__head != SETTINGS_HEAD)
    {
        printf("[Warning] Corrupted settings storage\n");
        settings_default();
        settings_save();
    }
    else
    {
        // 给非法值兜底
        if (globalSettings.digitalZoom < 100)
            globalSettings.digitalZoom = 100;
        if (globalSettings.digitalZoom > 400)
            globalSettings.digitalZoom = 400;
        if (globalSettings.autoContrast > 1)
            globalSettings.autoContrast = 1;
        if (globalSettings.contrast > 100)
            globalSettings.contrast = 50;
    }
}

void settings_save()
{
    FILE *fp = fopen(SETTINGS_PATH, "wb");
    if (fp == NULL)
    {
        printf("[Error] Unable to save settings\n");
        return;
    }
    fwrite(&globalSettings, sizeof(globalSettings), 1, fp);
    fclose(fp);
}
