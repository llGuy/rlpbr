#pragma once

#include <rlpbr/config.hpp>
#include <rlpbr_core/scene.hpp>

#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <random>

#include "descriptors.hpp"
#include "utils.hpp"
#include "core.hpp"
#include "memory.hpp"
#include "shader.hpp"

// Forward declare ktxTexture as kind of an opaque backing data type
struct ktxTexture;

namespace RLpbr {
namespace vk {

struct VulkanScene;

struct BLAS {
    VkAccelerationStructureKHR hdl;
    VkDeviceAddress devAddr;
};

struct BLASData {
public:
    BLASData(const DeviceState &dev, std::vector<BLAS> &&as,
             LocalBuffer &&buf);
    BLASData(const BLASData &) = delete;
    BLASData(BLASData &&o);
    ~BLASData();

    BLASData &operator=(const BLASData &) = delete;
    BLASData &operator=(BLASData &&o);

    const DeviceState *dev;
    std::vector<BLAS> accelStructs;
    LocalBuffer storage;
};

struct TLAS {
    VkAccelerationStructureKHR hdl;
    std::optional<HostBuffer> buildStorage;
    uint32_t numBuildInstances;

    std::optional<LocalBuffer> tlasStorage;
    VkDeviceAddress tlasStorageDevAddr;
    size_t numStorageBytes;

    void build(const DeviceState &dev,
               MemoryAllocator &alloc,
               const std::vector<ObjectInstance> &instances,
               const std::vector<InstanceTransform> &instance_transforms,
               const std::vector<InstanceFlags> &instance_flags,
               const std::vector<ObjectInfo> &objects,
               const BLASData &blases,
               VkCommandBuffer build_cmd);

    void free(const DeviceState &dev);
};

struct ReservoirGrid {
    AABB bbox;
    VkDeviceMemory storage;
    VkDeviceAddress devAddr;
    LocalBuffer grid;
};

struct DomainRandomization {
    glm::quat envRotation;
    glm::vec3 lightFilter;
    uint32_t envMapIdx;
};

struct VulkanEnvironment : public EnvironmentBackend {
    VulkanEnvironment(const DeviceState &dev,
                      const VulkanScene &scene,
                      const Camera &cam,
                      std::mt19937 &rand_gen,
                      bool should_randomize,
                      uint32_t num_env_maps);
    VulkanEnvironment(const VulkanEnvironment &) = delete;
    ~VulkanEnvironment();

    uint32_t addLight(const glm::vec3 &position, const glm::vec3 &color);

    void removeLight(uint32_t light_idx);

    std::vector<PackedLight> lights;

    const DeviceState &dev;
    TLAS tlas;

    Camera prevCam;

    DomainRandomization domainRandomization;
};

struct TextureData {
    TextureData(const DeviceState &d, MemoryAllocator &a);
    TextureData(const TextureData &) = delete;
    TextureData(TextureData &&);
    ~TextureData();

    const DeviceState &dev;
    MemoryAllocator &alloc;

    VkDeviceMemory memory;
    std::vector<LocalTexture> textures;
    std::vector<VkImageView> views;
};

struct VulkanEnvMapGroup : public EnvironmentMapGroup {
    TextureData texData;
    DescriptorSet descSet;
};

struct SharedSceneState {
    SharedSceneState(const DeviceState &dev,
                     VkDescriptorPool scene_pool,
                     VkDescriptorSetLayout scene_layout,
                     MemoryAllocator &alloc);

    std::mutex lock;

    VkDescriptorSet descSet;
    HostBuffer addrData;

    std::vector<uint32_t> freeSceneIDs;
    uint32_t numSceneIDs;
};

class SharedEnvMapState {
public:
    SharedEnvMapState(const DeviceState &dev,
                      const ShaderPipeline &rt_shader, uint32_t env_set_id);
    SharedEnvMapState(const SharedEnvMapState &) = delete;
    SharedEnvMapState(SharedEnvMapState &&) = delete;

    DescriptorSet getSet();
private:

    std::mutex lock_;
    DescriptorManager desc_mgr_;
};

class SceneID {
public:
    SceneID(SharedSceneState &shared);
    SceneID(const SceneID &) = delete;
    SceneID(SceneID &&o);
    ~SceneID();

    uint32_t getID() const { return id_; }
private:
    SharedSceneState *shared_;
    uint32_t id_;
};

struct VulkanScene : public Scene {
    TextureData textures;

    LocalBuffer data;
    VkDeviceSize indexOffset;
    uint32_t numMeshes;
    std::optional<SceneID> sceneID;

    BLASData blases;
};

class VulkanLoader : public LoaderBackend {
public:
    VulkanLoader(const DeviceState &dev,
                 MemoryAllocator &alloc,
                 const QueueState &transfer_queue,
                 const QueueState &render_queue,
                 SharedSceneState &shared_scene_state,
                 SharedEnvMapState *env_map_state,
                 uint32_t render_qf,
                 uint32_t max_texture_resolution);

    VulkanLoader(const DeviceState &dev,
                 MemoryAllocator &alloc,
                 const QueueState &transfer_queue,
                 const QueueState &render_queue,
                 VkDescriptorSet scene_set,
                 SharedEnvMapState *env_map_state,
                 uint32_t render_qf,
                 uint32_t max_texture_resolution);

    std::shared_ptr<Scene> loadScene(SceneLoadData &&load_info);

    std::shared_ptr<EnvironmentMapGroup> loadEnvironmentMaps(
        const char **paths, uint32_t num_paths);

private:
    VulkanLoader(const DeviceState &dev,
                 MemoryAllocator &alloc,
                 const QueueState &transfer_queue,
                 const QueueState &render_queue,
                 SharedSceneState *shared_scene_state,
                 VkDescriptorSet scene_set,
                 SharedEnvMapState *env_map_state,
                 uint32_t render_qf,
                 uint32_t max_texture_resolution);

    const DeviceState &dev;
    MemoryAllocator &alloc;
    const QueueState &transfer_queue_;
    const QueueState &render_queue_;
    SharedSceneState *shared_scene_state_;
    VkDescriptorSet scene_set_;

    SharedEnvMapState *env_map_state_;

    VkCommandPool transfer_cmd_pool_;
    VkCommandBuffer transfer_cmd_;
    VkCommandPool render_cmd_pool_;
    VkCommandBuffer render_cmd_;

    VkSemaphore transfer_sema_;
    VkFence fence_;

    uint32_t max_queries_;
    VkQueryPool compacted_query_pool_;
    VkQueryPool serialized_query_pool_;

    uint32_t render_qf_;
    uint32_t max_texture_resolution_;
};

// For now the baker only bakes one environment
class VulkanBaker : public BakerBackend {
public:
    VulkanBaker(
        const DeviceState &dev,
        MemoryAllocator &alloc,
        const QueueState &transfer_queue,
        const QueueState &render_queue,
        VkPipeline bake_pipeline,
        VkPipelineLayout bake_pipeline_layout);

    void init();
    void bake(RenderBatch &batch);

private:
    const DeviceState &dev_;
    MemoryAllocator &alloc_;
    const QueueState &transfer_queue_;
    const QueueState &render_queue_;
    VkPipeline bake_pipeline_;
    VkPipelineLayout bake_pipeline_layout_;

    // Current output
    half *output_;
};

}
}
