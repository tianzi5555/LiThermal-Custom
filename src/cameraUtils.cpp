#include "httplib.h"
#include "cameraUtils.h"
#include <mutex>
#include <string.h>

httplib::Client cli(CAMERA_HOST_URL);
CameraUtils cameraUtils;
static std::mutex cli_mutex; // httplib::Client 不是线程安全的，串行化所有 HTTP 请求

// 大请求专用连接 + 专用锁
//
// jpegPicWithAppendData 一次响应约 100KB（JPEG 图 + 160x120 的原始测温矩阵），
// 而 getTemperature() 是 25Hz 的小 JSON 请求。两者原来共用 cli + cli_mutex，
// 于是互相排队：一次大请求会把 getTemperature() 顶住几百毫秒，反过来也一样，
// 表现就是「最高/最低/中心温度刷新变慢」。
// 这里给大请求单独一条 HTTP 连接和一把锁，两条链路彻底不再互相阻塞。
static httplib::Client cli_thermal(CAMERA_HOST_URL);
static std::mutex cli_thermal_mutex;

#define IF_CHECK_FILENAME "/proc/net/dev"
#define IF_CHECK_NAME "usb0"
#define IF_CHECK_MAX_SIZE 2048

bool CameraUtils::checkCameraConnection()
{
    static char if_check_buffer[IF_CHECK_MAX_SIZE];
    FILE *f = fopen(IF_CHECK_FILENAME, "r");
    if (f == NULL)
        return false;
    fread(if_check_buffer, IF_CHECK_MAX_SIZE, 1, f);
    fclose(f);
    if (strstr(if_check_buffer, IF_CHECK_NAME) != NULL)
    {
        return true;
    }
    return false;
}

void CameraUtils::setCameraIP()
{
    system("ifconfig usb0 up");
    system("ifconfig usb0 192.168.64.32");
}

void CameraUtils::initHTTPClient()
{
    cli.set_basic_auth("admin", "Ab123456");
    cli_thermal.set_basic_auth("admin", "Ab123456");
}
#include <cJSON.h>

// 读 4117 热成像模组自己的运行状态：CPU 占用 / 内存占用
// 接口：GET /ISAPI/System/status -> DeviceStatus.CPUList.CPU.cpuUtilization / MemoryList.Memory.memoryUsage
void CameraUtils::getModuleStatus()
{
    std::lock_guard<std::mutex> lock(cli_mutex);
    auto res = cli.Get("/ISAPI/System/status");
    if (!res || res->status != 200)
    {
        moduleOk = false;
        return;
    }
    cJSON *root = cJSON_Parse(res->body.c_str());
    if (!root)
    {
        moduleOk = false;
        return;
    }
    int cpu = -1, mem = -1;
    cJSON *cpuList = cJSON_GetObjectItem(root, "CPUList");
    if (cpuList)
    {
        cJSON *cpuItem = cJSON_GetObjectItem(cpuList, "CPU");
        if (cpuItem && cJSON_IsArray(cpuItem))
            cpuItem = cJSON_GetArrayItem(cpuItem, 0);
        if (cpuItem)
        {
            cJSON *v = cJSON_GetObjectItem(cpuItem, "cpuUtilization");
            if (v && cJSON_IsNumber(v))
                cpu = (int)v->valuedouble;
        }
    }
    cJSON *memList = cJSON_GetObjectItem(root, "MemoryList");
    if (memList)
    {
        cJSON *memItem = cJSON_GetObjectItem(memList, "Memory");
        if (memItem && cJSON_IsArray(memItem))
            memItem = cJSON_GetArrayItem(memItem, 0);
        if (memItem)
        {
            cJSON *v = cJSON_GetObjectItem(memItem, "memoryUsage");
            if (v && cJSON_IsNumber(v))
                mem = (int)v->valuedouble;
        }
    }
    cJSON_Delete(root);
    moduleCpu = cpu;
    moduleMem = mem;
    moduleOk = (cpu >= 0);
}

