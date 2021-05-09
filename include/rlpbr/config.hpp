#pragma once

#include <glm/glm.hpp>

namespace RLpbr {

enum class BackendSelect {
    Optix,
};

struct RenderConfig {
    int gpuID;
    uint32_t numLoaders;
    uint32_t batchSize;
    uint32_t imgWidth;
    uint32_t imgHeight;
    uint32_t spp;
    uint32_t maxDepth;
    uint32_t maxTextureResolution;
    bool doubleBuffered;
    bool enablePhysics;
    bool auxiliaryOutputs;
    float clampThreshold;
    BackendSelect backend;
};

}
