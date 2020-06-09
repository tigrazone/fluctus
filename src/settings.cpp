#include <iostream>
#include <fstream>
#include <math/matrix.hpp>
#include "settings.hpp"

#include "utils.h"

#define VEC_RIGHT vfloat3(1.0f, 0.0f, 0.0f)
#define VEC_UP vfloat3(0.0f, 1.0f, 0.0f)

Settings::Settings()
{
    init();
    load();
}

void Settings::init()
{
    platformName = "";
    deviceName = "";
    envMapName = "";
    defaultScene = 0;
    renderScale = 1.0f;
    windowWidth = 640;
    windowHeight = 480;
    wfBufferSize = 1 << 20; // appropriate for dedicated GPU
    clUseBitstack = false;
    clUseSoA = true;
    useWavefront = false;
    useRussianRoulette = false;
    useSeparateQueues = false;
    maxPathDepth = 10;
    maxSpp = 0; // 0 = no limit
    maxRenderTime = 0; // 0 = no limit
    tonemap = 2; // UC2 default
    cameraSettings = {
        vfloat3(0.0f, 1.0f, 3.5f),
        VEC_RIGHT,
        VEC_UP,
        vfloat3(0.0f, 0.0f, -1.0f),
        cl_float(60.f),
        cl_float(0.0f),
        cl_float(0.5f),
        vfloat2(0.0f),
        1.0f
    };
    areaLightSettings = {
        FireRays::float3(0.0f, 0.0f, -1.0f),
        FireRays::float3(0.0f, 1.0f, 0.0f),
        FireRays::float4(-1.0f, 0.0f, 0.0f, 0.0f),
        FireRays::float4(1.0f, 1.0f, 0.0f, 1.0f),
        FireRays::float3(1.0f, 1.0f, 1.0f) * 100.f,
        FireRays::float2(0.5f, 0.5f)
    };
}

void Settings::load()
{
    std::ifstream i("settings.json");

    if(!i.good())
    {
        std::cout << "Settings file not found!" << std::endl;
        return;
    }

    json j;
    i >> j;

    if(!json_contains(j, "release") || !json_contains(j, "debug"))
    {
        std::cout << R"(Settings file must contain the objects "release" and "debug")" << std::endl;
        return;
    }

    // Read release settings first
    import(j["release"]);

#ifdef _DEBUG
    // Override with debug settings in debug mode
    import(j["debug"]);
#endif
}