void CameraUtils::getTemperature()
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    auto res = cli.Get("/ISAPI/Thermal/channels/1/thermometry/1/rulesTemperatureInfo?format=json");
    if (res && res->status == 200)
    {
        /*
         {
        "ThermometryRulesTemperatureInfoList":  {
                "ThermometryRulesTemperatureInfo":      [{
                                "maxTemperature":       28.7,
                                "minTemperature":       27.7,
                                "averageTemperature":   28.3,
                                "MaxTemperaturePoint":  {
                                        "positionX":    1.000,
                                        "positionY":    0.983
                                },
                                "MinTemperaturePoint":  {
                                        "positionX":    0.918,
                                        "positionY":    0.008
                                },
                                "isFreezedata": false
                        }]
        }
}
         */

        cJSON *root = cJSON_Parse(res->body.c_str());
        cJSON *ThermometryRulesTemperatureInfoList = cJSON_GetObjectItem(root, "ThermometryRulesTemperatureInfoList");
        cJSON *ThermometryRulesTemperatureInfo = cJSON_GetObjectItem(ThermometryRulesTemperatureInfoList, "ThermometryRulesTemperatureInfo");
        cJSON *pack = cJSON_GetArrayItem(ThermometryRulesTemperatureInfo, 0);
        cJSON *maxTemperature = cJSON_GetObjectItem(pack, "maxTemperature");
        cJSON *minTemperature = cJSON_GetObjectItem(pack, "minTemperature");
        cJSON *averageTemperature = cJSON_GetObjectItem(pack, "averageTemperature");
        cJSON *MaxTemperaturePoint = cJSON_GetObjectItem(pack, "MaxTemperaturePoint");
        cJSON *MinTemperaturePoint = cJSON_GetObjectItem(pack, "MinTemperaturePoint");
        cJSON *positionX = cJSON_GetObjectItem(MaxTemperaturePoint, "positionX");
        cJSON *positionY = cJSON_GetObjectItem(MaxTemperaturePoint, "positionY");
        cJSON *positionX2 = cJSON_GetObjectItem(MinTemperaturePoint, "positionX");
        cJSON *positionY2 = cJSON_GetObjectItem(MinTemperaturePoint, "positionY");
        lastResult.maxTemperature = maxTemperature->valuedouble;
        lastResult.minTemperature = minTemperature->valuedouble;
        lastResult.averageTemperature = averageTemperature->valuedouble;
        lastResult.MaxTemperaturePoint.positionX = positionX->valuedouble;
        lastResult.MaxTemperaturePoint.positionY = positionY->valuedouble;
        lastResult.MinTemperaturePoint.positionX = positionX2->valuedouble;
        lastResult.MinTemperaturePoint.positionY = positionY2->valuedouble;
        lastResult.isFreezedata = cJSON_GetObjectItem(pack, "isFreezedata")->valueint;
        cJSON_Delete(root);
    }
    else
    {
        printf("Failed to get temperature\n");
    }
}

const char *color_palette[] = {
    "WhiteHot",
    "BlackHot",
    "Fusion1",
    "Fusion2",
    "Rainbow",
    "Ironbow1",
    "Ironbow2",
    "Sepia",
    "Color1",
    "Color2",
    "IceFire",
    "Rain",
    "RedHot",
    "GreenHot",
    "DeepBlue",
};

void CameraUtils::setColorPalette(int palette)
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    std::string data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Palettes><mode>";
    if (palette < 0 || palette >= IR_COLOR_PALETTE_MAX)
        palette = IR_COLOR_PALETTE_DEFAULT;
    data += color_palette[palette];
    data += "</mode></Palettes>";
    cli.Put("/ISAPI/Image/channels/1/Palettes", data, "application/xml");
}

