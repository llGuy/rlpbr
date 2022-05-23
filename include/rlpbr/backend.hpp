#pragma once

#include <rlpbr/fwd.hpp>
#include <rlpbr/utils.hpp>

#include <glm/glm.hpp>

#include <cuda_fp16.h>

#include <memory>
#include <string_view>

namespace RLpbr {

class EnvironmentImpl {
public:
    typedef void(*DestroyType)(EnvironmentBackend *);
    typedef uint32_t(EnvironmentBackend::*AddLightType)(
        const glm::vec3 &, const glm::vec3 &);
    typedef void(EnvironmentBackend::*RemoveLightType)(uint32_t);

    EnvironmentImpl(DestroyType destroy_ptr, AddLightType add_light_ptr,
                    RemoveLightType remove_light_ptr,
                    EnvironmentBackend *state);
    EnvironmentImpl(const EnvironmentImpl &) = delete;
    EnvironmentImpl(EnvironmentImpl &&);

    EnvironmentImpl & operator=(const EnvironmentImpl &) = delete;
    EnvironmentImpl & operator=(EnvironmentImpl &&);

    ~EnvironmentImpl();

    inline uint32_t addLight(const glm::vec3 &position,
                             const glm::vec3 &color);
    inline void removeLight(uint32_t idx);

    inline EnvironmentBackend *getState() { return state_; };
    inline const EnvironmentBackend *getState() const  { return state_; };

private:
    DestroyType destroy_ptr_;
    AddLightType add_light_ptr_;
    RemoveLightType remove_light_ptr_;
    EnvironmentBackend *state_;
};

class BakerImpl {
public:
    typedef void(*DestroyType)(BakerBackend *);
    typedef void(BakerBackend::*InitType)();
    typedef void(BakerBackend::*BakeType)(RenderBatch &);

    BakerImpl(DestroyType destroy_ptr, InitType init_ptr,
              BakeType bake_ptr, BakerBackend *state);
    BakerImpl(const BakerImpl &) = delete;
    BakerImpl(BakerImpl &&);

    BakerImpl & operator=(const BakerImpl &) = delete;
    BakerImpl & operator=(BakerImpl &&);

    ~BakerImpl();

private:
    DestroyType destroy_ptr_;
    InitType init_ptr_;
    BakeType bake_ptr_;
    BakerBackend *state_;
};

class LoaderImpl {
public:
    typedef void(*DestroyType)(LoaderBackend *);
    typedef std::shared_ptr<Scene>(LoaderBackend::*LoadSceneType)(
        SceneLoadData &&);
    typedef std::shared_ptr<EnvironmentMapGroup>(LoaderBackend::*LoadEnvMapsType)(
        const char **, uint32_t);

    LoaderImpl(DestroyType destroy_ptr, LoadSceneType load_scene_ptr,
               LoadEnvMapsType load_env_maps_ptr, LoaderBackend *state);
    LoaderImpl(const LoaderImpl &) = delete;
    LoaderImpl(LoaderImpl &&);

    LoaderImpl & operator=(const LoaderImpl &) = delete;
    LoaderImpl & operator=(LoaderImpl &&);

    ~LoaderImpl();

    inline std::shared_ptr<Scene> loadScene(SceneLoadData &&scene_data);

    inline std::shared_ptr<EnvironmentMapGroup> loadEnvironmentMaps(
        const char **paths, uint32_t num_maps);

private:
    DestroyType destroy_ptr_;
    LoadSceneType load_scene_ptr_;
    LoadEnvMapsType load_env_maps_ptr_;
    LoaderBackend *state_;
};

struct AuxiliaryOutputs {
    half *normal;
    half *albedo;
};

class RendererImpl {
public:
    typedef void(*DestroyType)(RenderBackend *);
    typedef LoaderImpl(RenderBackend::*MakeLoaderType)();
    typedef EnvironmentImpl(RenderBackend::*MakeEnvironmentType)(
        const std::shared_ptr<Scene> &, const Camera &);
    typedef void(RenderBackend::*SetEnvMapsType)(
        std::shared_ptr<EnvironmentMapGroup>);
    typedef std::unique_ptr<BatchBackend, BatchDeleter>(
        RenderBackend::*MakeBatchType)();
    typedef void(RenderBackend::*RenderType)(RenderBatch &batch);
    typedef void(RenderBackend::*BakeType)(RenderBatch &batch);
    typedef void(RenderBackend::*WaitType)(RenderBatch &batch);
    typedef half *(RenderBackend::*GetOutputType)(RenderBatch &batch);
    typedef AuxiliaryOutputs(RenderBackend::*GetAuxType)(RenderBatch &batch);
    typedef BakerImpl(RenderBackend::*MakeBakerType)();
    typedef half *(RenderBackend::*GetBakeOutputPointer)();

    RendererImpl(DestroyType destroy_ptr,
        MakeBakerType make_baker_ptr,
        MakeLoaderType make_loader_ptr, MakeEnvironmentType make_env_ptr,
        SetEnvMapsType set_env_maps_ptr_, MakeBatchType make_batch_ptr,
        RenderType render_ptr, BakeType bake_ptr, WaitType wait_ptr,
        GetOutputType get_output_ptr, GetBakeOutputPointer get_bake_output_ptr, GetAuxType get_aux_ptr,
        RenderBackend *state);
    RendererImpl(const RendererImpl &) = delete;
    RendererImpl(RendererImpl &&);

    RendererImpl & operator=(const RendererImpl &) = delete;
    RendererImpl & operator=(RendererImpl &&);

    ~RendererImpl();

    inline LoaderImpl makeLoader();

    inline EnvironmentImpl makeEnvironment(
        const std::shared_ptr<Scene> &scene, const Camera &) const;

    inline BakerImpl makeBaker();

    inline void setActiveEnvironmentMaps(
        std::shared_ptr<EnvironmentMapGroup> env_maps);

    inline void render(RenderBatch &batch);
    inline void bake(RenderBatch &batch);
    inline std::unique_ptr<BatchBackend, BatchDeleter> makeRenderBatch() const;

    inline void waitForBatch(RenderBatch &batch);

    inline half *getOutputPointer(RenderBatch &batch) const;
    inline half *getBakeOutputPointer();

    inline AuxiliaryOutputs getAuxiliaryOutputs(RenderBatch &batch) const;

private:
    DestroyType destroy_ptr_;
    MakeBakerType make_baker_ptr_;
    MakeLoaderType make_loader_ptr_;
    MakeEnvironmentType make_env_ptr_;
    SetEnvMapsType set_env_maps_ptr_;
    MakeBatchType make_batch_ptr_;
    RenderType render_ptr_;
    BakeType bake_ptr_;
    WaitType wait_ptr_;
    GetOutputType get_output_ptr_;
    GetBakeOutputPointer get_bake_output_ptr_;
    GetAuxType get_aux_ptr_;
    RenderBackend *state_;
};

}
