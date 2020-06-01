#pragma once

// Suppress sscanf warning on MSVCCompiler
#if defined (_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <string>
#include <vector>
#include <array>
#include <memory>
#include "texture.hpp"
#include "envmap.hpp"
#include "triangle.hpp"
#include "geom.h"

namespace fr = FireRays;

class ProgressView;

struct ModelTransform
{
    // for now we only support scaling all axis by the same value
    float scale = 1;
    fr::float3 translation = fr::float3(0);
    // no rotation

    fr::float3 apply(const fr::float3 p) const
    {
        return p * scale + translation;
    }
};

class Scene {
public:
    Scene();
    ~Scene();

    void loadEnvMap(const std::string filename);
    void setEnvMap(std::shared_ptr<EnvironmentMap> envMapPtr);
    void loadModel(const std::string filename, ProgressView *progress, ModelTransform* transform = nullptr); // load .obj or .ply model

    std::vector<RTTriangle> &getTriangles() { return triangles; }
    std::vector<Material> &getMaterials() { return materials; }
    std::vector<Texture*> &getTextures() { return textures; }
    std::shared_ptr<EnvironmentMap> getEnvMap() { return envmap; }

    std::string hashString();
    unsigned int getMaterialTypes() { return materialTypes; }

    fr::float3 getWorldRight() { return worldRight; }
    fr::float3 getWorldUp() { return worldUp; }

private:
    void loadObjModel(const std::string filename, ModelTransform* transform);
    void loadPlyModel(const std::string filename, ModelTransform* transform);

    void loadPBRTModel(const std::string filename);
    void convertPBRTModel(const std::string filenameIn, const std::string filenameOut);
    void loadPBFModel(const std::string filename, ModelTransform* transform);
    void loadSceneFile(const std::string filename, ProgressView *progress);

    // With tiny_obj_loader
    void loadObjWithMaterials(const std::string filename, ProgressView *progress, ModelTransform* transform);
    cl_int tryImportTexture(const std::string path, const std::string name);
    cl_int parseShaderType(std::string type);

    void unpackIndexedData(const std::vector<fr::float3> &positions,
                           const std::vector<fr::float3>& normals,
                           const std::vector<std::array<unsigned, 6>>& faces,
                           bool type_ply,
                           ModelTransform* transform);

    std::shared_ptr<EnvironmentMap> envmap;
    std::vector<RTTriangle> triangles;
    std::vector<Material> materials;
    std::vector<Texture*> textures;
    size_t hash;
  
    fr::float3 worldRight = fr::float3(1.0f, 0.0f, 0.0f);
    fr::float3 worldUp = fr::float3(0.0f, 1.0f, 0.0f);

    unsigned int materialTypes = 0; // bits represent material types present in scene
};
