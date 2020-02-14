#pragma once

#include <string>
#include <map>
#include "json.hpp"
#include "clcontext.hpp"

typedef struct {
    vfloat3 pos;
    vfloat3 right;
    vfloat3 up;
    vfloat3 dir;
    cl_float fov;
    cl_float apertureSize;
    cl_float focalDist;
    vfloat2 cameraRotation;
    float cameraSpeed;
} CameraSettings;

typedef struct {
    vfloat3 right;
    vfloat3 up;
    vfloat3 N;
    vfloat3 pos;
    vfloat3 E;
    vfloat2 size;
} AreaLightSettings;

class Settings
{
public:
    // Singleton pattern
    static Settings &getInstance() {
        static Settings instance;
        return instance;
    }
    Settings(Settings const&) = delete;
    void operator=(Settings const&) = delete;

    // Getters
    std::string getPlatformName() { return platformName; }
    std::string getDeviceName() { return deviceName; }
    std::string getEnvMapName() { return envMapName; }
    void setEnvMapName(const std::string name) { envMapName = name; }
    std::map<unsigned int, std::string> getShortcuts() { return shortcuts; }
    int getWindowWidth() { return windowWidth; }
    int getWindowHeight() { return windowHeight; }
    float getRenderScale() { return renderScale; }
    void setRenderScale(float s) { renderScale = s; }
    bool getUseBitstack() { return clUseBitstack; }
    bool getUseSoA() { return clUseSoA; }
    unsigned int getWfBufferSize() { return wfBufferSize; }
    bool getUseWavefront() { return useWavefront; }
    bool getUseRussianRoulette() { return useRussianRoulette; }
    bool getUseSeparateQueues() { return useSeparateQueues; }
    int getMaxPathDepth() { return maxPathDepth; }
    int getTonemap() { return tonemap; }
    CameraSettings getCameraSettings() { return cameraSettings; }
    AreaLightSettings getAreaLightSettings() { return areaLightSettings; }

private:
    Settings();
    void init();
    void load();
    void import(nlohmann::json j);
    void calculateCameraRotation();
    void calculateCameraMatrix();

    // Contents of settings singleton
    std::string platformName;
    std::string deviceName;
    std::string envMapName;
    std::map<unsigned int, std::string> shortcuts;
    unsigned int wfBufferSize;
    bool clUseBitstack;
    bool clUseSoA;
    int windowWidth;
    int windowHeight;
    float renderScale;
    bool useWavefront;
    bool useRussianRoulette;
    bool useSeparateQueues;
    int maxPathDepth;
    int tonemap;
    CameraSettings cameraSettings;
    AreaLightSettings areaLightSettings;
};
