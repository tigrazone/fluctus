#include <iostream>
#include <fstream>
#include <math/matrix.hpp>
#include "settings.hpp"

using json = nlohmann::json;

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
}

inline bool contains(json j, std::string value)
{
    return j.find(value) != j.end();
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

    if(!contains(j, "release") || !contains(j, "debug"))
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
    if (contains(j, "platformName")) this->platformName = j["platformName"].get<std::string>();
    if (contains(j, "deviceName")) this->deviceName = j["deviceName"].get<std::string>();
    if (contains(j, "envMap")) this->envMapName = j["envMap"].get<std::string>();
    if (contains(j, "renderScale")) this->renderScale = j["renderScale"].get<float>();
    if (contains(j, "windowWidth")) this->windowWidth = j["windowWidth"].get<int>();
    if (contains(j, "windowHeight")) this->windowHeight = j["windowHeight"].get<int>();
    if (contains(j, "clUseBitstack")) this->clUseBitstack = j["clUseBitstack"].get<bool>();
    if (contains(j, "clUseSoA")) this->clUseSoA = j["clUseSoA"].get<bool>();
    if (contains(j, "wfBufferSize")) this->wfBufferSize = j["wfBufferSize"].get<unsigned int>();
    if (contains(j, "useWavefront")) this->useWavefront = j["useWavefront"].get<bool>();
    if (contains(j, "useRussianRoulette")) this->useRussianRoulette = j["useRussianRoulette"].get<bool>();
    if (contains(j, "useSeparateQueues")) this->useSeparateQueues = j["useSeparateQueues"].get<bool>();
    if (contains(j, "maxPathDepth")) this->maxPathDepth = j["maxPathDepth"].get<int>();
    if (contains(j, "tonemap")) this->tonemap = j["tonemap"].get<int>();

    if (contains(j, "camera"))
    {
        json map = j["camera"];
        if (contains(map, "pos"))
        {
            const auto values = map["pos"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                cameraSettings.pos = vfloat3(values[0], values[1], values[2]);
            }
        }
        if (contains(map, "dir"))
        {
            const auto values = map["dir"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                cameraSettings.dir = vfloat3(values[0], values[1], values[2]);
                calculateCameraRotation();
            }
        }
        // this overrides dir if existent
        if (contains(map, "lookAt"))
        {
            const auto values = map["lookAt"].get<std::vector<float>>();
            if (values.size() == 3)
            {
                const vfloat3 dir = (vfloat3(values[0], values[1], values[2]) - cameraSettings.pos);
                if (dir.sqnorm() > 1E-3) {
                    cameraSettings.dir = dir;
                    cameraSettings.dir.normalize();
                    calculateCameraRotation();
                }
            }
        }

        if (contains(map, "fov")) this->cameraSettings.fov = cl_float(map["fov"].get<float>());
        if (contains(map, "apertureSize")) this->cameraSettings.apertureSize = cl_float(map["apertureSize"].get<float>());
        if (contains(map, "focalDist")) this->cameraSettings.focalDist = cl_float(map["focalDist"].get<float>());

        if (contains(map, "cameraRotation"))
        {
            const auto values = map["cameraRotation"].get<std::vector<float>>();
            if (values.size() == 2)
            {
                cameraSettings.cameraRotation = vfloat2(values[0], values[1]);
            }
        }

        calculateCameraMatrix();

        if (contains(map, "cameraSpeed")) this->cameraSettings.cameraSpeed = map["cameraSpeed"].get<float>();
    }

    // Map of numbers 1-5 to scenes (shortcuts)
    if (contains(j, "shortcuts"))
    {
        json map = j["shortcuts"];
        for (unsigned int i = 1; i < 6; i++)
        {
            std::string numeral = std::to_string(i);
            if (contains(map, numeral)) this->shortcuts[i] = map[numeral].get<std::string>();
        }
    }
}

void Settings::calculateCameraRotation()
{
    const vfloat3& dir = cameraSettings.dir;
    cameraSettings.cameraRotation.x = toDeg(std::atan2(dir.x, -dir.z));
    cameraSettings.cameraRotation.y = -toDeg(std::atan2(dir.y, std::sqrt(1 - dir.y * dir.y)));
}

void Settings::calculateCameraMatrix()
{
    const FireRays::matrix rot = rotation(VEC_RIGHT, toRad(cameraSettings.cameraRotation.y)) * rotation(VEC_UP, toRad(cameraSettings.cameraRotation.x));

    cameraSettings.right = vfloat3(rot.m00, rot.m01, rot.m02);
    cameraSettings.up = vfloat3(rot.m10, rot.m11, rot.m12);
    cameraSettings.dir = -vfloat3(rot.m20, rot.m21, rot.m22); // camera points in the negative z-direction
}

#undef VEC_UP
#undef VEC_RIGHT