void CameraUtils::setDigitalNoiceReduce(int mode, int frameLevel, int interFrameLevel)
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    std::string data;
    //= "<?xml version=\"1.0\" encoding=\"UTF-8\"?><DigitalNoiseReduction><mode>";
    if (mode < IR_DNR_MODE_CLOSE || mode > IR_DNR_MODE_ADVANCED)
        mode = IR_DNR_MODE_CLOSE;
    if (frameLevel < 0 || frameLevel > 100)
        frameLevel = 50;
    if (interFrameLevel < 0 || interFrameLevel > 100)
        interFrameLevel = 50;
    if (mode == IR_DNR_MODE_CLOSE)
    {
        data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><NoiseReduce><mode>close</mode></NoiseReduce>";
    }
    else if (mode == IR_DNR_MODE_GENERAL)
    {
        data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><NoiseReduce><mode>general</mode><GeneralMode><generalLevel>" + std::to_string(frameLevel) + "</frameLevel></GeneralMode></NoiseReduce>";
    }
    else
    {
        data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><NoiseReduce><mode>advanced</mode><AdvancedMode><FrameNoiseReduceLevel>" + std::to_string(frameLevel) + "</FrameNoiseReduceLevel><InterFrameNoiseReduceLevel>" + std::to_string(interFrameLevel) + "</InterFrameNoiseReduceLevel></AdvancedMode></NoiseReduce>";
    }
    cli.Put("/ISAPI/Image/channels/1/noiseReduce", data, "application/xml");
}

void CameraUtils::setDigitalDetailEnhancement(bool en, int level)
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    std::string data;
    if (level < 0 || level > 100)
        level = 50;
    if (en)
    {
        data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><DDE><mode>on</mode><GeneralMode><enhanceLevel>" + std::to_string(level) + "</enhanceLevel></GeneralMode></DDE>";
    }
    else
    {
        data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><DDE><mode>close</mode></DDE>";
    }
    cli.Put("/ISAPI/Image/channels/1/DDE", data, "application/xml");
}

static std::string extract_boundary(const std::string &contentType)
{
    std::string boundary;
    size_t pos = contentType.find("boundary=");
    if (pos != std::string::npos)
    {
        boundary = contentType.substr(pos + 9);
    }
    return boundary;
}

// multipart 里的一个分段：直接指向响应体内部，不做任何数据拷贝
struct multipart_part_t
{
    const char *data;
    size_t size;
};

// 就地切分 multipart 响应。
// 原来的实现把每一段都 substr 成 std::string（还有 vector<string> 容器），
// 一次 100KB 的响应要额外 malloc/memcpy 两三百 KB；在 117MB 内存的机器上
// 每秒来一发这种大块 malloc/free，会带来明显的堆抖动和缺页。
// 切分规则（用 Content-Length 定长、按 boundary 找下一段）和原来完全一致，
// 只是改成返回指向 body 内部的“只读切片”，不复制数据。
static std::vector<multipart_part_t> split_multipart_ro(const std::string &body, const std::string &boundary)
{
    std::vector<multipart_part_t> parts;
    std::string delimiter = "--" + boundary;
    const char *base = body.data();
    size_t start = body.find(delimiter);
    while (start != std::string::npos)
    {
        size_t start_length = body.find("Content-Length:", start);
        if (start_length == std::string::npos)
        {
            break;
        }
        size_t end_length = body.find("\r\n", start_length);
        if (end_length == std::string::npos)
        {
            break;
        }
        size_t this_part_length = (size_t)std::stoi(body.substr(start_length + 15, end_length));
        size_t data_start = body.find("\r\n\r\n", start);
        if (data_start == std::string::npos)
        {
            break;
        }
        data_start += 4;
        size_t end = body.find(delimiter, data_start + this_part_length);
        if (end == std::string::npos)
        {
            break;
        }
        multipart_part_t part;
        part.data = base + data_start;
        part.size = this_part_length;
        parts.push_back(part);
        start = end;
    }
    return parts;
}

