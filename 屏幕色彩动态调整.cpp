#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cmath>
#include "cJSON.h"

struct Color {
    int red;
    int green;
    int blue;
};

int getSystemSetting(const std::string& settingName) {
    char buffer[128];
    std::string result;
    std::string command = "settings get system " + settingName;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed!");
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    return std::stoi(result);
}

void setKcal(int red, int green, int blue) {
    std::string command = "mat4 '" + std::to_string(red) + " " + std::to_string(green) + " " + std::to_string(blue) + "'";
    system(command.c_str());
}

Color interpolateColor(const Color& color1, const Color& color2, float fraction) {
    Color result;
    result.red   = static_cast<int>(std::lerp(color1.red, color2.red, fraction));
    result.green = static_cast<int>(std::lerp(color1.green, color2.green, fraction));
    result.blue  = static_cast<int>(std::lerp(color1.blue, color2.blue, fraction));
    return result;
}

// This function assumes that the cJSON* passed to it is an array of colors under a refresh rate
Color getColorForBrightness(cJSON* brightnessArray, int brightness) {
    Color lowerColor = {0, 0, 0};
    Color upperColor = {0, 0, 0}; // 初始化为配置文件中的最大RGB值。
    int lowerBrightness = 0;
    int upperBrightness = 100;  // 假设配置文件中最高亮度值永远是100%
    cJSON* brightnessSetting = nullptr;

    // 赋初值
    bool foundLower = false;
    bool foundUpper = false;

    cJSON_ArrayForEach(brightnessSetting, brightnessArray) {
        int currentBrightness = cJSON_GetObjectItem(brightnessSetting, "brightness")->valueint;
        if (currentBrightness <= brightness) {
            foundLower = true;
            lowerBrightness = currentBrightness;
            lowerColor.red = cJSON_GetObjectItem(brightnessSetting, "red")->valueint;
            lowerColor.green = cJSON_GetObjectItem(brightnessSetting, "green")->valueint;
            lowerColor.blue = cJSON_GetObjectItem(brightnessSetting, "blue")->valueint;
        } 
        if (currentBrightness >= brightness) {
            foundUpper = true;
            upperBrightness = currentBrightness;
            upperColor.red = cJSON_GetObjectItem(brightnessSetting, "red")->valueint;
            upperColor.green = cJSON_GetObjectItem(brightnessSetting, "green")->valueint;
            upperColor.blue = cJSON_GetObjectItem(brightnessSetting, "blue")->valueint;
            if (brightness == currentBrightness){
                // 亮度与当前档位正好相等，无需插值，直接返回此颜色
                return { upperColor.red, upperColor.green, upperColor.blue };
            }
            break; // 找到第一个比目标亮度高的档位，停止查找
        }
    }
  
    // 处理未找到上限的情况，使用查找到的最高亮度
    if (!foundUpper && foundLower) {
        upperColor = lowerColor;
        upperBrightness = lowerBrightness;
    }

    // 计算插值的比例
    float fraction = static_cast<float>(brightness - lowerBrightness) / (upperBrightness - lowerBrightness);
    return interpolateColor(lowerColor, upperColor, fraction);
}

void printHelp() {
    std::cout << "用法:" << std::endl;
    std::cout << "-s <秒> 检测间隔" << std::endl;
    std::cout << "-h 显示帮助信息" << std::endl;
    std::cout << "本程序用于任意刷新率档位下不同亮度的色彩比值控制" << std::endl;
    std::cout << "作者:囫碾" << std::endl;
}

int main(int argc, char* argv[]) {
    int interval = 1;  // Default interval in seconds
    int lastBrightness = -1;  // 初始化上次的亮度信息
    int opt;
    while ((opt = getopt(argc, argv, "hs:")) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return 0;
            case 's':
                interval = std::atoi(optarg);
                break;
            default:
                printHelp();
                return 1;
        }
    }

    std::ifstream ifs("config.json");
    if (!ifs.is_open()) {
        std::cerr << "错误： 无法打开配置文件" << std::endl;
        return 1;
    }
    std::string configContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    cJSON* configJson = cJSON_Parse(configContent.c_str());
    if (!configJson) {
        std::cerr << "错误： 配置文件中的 JSON 格式不正确" << std::endl;
        return 1;
    }

    // 查找最接近的刷新率档位
    int closestRefreshRate = -1;
    cJSON* closestRateConfig = nullptr;
    const int actualRefreshRate = getSystemSetting("peak_refresh_rate");
    cJSON* refreshRatesConfig = configJson->child;  // 获取配置文件的第一个刷新率配置节点
    while (refreshRatesConfig) {
        const int refreshRate = std::stoi(refreshRatesConfig->string);
        if (closestRefreshRate == -1 || std::abs(actualRefreshRate - refreshRate) < std::abs(actualRefreshRate - closestRefreshRate)) {
            closestRefreshRate = refreshRate;
            closestRateConfig = refreshRatesConfig;
        }
        refreshRatesConfig = refreshRatesConfig->next;  // 移动到下一个配置节点
    }
    if (!closestRateConfig) {
        std::cerr << "错误： 配置文件中未发现刷新率配置" << std::endl;
        cJSON_Delete(configJson);
        return 1;
    }

while (true) {
        int brightness = getSystemSetting("screen_brightness");
        
        // 增加合理性检查
        if (brightness < 0 || brightness > 255) {
            brightness = 255;  // 使用255作为默认值
        }
        
        // 检查是否与上次的亮度一致
        if (brightness == lastBrightness) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            continue;   // 若一致，等待后重新循环
        }

        int brightnessPercentage = static_cast<int>(round(brightness / 255.0 * 100.0));
        cJSON* brightnessArray = cJSON_GetObjectItemCaseSensitive(closestRateConfig, "brightness_levels");

        if (!brightnessArray) {
            std::cerr << "错误：未找到最接近刷新率的亮度级别配置: " << closestRefreshRate << "Hz." << std::endl;
            break;
        }

        Color interpolatedColor = getColorForBrightness(brightnessArray, brightnessPercentage);
        setKcal(interpolatedColor.red, interpolatedColor.green, interpolatedColor.blue);

        // 更新上次的亮度信息
        lastBrightness = brightness;

        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
    cJSON_Delete(configJson);
    return 0;
}
