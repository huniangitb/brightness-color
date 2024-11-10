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

// 定义颜色结构体
struct Color {
    int red;
    int green;
    int blue;
};

// 获取系统设置值
int getSystemSetting(const std::string& settingName) {
    std::string command = "settings get system " + settingName;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("错误：无法执行命令 '" + command + "'");
    }

    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    try {
        return std::stoi(result);
    } catch (const std::exception&) {
        throw std::runtime_error("错误：无法解析设置值 '" + result + "'");
    }
}

// 设置色彩
void setKcal(int red, int green, int blue) {
    std::string command = "mat4 '" + std::to_string(red) + " " + std::to_string(green) + " " + std::to_string(blue) + "'";
    if (system(command.c_str()) != 0) {
        std::cerr << "错误：执行命令失败: " << command << std::endl;
    }
}

// 颜色插值
Color interpolateColor(const Color& color1, const Color& color2, float fraction) {
    return Color{
        static_cast<int>(std::lerp(color1.red, color2.red, fraction)),
        static_cast<int>(std::lerp(color1.green, color2.green, fraction)),
        static_cast<int>(std::lerp(color1.blue, color2.blue, fraction))
    };
}

// 根据亮度获取对应颜色
Color getColorForBrightness(cJSON* brightnessArray, int brightness) {
    Color lowerColor = {0, 0, 0};
    Color upperColor = {0, 0, 0};
    int lowerBrightness = 0;
    int upperBrightness = 100;
    cJSON* brightnessSetting = nullptr;
    bool foundExact = false;

    cJSON_ArrayForEach(brightnessSetting, brightnessArray) {
        cJSON* brightnessItem = cJSON_GetObjectItem(brightnessSetting, "brightness");
        if (!brightnessItem || !cJSON_IsNumber(brightnessItem)) {
            continue;
        }
        int currentBrightness = brightnessItem->valueint;

        if (currentBrightness == brightness) {
            lowerColor.red = cJSON_GetObjectItem(brightnessSetting, "red")->valueint;
            lowerColor.green = cJSON_GetObjectItem(brightnessSetting, "green")->valueint;
            lowerColor.blue = cJSON_GetObjectItem(brightnessSetting, "blue")->valueint;
            foundExact = true;
            break;
        }

        if (currentBrightness < brightness) {
            lowerBrightness = currentBrightness;
            lowerColor.red = cJSON_GetObjectItem(brightnessSetting, "red")->valueint;
            lowerColor.green = cJSON_GetObjectItem(brightnessSetting, "green")->valueint;
            lowerColor.blue = cJSON_GetObjectItem(brightnessSetting, "blue")->valueint;
        }

        if (currentBrightness > brightness) {
            upperBrightness = currentBrightness;
            upperColor.red = cJSON_GetObjectItem(brightnessSetting, "red")->valueint;
            upperColor.green = cJSON_GetObjectItem(brightnessSetting, "green")->valueint;
            upperColor.blue = cJSON_GetObjectItem(brightnessSetting, "blue")->valueint;
            break;
        }
    }

    if (foundExact) {
        return lowerColor;
    }

    // 处理未找到上限的情况
    if (upperBrightness == 100 && lowerBrightness == 0 &&
        lowerColor.red == 0 && lowerColor.green == 0 && lowerColor.blue == 0) {
        return upperColor;
    }

    // 避免除以零
    if (upperBrightness == lowerBrightness) {
        return lowerColor;
    }

    float fraction = static_cast<float>(brightness - lowerBrightness) / (upperBrightness - lowerBrightness);
    return interpolateColor(lowerColor, upperColor, fraction);
}

// 打印帮助信息
void printHelp() {
    std::cout << "用法:" << std::endl;
    std::cout << "  -s <秒>    设置检测间隔（秒）" << std::endl;
    std::cout << "  -h         显示帮助信息" << std::endl;
    std::cout << "本程序用于在不同刷新率档位下，根据屏幕亮度控制色彩比值" << std::endl;
    std::cout << "作者: 囫碾" << std::endl;
}

int main(int argc, char* argv[]) {
    int interval = 1;  // 默认检测间隔为1秒
    int lastBrightness = -1;  // 上次亮度值初始化
    int opt;

    // 解析命令行参数
    while ((opt = getopt(argc, argv, "hs:")) != -1) {
        switch (opt) {
            case 'h':
                printHelp();
                return 0;
            case 's':
                interval = std::atoi(optarg);
                if (interval <= 0) {
                    std::cerr << "错误：检测间隔必须为正整数。" << std::endl;
                    return 1;
                }
                break;
            default:
                printHelp();
                return 1;
        }
    }

    // 读取配置文件
    std::ifstream ifs("config.json");
    if (!ifs.is_open()) {
        std::cerr << "错误：无法打开配置文件 'config.json'" << std::endl;
        return 1;
    }

    std::string configContent((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    cJSON* configJson = cJSON_Parse(configContent.c_str());
    if (!configJson) {
        std::cerr << "错误：配置文件中的 JSON 格式不正确" << std::endl;
        return 1;
    }

    // 获取系统实际刷新率
    int actualRefreshRate;
    try {
        actualRefreshRate = getSystemSetting("peak_refresh_rate");
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        cJSON_Delete(configJson);
        return 1;
    }

    // 查找最接近的刷新率配置
    int closestRefreshRate = -1;
    cJSON* closestRateConfig = nullptr;
    cJSON* refreshRatesConfig = configJson->child;  // 获取配置文件的第一个刷新率配置节点

    while (refreshRatesConfig) {
        int refreshRate = std::stoi(refreshRatesConfig->string);
        if (closestRefreshRate == -1 ||
            std::abs(actualRefreshRate - refreshRate) < std::abs(actualRefreshRate - closestRefreshRate)) {
            closestRefreshRate = refreshRate;
            closestRateConfig = refreshRatesConfig;
        }
        refreshRatesConfig = refreshRatesConfig->next;
    }

    if (!closestRateConfig) {
        std::cerr << "错误：配置文件中未找到任何刷新率配置。" << std::endl;
        cJSON_Delete(configJson);
        return 1;
    }

    // 获取亮度级别配置
    cJSON* brightnessArray = cJSON_GetObjectItemCaseSensitive(closestRateConfig, "brightness_levels");
    if (!brightnessArray || !cJSON_IsArray(brightnessArray)) {
        std::cerr << "错误：在刷新率 " << closestRefreshRate << "Hz 下未找到有效的亮度级别配置。" << std::endl;
        cJSON_Delete(configJson);
        return 1;
    }

    // 主循环
    while (true) {
        int brightness;
        try {
            brightness = getSystemSetting("screen_brightness");
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
            break;
        }

        // 合理性检查
        if (brightness < 0 || brightness > 255) {
            std::cerr << "警告：亮度值异常 (" << brightness << "), 使用默认值 255。" << std::endl;
            brightness = 255;
        }

        // 检查亮度是否变化
        if (brightness == lastBrightness) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            continue;
        }

        int brightnessPercentage = static_cast<int>(round(brightness / 255.0 * 100.0));
        Color interpolatedColor = getColorForBrightness(brightnessArray, brightnessPercentage);
        setKcal(interpolatedColor.red, interpolatedColor.green, interpolatedColor.blue);

        // 更新上次亮度
        lastBrightness = brightness;

        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }

    cJSON_Delete(configJson);
    return 0;
}
