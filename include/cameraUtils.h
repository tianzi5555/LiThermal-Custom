#pragma once
#include <pthread.h>
#include <stdint.h>
#define CAMERA_HOST_URL "http://192.168.64.64"

// 大快照（jpegPicWithAppendData，~100KB，实测模组要 270ms 才给）最后一次返回的时刻。
// 只给临时探针做「快照是不是把 RTSP 视频拖卡的元凶」的相关性统计用。
extern volatile uint32_t g_last_snap_end_us;

typedef struct temperature_point_t
{
    float maxTemperature;
    float minTemperature;
    float averageTemperature;
    struct
    {
        float positionX;
        float positionY;
    } MaxTemperaturePoint, MinTemperaturePoint;
    bool isFreezedata;
} temperature_point_t;

// 摄像头调色板
#define IR_COLOR_PALETTE_WHITEHOT 0
#define IR_COLOR_PALETTE_BLACKHOT 1
#define IR_COLOR_PALETTE_FUSION1 2
#define IR_COLOR_PALETTE_FUSION2 3
#define IR_COLOR_PALETTE_RAINBOW 4
#define IR_COLOR_PALETTE_IRONBOW1 5
#define IR_COLOR_PALETTE_IRONBOW2 6
#define IR_COLOR_PALETTE_SEPIA 7
#define IR_COLOR_PALETTE_COLOR1 8
#define IR_COLOR_PALETTE_COLOR2 9
#define IR_COLOR_PALETTE_ICEFIRE 10
#define IR_COLOR_PALETTE_RAIN 11
#define IR_COLOR_PALETTE_REDHOT 12
#define IR_COLOR_PALETTE_GREENHOT 13
#define IR_COLOR_PALETTE_DEEPBLUE 14
#define IR_COLOR_PALETTE_MAX 15
#define IR_COLOR_PALETTE_DEFAULT IR_COLOR_PALETTE_FUSION1

// 数字降噪模式
#define IR_DNR_MODE_CLOSE 0
#define IR_DNR_MODE_GENERAL 1
#define IR_DNR_MODE_ADVANCED 2
class CameraUtils
{
public:
    // 最近一次温度读数。
    // 注意：现在由独立「温度轮询线程」写（原来在主循环里同步写），
    // 而 UI/图表线程会读，所以改用 lastResultMutex 保护 —— 写法：
    //   写：getTemperature() 内部填好本地副本后整体发布
    //   读：copyTemperatureResult()（或 copyTemperature 只取一份）
    temperature_point_t lastResult;
    pthread_mutex_t lastResultMutex = PTHREAD_MUTEX_INITIALIZER;
    float lastCenterTemperature;
    bool connected = false;
    // 热成像模组（4117）自己的状态，从它的 ISAPI 读；读不到就是 -1
    int moduleCpu = -1;
    int moduleMem = -1;
    bool moduleOk = false;
    bool checkCameraConnection(); // 检查摄像头连接
    void setCameraIP();           // 设置摄像头固定IP
    void initHTTPClient();        // 初始化HTTP客户端（即设置HTTP认证）
    void getModuleStatus();       // 读 4117 自己的 CPU/内存占用（节流调用）
    void getTemperature();        // 获取摄像头温度信息（阻塞式 HTTP，放到独立线程里跑）
    void copyTemperatureResult(temperature_point_t &out); // 加锁取一份快照给 UI/图表用
    void setColorPalette(int palette);
    void setDigitalNoiceReduce(int mode, int frameLevel, int interFrameLevel);
    void setDigitalDetailEnhancement(bool en, int level);
    float readJpegWithExtra(const char *save_filename = NULL, int result_x = 160 / 2, int result_y = 120 / 2);
    void setCenterMeasure(bool en);
    void calibrateManually();
    void set4117Cursor(bool min, bool max);
};

extern CameraUtils cameraUtils;