void Settings::import(json j)
{
    if (json_contains(j, "platformName")) this->platformName = j["platformName"].get<std::string>();
    if (json_contains(j, "deviceName")) this->deviceName = j["deviceName"].get<std::string>();
    if (json_contains(j, "envMap")) this->envMapName = j["envMap"].get<std::string>();
    if (json_contains(j, "renderScale")) this->renderScale = j["renderScale"].get<float>();
    if (json_contains(j, "windowWidth")) this->windowWidth = j["windowWidth"].get<int>();
    if (json_contains(j, "windowHeight")) this->windowHeight = j["windowHeight"].get<int>();
    if (json_contains(j, "clUseBitstack")) this->clUseBitstack = j["clUseBitstack"].get<bool>();
    if (json_contains(j, "clUseSoA")) this->clUseSoA = j["clUseSoA"].get<bool>();
    if (json_contains(j, "wfBufferSize")) this->wfBufferSize = j["wfBufferSize"].get<unsigned int>();
    if (json_contains(j, "useWavefront")) this->useWavefront = j["useWavefront"].get<bool>();
    if (json_contains(j, "useRussianRoulette")) this->useRussianRoulette = j["useRussianRoulette"].get<bool>();
    if (json_contains(j, "useSeparateQueues")) this->useSeparateQueues = j["useSeparateQueues"].get<bool>();
    if (json_contains(j, "maxPathDepth")) this->maxPathDepth = j["maxPathDepth"].get<int>();
    if (json_contains(j, "maxSpp")) this->maxSpp = j["maxSpp"].get<unsigned int>();
    if (json_contains(j, "maxRenderTime")) this->maxRenderTime = j["maxRenderTime"].get<unsigned int>();
    if (json_contains(j, "tonemap")) this->tonemap = j["tonemap"].get<int>();

    // Map of numbers 1-6 to scenes (shortcuts)
    if (json_contains(j, "shortcuts"))
    {
        json map = j["shortcuts"];
        for (unsigned int i = 1; i < 7; i++)
        {
            std::string numeral = std::to_string(i);
            if (json_contains(map, numeral)) this->shortcuts[i] = map[numeral].get<std::string>();
        }
    }
    if (json_contains(j, "defaultScene"))
    {
        unsigned int defaultSceneNumber = j["defaultScene"].get<unsigned int>();
        auto scenePair = this->shortcuts.find(defaultSceneNumber);
        if(scenePair != this->shortcuts.end())
        {
            this->defaultScene = distance(this->shortcuts.begin(), scenePair);
        }
    }

    if (json_contains(j, "camera"))
    {
        json map = j["camera"];
        if (json_contains(map, "pos"))
        {
            const auto values = map["pos"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                cameraSettings.pos = vfloat3(values[0], values[1], values[2]);
            }
        }
        if (json_contains(map, "dir"))
        {
            const auto values = map["dir"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                const vfloat3 dir = vfloat3(values[0], values[1], values[2]);
                if (dir.sqnorm() > 1E-3) {
                    cameraSettings.dir = dir;
                    calculateCameraRotation();
                }
            }
        }
        // this overrides dir if existent
        if (json_contains(map, "lookAt"))
        {
            const auto values = map["lookAt"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                const vfloat3 dir = vfloat3(values[0], values[1], values[2]) - cameraSettings.pos;
                if (dir.sqnorm() > 1E-3) {
                    cameraSettings.dir = dir;
                    calculateCameraRotation();
                }
            }
        }

        if (json_contains(map, "fov")) this->cameraSettings.fov = cl_float(map["fov"].get<float>());
        if (json_contains(map, "apertureSize")) this->cameraSettings.apertureSize = cl_float(map["apertureSize"].get<float>());
        if (json_contains(map, "focalDist")) this->cameraSettings.focalDist = cl_float(map["focalDist"].get<float>());

        if (json_contains(map, "cameraRotation"))
        {
            const auto values = map["cameraRotation"].get<std::vector<float>>();
            if (values.size() == 2)
            {
                cameraSettings.cameraRotation = vfloat2(values[0], values[1]);
            }
        }

        calculateCameraMatrix();

        if (json_contains(map, "cameraSpeed")) this->cameraSettings.cameraSpeed = map["cameraSpeed"].get<float>();
    }

    if (json_contains(j, "areaLight"))
    {
        json map = j["areaLight"];
        if (json_contains(map, "pos"))
        {
            const auto values = map["pos"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                areaLightSettings.pos = FireRays::float4(values[0], values[1], values[2], 1.0f);
            }
        }
        if (json_contains(map, "N"))
        {
            const auto values = map["N"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                areaLightSettings.N = FireRays::float4(values[0], values[1], values[2], 0.0f);
                areaLightSettings.right = cross(areaLightSettings.N, VEC_UP);
                if (areaLightSettings.right.sqnorm() < 1E-6)
                {
                    // N and Up are multiples of each other --> use the known right vector
                    areaLightSettings.right = VEC_RIGHT * dot(areaLightSettings.N, VEC_UP);
                }
                areaLightSettings.up = cross(areaLightSettings.right, areaLightSettings.N);

                areaLightSettings.N.normalize();
                areaLightSettings.right.normalize();
                areaLightSettings.up.normalize();
            }
        }
        if (json_contains(map, "E"))
        {
            const auto values = map["E"].get<std::vector<float>>();
            switch (values.size()) {
            case 1:
                areaLightSettings.E = vfloat3(values[0]);
                areaLightSettings.E.w = 0.f;
                break;
            case 3:
                areaLightSettings.E = vfloat3(values[0], values[1], values[2]);
                break;
            default:
                break;
            }
        }
        if (json_contains(map, "size"))
        {
            const auto values = map["size"].get<std::vector<float>>();
            switch (values.size()) {
            case 1:
                areaLightSettings.size = vfloat2(values[0]);
                break;
            case 2:
                areaLightSettings.size = vfloat2(values[0], values[1]);
                break;
            default:
                break;
            }
        }
    }
}

void Settings::calculateCameraRotation()
{
    vfloat3& dir = cameraSettings.dir;
    dir.normalize();
    cameraSettings.cameraRotation.x = toDeg(std::atan2(dir.x, -dir.z));
    cameraSettings.cameraRotation.y = -toDeg(std::asin(dir.y));
}

void Settings::calculateCameraMatrix()
{
    const FireRays::matrix rot = rotation(VEC_RIGHT, toRad(cameraSettings.cameraRotation.y)) * rotation(VEC_UP, toRad(cameraSettings.cameraRotation.x));

    cameraSettings.right = vfloat3(rot.m00, rot.m01, rot.m02);
    cameraSettings.up = vfloat3(rot.m10, rot.m11, rot.m12);
    cameraSettings.dir = -vfloat3(rot.m20, rot.m21, rot.m22);
}

#undef VEC_UP
#undef VEC_RIGHT

