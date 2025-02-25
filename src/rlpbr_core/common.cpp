#include "common.hpp"
#include "scene.hpp"

#include <functional>

using namespace std;

namespace RLpbr {

EnvironmentImpl::EnvironmentImpl(
    DestroyType destroy_ptr, AddLightType add_light_ptr,
    RemoveLightType remove_light_ptr,
    EnvironmentBackend *state)
    : destroy_ptr_(destroy_ptr),
      add_light_ptr_(add_light_ptr),
      remove_light_ptr_(remove_light_ptr),
      state_(state)
{}

EnvironmentImpl::EnvironmentImpl(EnvironmentImpl &&o)
    : destroy_ptr_(o.destroy_ptr_),
      add_light_ptr_(o.add_light_ptr_),
      remove_light_ptr_(o.remove_light_ptr_),
      state_(o.state_)
{
    o.state_ = nullptr;
}

EnvironmentImpl::~EnvironmentImpl()
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }
}

EnvironmentImpl & EnvironmentImpl::operator=(EnvironmentImpl &&o)
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }

    destroy_ptr_ = o.destroy_ptr_;
    add_light_ptr_ = o.add_light_ptr_;
    remove_light_ptr_ = o.remove_light_ptr_;
    state_ = o.state_;

    o.state_ = nullptr;

    return *this;
}

BakerImpl::BakerImpl(DestroyType destroy_ptr, InitType init_ptr,
                     BakeType bake_ptr, BakerBackend *state)
    : destroy_ptr_(destroy_ptr),
      init_ptr_(init_ptr),
      bake_ptr_(bake_ptr),
      state_(state)
{
    
}

LoaderImpl::LoaderImpl(DestroyType destroy_ptr,
                       LoadSceneType load_scene_ptr,
                       LoadEnvMapsType load_env_maps_ptr,
                       LoaderBackend *state)
    : destroy_ptr_(destroy_ptr),
      load_scene_ptr_(load_scene_ptr),
      load_env_maps_ptr_(load_env_maps_ptr),
      state_(state)
{}

LoaderImpl::LoaderImpl(LoaderImpl &&o)
    : destroy_ptr_(o.destroy_ptr_),
      load_scene_ptr_(o.load_scene_ptr_),
      load_env_maps_ptr_(o.load_env_maps_ptr_),
      state_(o.state_)
{
    o.state_ = nullptr;
}

LoaderImpl::~LoaderImpl()
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }
}

LoaderImpl & LoaderImpl::operator=(LoaderImpl &&o)
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }

    destroy_ptr_ = o.destroy_ptr_;
    load_scene_ptr_ = o.load_scene_ptr_;
    state_ = o.state_;

    o.state_ = nullptr;

    return *this;
}

RendererImpl::RendererImpl(DestroyType destroy_ptr,
                           MakeBakerType make_baker_ptr,
                           MakeLoaderType make_loader_ptr,
                           MakeEnvironmentType make_env_ptr,
                           SetEnvMapsType set_env_maps_ptr,
                           MakeBatchType make_batch_ptr,
                           RenderType render_ptr,
                           BakeType bake_ptr,
                           WaitType wait_ptr,
                           GetOutputType get_output_ptr,
                           GetBakeOutputPointer get_bake_output_ptr,
                           GetAuxType get_aux_ptr,
                           RenderBackend *state)
    : destroy_ptr_(destroy_ptr),
      make_baker_ptr_(make_baker_ptr),
      make_loader_ptr_(make_loader_ptr),
      make_env_ptr_(make_env_ptr),
      set_env_maps_ptr_(set_env_maps_ptr),
      make_batch_ptr_(make_batch_ptr),
      render_ptr_(render_ptr),
      bake_ptr_(bake_ptr),
      wait_ptr_(wait_ptr),
      get_output_ptr_(get_output_ptr),
      get_bake_output_ptr_(get_bake_output_ptr),
      get_aux_ptr_(get_aux_ptr),
      state_(state)
{}

RendererImpl::RendererImpl(RendererImpl &&o)
    : destroy_ptr_(o.destroy_ptr_),
      make_loader_ptr_(o.make_loader_ptr_),
      make_env_ptr_(o.make_env_ptr_),
      set_env_maps_ptr_(o.set_env_maps_ptr_),
      render_ptr_(o.render_ptr_),
      bake_ptr_(o.bake_ptr_),
      wait_ptr_(o.wait_ptr_),
      get_output_ptr_(o.get_output_ptr_),
      get_bake_output_ptr_(o.get_bake_output_ptr_),
      get_aux_ptr_(o.get_aux_ptr_),
      state_(o.state_)
{
    o.state_ = nullptr;
}

RendererImpl::~RendererImpl()
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }
}

RendererImpl & RendererImpl::operator=(RendererImpl &&o)
{
    if (state_) {
        invoke(destroy_ptr_, state_);
    }

    destroy_ptr_ = o.destroy_ptr_;
    make_loader_ptr_ = o.make_loader_ptr_;
    make_env_ptr_ = o.make_env_ptr_;
    render_ptr_ = o.render_ptr_;
    wait_ptr_ = o.wait_ptr_;
    get_output_ptr_ = o.get_output_ptr_;
    get_aux_ptr_ = o.get_aux_ptr_;
    state_ = o.state_;

    o.state_ = nullptr;

    return *this;
}

}
