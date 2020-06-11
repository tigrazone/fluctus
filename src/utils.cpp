#include "utils.h"
#include "xxhash/xxhash.h"
#include "tinyfiledialogs.h"
#include <fstream>
#include <iostream>
#include <vector>
#if defined(_WIN32)
#include <direct.h>   // _mkdir
#endif


bool isAbsolutePath(const std::string filename)
{
#ifdef _WIN32
    return filename.find(':') != std::string::npos;
#else
    return filename.find('/') == 0;
#endif
}

std::string getAbsolutePath(const std::string filename)
{
    const int MAX_LENTH = 4096;
    char resolved_path[MAX_LENTH];
#ifdef _WIN32
    _fullpath(resolved_path, filename.c_str(), MAX_LENTH);
#else
    realpath(filename.c_str(), resolved_path);
#endif
    return std::string(resolved_path);
}

bool endsWith(const std::string s, const std::string end)
{
    size_t len = end.size();
    if (len > s.size()) return false;

    std::string substr = s.substr(s.size() - len, len);
    return end == substr;
}

bool endsWithAny(const std::string s, const std::vector<std::string> ends)
{
    for(const std::string end : ends)
    {
        if (endsWith(s, end))
        {
            return true;
        }
    }
    return false;
}

std::string unixifyPath(std::string path)
{
    size_t index = 0;
    while (true)
    {
        index = path.find("\\", index);
        if (index == std::string::npos) break;

        path.replace(index, 1, "/");
        index += 1;
    }

    return path;
}

bool json_contains(const json& j, std::string value)
{
    return j.find(value) != j.end();
}

std::string getFileName(const std::string path)
{
    const std::string upath = unixifyPath(path);
    size_t idx = path.find("/", 0) + 1;
    return upath.substr(idx, upath.length() - idx);
}

std::string getUnixFolderPath(const std::string path, const bool isFile)
{
    std::string unixPath = unixifyPath(path);
    if (isFile)
    {
        return unixPath.substr(0, unixPath.find_last_of('/') + 1);

    }
    return unixPath.find_last_of('/') == unixPath.size() - 1 ? unixPath : unixPath + "/";
}

bool createPath(const std::string& inpath)
{
    auto makeDirFun = [](const std::string& p)
    {
#if defined(_WIN32)
        return _mkdir(p.c_str());
#else
        return mkdir(p.c_str(), 0755);
#endif
    };

    std::string path = unixifyPath(inpath);
    if (makeDirFun(path) == -1)
    {
        switch (errno)
        {
        case ENOENT:
            if (createPath(path.substr(0, path.find_last_of('/'))))
                return (makeDirFun(path) == 0);
            else
                return false;
        case EEXIST:
            return true;
        default:
            return false;
        }
    }

    return true;
}

std::string openFileDialog(const std::string message, const std::string defaultPath, const std::vector<const char*> filter)
{
    char const *selected = tinyfd_openFileDialog(message.c_str(), defaultPath.c_str(), filter.size(), filter.data(), NULL, 0);
    return (selected) ? std::string(selected) : "";
}

std::string saveFileDialog(const std::string message, const std::string defaultPath, const std::vector<const char*> filter)
{
    char const *selected = tinyfd_saveFileDialog(message.c_str(), defaultPath.c_str(), filter.size(), filter.data(), NULL);
    return (selected) ? std::string(selected) : "";
}

size_t computeHash(const void* buffer, size_t length)
{
    const size_t seed = 0;
#ifdef ENVIRONMENT64
    size_t const hash = XXH64(buffer, length, seed);
#else
    size_t const hash = XXH32(buffer, length, seed);
#endif
    return hash;
}

size_t fileHash(const std::string filename)
{
    std::ifstream f(filename, std::ios::binary | std::ios::ate);

    if (!f)
    {
        std::cout << "Could not open file " << filename << " for hashing, exiting..." << std::endl;
        waitExit();
    }

    std::ifstream::pos_type pos = f.tellg();
    std::vector<char> data(pos);
    f.seekg(0, std::ios::beg);
    f.read(&data[0], pos);
    f.close();

    return computeHash((const void*)data.data(), (size_t)pos);
}

std::string getBxdfDefines(unsigned int typeBits)
{
    std::string defines = "";
    
    if (typeBits & BXDF_DIFFUSE)
        defines += " -DBXDF_USE_DIFFUSE";
    if (typeBits & BXDF_GLOSSY)
        defines += " -DBXDF_USE_GLOSSY";
    if (typeBits & BXDF_GGX_ROUGH_REFLECTION)
        defines += " -DBXDF_USE_GGX_ROUGH_REFLECTION";
    if (typeBits & BXDF_IDEAL_REFLECTION)
        defines += " -DBXDF_USE_IDEAL_REFLECTION";
    if (typeBits & BXDF_GGX_ROUGH_DIELECTRIC)
        defines += " -DBXDF_USE_GGX_ROUGH_DIELECTRIC";
    if (typeBits & BXDF_IDEAL_DIELECTRIC)
        defines += " -DBXDF_USE_IDEAL_DIELECTRIC";
    if (typeBits & BXDF_EMISSIVE)
        defines += " -DBXDF_USE_EMISSIVE";

    return defines;
}