float CameraUtils::readJpegWithExtra(const char *save_filename, int result_x, int result_y)
{
    // 注意：这里用 cli_thermal/cli_thermal_mutex（大请求专用），不再占用 getTemperature() 的 cli_mutex，
    // 所以这条 100KB 的请求不会再把 25Hz 的温度轮询顶住。
    std::lock_guard<std::mutex> cli_lock(cli_thermal_mutex);
    auto res = cli_thermal.Get("/ISAPI/Thermal/channels/1/thermometry/jpegPicWithAppendData?format=json");
    if (res && res->status == 200)
    {
        // Parse the response headers to get the boundary string
        std::string contentType = res->get_header_value("Content-Type");
        std::string boundary = extract_boundary(contentType);

        // Split the response body into parts using the boundary string
        std::vector<multipart_part_t> parts = split_multipart_ro(res->body, boundary);
        if (parts.size() < 3)
        {
            printf("Failed to split multipart response\n");
            return 0.0f;
        }
        if (save_filename != NULL)
        {
            // 落盘也直接写切片，省掉一份 100KB 的临时拷贝；写出的字节和以前完全一样
            std::ofstream file(std::string(save_filename) + ".jpeg");
            if (file.is_open())
            {
                file.write(parts[1].data, (std::streamsize)parts[1].size);
                file.close();
            }
            std::ofstream file2(std::string(save_filename) + ".raw");
            if (file2.is_open())
            {
                file2.write(parts[2].data, (std::streamsize)parts[2].size);
                file2.close();
            }
        }
        // 只要第 3 段（原始测温矩阵 160x120 float）里目标坐标的那 4 个字节，不再整段拷贝。
        // 加边界检查：以前是无条件下标访问，模组万一返回的矩阵比预期小就会读越界。
        size_t offset = (size_t)(result_x + result_y * 160) * 4;
        if (offset + sizeof(float) <= parts[2].size)
        {
            memcpy(&lastCenterTemperature, parts[2].data + offset, sizeof(float));
        }
        return lastCenterTemperature;
    }
    return 0.0f;
}

void CameraUtils::setCenterMeasure(bool en)
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    auto res = cli.Get("/ISAPI/Thermal/channels/1/temperatureCorrect?format=json");
    char data[1024];
    if (res && res->status == 200)
    {
        cJSON *root = cJSON_Parse(res->body.c_str());
        cJSON *TemperatureCorrect = cJSON_GetObjectItem(root, "TemperatureCorrect");
        cJSON *streamOverlay = cJSON_GetObjectItem(TemperatureCorrect, "streamOverlay");
        cJSON_SetBoolValue(streamOverlay, en);
        cJSON_PrintPreallocated(root, data, 1024, cJSON_False);
        cJSON_Delete(root);
        cli.Put("/ISAPI/Thermal/channels/1/temperatureCorrect?format=json", data, "application/json");
    }
}

void CameraUtils::calibrateManually()
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    cli.Put("/ISAPI/Image/channels/1/ManualShutterCorrect", "", "application/xml");
}

void CameraUtils::set4117Cursor(bool min, bool max)
{
    std::lock_guard<std::mutex> cli_lock(cli_mutex);
    auto res = cli.Get("/ISAPI/Thermal/channels/1/thermometry/basicParam");
    int len;
    if (res && res->status == 200) {
        std::string body = res->body;
        std::string line;
        std::string result = "";
        int last_start = 0;
        int this_end;
        int length = body.length();
        while (1)
        {
            this_end = body.find("\n", last_start);
            if (this_end == std::string::npos){
                result += body.substr(last_start, length - last_start);
                break;
            }
            line = body.substr(last_start, this_end - last_start);
            if (line == "") {
                printf("Warning: Empty line\n");
                break;
            }
            last_start = this_end + 1;
            if (line.find("displayMax") != std::string::npos)
            {
                if (max) {
                    result += "<displayMaxTemperatureEnabled>true</displayMaxTemperatureEnabled>\n";
                } else {
                    result += "<displayMaxTemperatureEnabled>false</displayMaxTemperatureEnabled>\n";
                }
            }
            else if (line.find("displayMin") != std::string::npos)
            {
                if (min) {
                    result += "<displayMinTemperatureEnabled>true</displayMinTemperatureEnabled>\n";
                } else {
                    result += "<displayMinTemperatureEnabled>false</displayMinTemperatureEnabled>\n";
                }
            }
            else if (line.find("<enabled>") != std::string::npos)
            {
                result += "<enabled>true</enabled>\n";
            }
            else if (line.find("<streamOverlay>") != std::string::npos)
            {
                result += (min || max) ? "<streamOverlay>true</streamOverlay>\n" : "<streamOverlay>false</streamOverlay>\n";
            }
            else
            {
                result += line + "\n";
            }
        }
        cli.Put("/ISAPI/Thermal/channels/1/thermometry/basicParam", result, "application/x-www-form-urlencoded; charset=UTF-8");
    }
}