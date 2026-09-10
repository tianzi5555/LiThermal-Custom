#pragma once
#include "my_main.h"

class VideoPlayer
{
private:
public:
    // cv::VideoCapture video;
    lv_obj_t *img_obj;
    void init();
    void connect();
    void play();
    void disconnect();
};

extern VideoPlayer videoPlayer;

// 当前数码变焦裁剪区域: cropX,cropY,cropW,cropH,srcW,srcH
extern int g_last_crop[6];