#include "render.hpp"
#include <vulkan/vulkan_core.h>

#include "scene.hpp"

#include <iostream>
#include <sstream>
#include <glm/gtx/string_cast.hpp>
#include <cmath>
#include <filesystem>

int probeIdx = 0;

using namespace std;

namespace RLpbr {
namespace vk {

static uint32_t PROBE_WIDTH = 32;
static uint32_t PROBE_HEIGHT = 32;
static glm::ivec3 PROBE_DIM = glm::ivec3(3);
static uint32_t PROBE_COUNT = PROBE_DIM.x * PROBE_DIM.y * PROBE_DIM.z;

static InitConfig getInitConfig(const RenderConfig &cfg, bool validate)
{
    bool use_zsobol = (cfg.flags & RenderFlags::AdaptiveSample) ||
        uint64_t(cfg.spp) * uint64_t(cfg.imgWidth) * uint64_t(cfg.imgHeight) <
            uint64_t(~0u);

    bool need_present = false;
    char *fake_present = getenv("VK_FAKE_PRESENT");
    if (fake_present && fake_present[0] == '1') {
        need_present = true;
    }

    return InitConfig {
        use_zsobol,
        false,
        validate,
        need_present,
    };
}

static ParamBufferConfig getParamBufferConfig(uint32_t batch_size,
                                              const MemoryAllocator &alloc)
{
    ParamBufferConfig cfg {};

    cfg.totalTransformBytes =
        sizeof(InstanceTransform) * VulkanConfig::max_instances;

    VkDeviceSize cur_offset = cfg.totalTransformBytes;

    cfg.materialIndicesOffset = alloc.alignStorageBufferOffset(cur_offset);

    cfg.totalMaterialIndexBytes =
        sizeof(uint32_t) * VulkanConfig::max_instances;

    cur_offset = cfg.materialIndicesOffset + cfg.totalMaterialIndexBytes;

    cfg.lightsOffset = alloc.alignStorageBufferOffset(cur_offset);
    cfg.totalLightParamBytes = sizeof(PackedLight) * VulkanConfig::max_lights;

    cur_offset = cfg.lightsOffset + cfg.totalLightParamBytes;

    cfg.envOffset = alloc.alignStorageBufferOffset(cur_offset);
    cfg.totalEnvParamBytes = sizeof(PackedEnv) * batch_size;

    cur_offset = cfg.envOffset + cfg.totalEnvParamBytes;

    // Ensure that full block is aligned to maximum requirement
    cfg.totalParamBytes =  alloc.alignStorageBufferOffset(cur_offset);

    return cfg;
}

static FramebufferConfig getProbeFramebufferConfig(uint32_t width, uint32_t height)
{
    uint32_t pixels_per_batch = width * height;

    uint32_t output_bytes = 4 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t hdr_bytes = 4 * sizeof(float) * pixels_per_batch;
    uint32_t normal_bytes = 3 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t albedo_bytes = 3 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t reservoir_bytes = sizeof(Reservoir) * pixels_per_batch;

    FramebufferConfig config;
    config.imgWidth = width;
    config.imgHeight = height;
    config.miniBatchSize = 1;
    config.numImagesWidePerMiniBatch = 1;
    config.numImagesTallPerMiniBatch = 1;
    config.numImagesWidePerBatch = 1;
    config.numImagesTallPerBatch = 1;
    config.frameWidth = width;
    config.frameHeight = height;
    config.numTilesWide = 1;
    config.numTilesTall = 1;
    config.outputBytes = output_bytes;
    config.hdrBytes = hdr_bytes;
    config.normalBytes = normal_bytes;
    config.albedoBytes = albedo_bytes;
    config.reservoirBytes = reservoir_bytes;
    config.illuminanceBytes = sizeof(float);
    config.adaptiveBytes = sizeof(AdaptiveTile);

    return config;
}

static FramebufferConfig getFramebufferConfig(const RenderConfig &cfg)
{
    uint32_t batch_size = cfg.batchSize;

    uint32_t minibatch_size =
        max(batch_size / VulkanConfig::minibatch_divisor, batch_size);
    assert(batch_size % minibatch_size == 0);

    uint32_t batch_fb_images_wide = ceil(sqrt(batch_size));
    while (batch_size % batch_fb_images_wide != 0) {
        batch_fb_images_wide++;
    }

    uint32_t minibatch_fb_images_wide;
    uint32_t minibatch_fb_images_tall;
    if (batch_fb_images_wide >= minibatch_size) {
        assert(batch_fb_images_wide % minibatch_size == 0);
        minibatch_fb_images_wide = minibatch_size;
        minibatch_fb_images_tall = 1;
    } else {
        minibatch_fb_images_wide = batch_fb_images_wide;
        minibatch_fb_images_tall = minibatch_size / batch_fb_images_wide;
    }

    assert(minibatch_fb_images_wide * minibatch_fb_images_tall ==
           minibatch_size);

    uint32_t batch_fb_images_tall = (batch_size / batch_fb_images_wide);
    assert(batch_fb_images_wide * batch_fb_images_tall == batch_size);

    uint32_t batch_fb_width = cfg.imgWidth * batch_fb_images_wide;
    uint32_t batch_fb_height = cfg.imgHeight * batch_fb_images_tall;

    uint32_t pixels_per_batch = batch_fb_width * batch_fb_height;

    uint32_t output_bytes = 4 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t hdr_bytes = 4 * sizeof(float) * pixels_per_batch;
    uint32_t normal_bytes = 3 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t albedo_bytes = 3 * sizeof(uint16_t) * pixels_per_batch;
    uint32_t reservoir_bytes = sizeof(Reservoir) * pixels_per_batch;

    // FIXME: dedup this with the EXPOSURE_RES_X code
    uint32_t num_tiles_wide =
        divideRoundUp(cfg.imgWidth, VulkanConfig::localWorkgroupX);
    uint32_t num_tiles_tall =
        divideRoundUp(cfg.imgHeight, VulkanConfig::localWorkgroupY);

    uint32_t illuminance_bytes =
        sizeof(float) * num_tiles_wide * num_tiles_tall * batch_size;

    uint32_t adaptive_bytes =
        sizeof(AdaptiveTile) * num_tiles_wide * num_tiles_tall * batch_size;

    return FramebufferConfig {
        cfg.imgWidth,
        cfg.imgHeight,
        minibatch_size,
        minibatch_fb_images_wide,
        minibatch_fb_images_tall,
        batch_fb_images_wide,
        batch_fb_images_tall,
        batch_fb_width,
        batch_fb_height,
        num_tiles_wide,
        num_tiles_tall,
        output_bytes,
        hdr_bytes,
        normal_bytes,
        albedo_bytes,
        reservoir_bytes,
        illuminance_bytes,
        adaptive_bytes,
    };
}

static RenderState makeRenderState(const DeviceState &dev,
                                   const RenderConfig &cfg,
                                   const InitConfig &init_cfg)
{
    VkSampler repeat_sampler =
        makeImmutableSampler(dev, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkSampler clamp_sampler =
        makeImmutableSampler(dev, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    auto log2Int = [](uint32_t v) {
        return 32 - __builtin_clz(v) - 1;
    };

    uint32_t spp = cfg.spp;
    if (cfg.flags & RenderFlags::AdaptiveSample) {
#if 0
        if (spp > 512) {
            cerr << "SPP per launch too high for adaptive sampling" << endl;
            fatalExit();
        }
#endif

        if (spp < VulkanConfig::adaptive_samples_per_thread) {
            cerr << "SPP per launch too low for adaptive sampling - " << VulkanConfig::adaptive_samples_per_thread << endl;
            fatalExit();
        }
    }

    // In optix these are just C++ constexprs
    // Compute a bunch of sampler variables ahead of time
    uint32_t log2_spp = log2Int(spp);
    bool is_odd_power2 = log2_spp & 1;

    uint32_t index_shift = is_odd_power2 ? (log2_spp + 1) : log2_spp;

    uint32_t num_index_digits_base4 =
        log2Int(max(cfg.imgWidth, cfg.imgHeight) - 1) + 1 + (log2_spp + 1) / 2;

    string sampling_define;
    if (num_index_digits_base4 * 2 <= 32) {
        sampling_define = "ZSOBOL_SAMPLING";
    } else {
        cerr << "Warning: Not enough bits for ZSobol morton code.\n"
             << "Falling back to uniform sampling." << endl;

        sampling_define = "UNIFORM_SAMPLING";
    }

    if (cfg.flags & RenderFlags::ForceUniform) {
        sampling_define = "UNIFORM_SAMPLING";
    }

    float inv_sqrt_spp = 1.f / sqrtf(float(spp));
    ostringstream float_conv;
    float_conv.precision(9);
    float_conv << fixed << inv_sqrt_spp;

    uint32_t num_workgroups_x = divideRoundUp(cfg.imgWidth,
                                              VulkanConfig::localWorkgroupX);

    uint32_t num_workgroups_y = divideRoundUp(cfg.imgHeight,
                                              VulkanConfig::localWorkgroupY);

    vector<string> rt_defines {
        string("SPP (") + to_string(spp) + "u)",
        string("INV_SQRT_SPP (") + float_conv.str() + "f)",
        string("MAX_DEPTH (") + to_string(cfg.maxDepth) + "u)",
        string("RES_X (") + to_string(cfg.imgWidth) + "u)",
        string("RES_Y (") + to_string(cfg.imgHeight) + "u)",
        string("NUM_WORKGROUPS_X (") + to_string(num_workgroups_x) + "u)",
        string("NUM_WORKGROUPS_Y (") + to_string(num_workgroups_y) + "u)",
        string("BATCH_SIZE (") + to_string(cfg.batchSize) + "u)",
        sampling_define,
        string("ZSOBOL_NUM_BASE4 (") + to_string(num_index_digits_base4) + "u)",
        string("ZSOBOL_INDEX_SHIFT (") + to_string(index_shift) + "u)",
    };

    if (is_odd_power2) {
        rt_defines.push_back("ZSOBOL_ODD_POWER");
    }

    if (spp == 1) {
        rt_defines.push_back("ONE_SAMPLE");
    }

    if (cfg.maxDepth == 1) {
        rt_defines.push_back("PRIMARY_ONLY");
    }

    if (cfg.flags & RenderFlags::AuxiliaryOutputs) {
         rt_defines.emplace_back("AUXILIARY_OUTPUTS");
    }

    if (cfg.flags & RenderFlags::Tonemap) {
         rt_defines.emplace_back("TONEMAP");
    }

    if (cfg.flags & RenderFlags::AdaptiveSample) {
        rt_defines.emplace_back("ADAPTIVE_SAMPLING");
    }

    if ((cfg.flags & RenderFlags::Tonemap) ||
        (cfg.flags & RenderFlags::AdaptiveSample)) {
        rt_defines.emplace_back("NEED_SHARED_MEM");
    }

    if (cfg.clampThreshold > 0.f) {
        rt_defines.emplace_back(string("-DINDIRECT_CLAMP (") +
            to_string(cfg.clampThreshold) + "f)");
    }

    const char *rt_name;
    switch (cfg.mode) {
        case RenderMode::PathTracer:
            rt_name = "pathtracer.comp";
            break;
        case RenderMode::Biased:
            rt_name = "basic.comp";
            break;
    }

    // Give 9 digits of precision for lossless conversion
    auto floatToString = [](float v) {
        ostringstream strm;
        strm.precision(9);
        strm << fixed << v;

        return strm.str();
    };
    
    // 64 stops. Probably overkill, especially on high end
    float min_log_luminance = -30.f;
    float max_log_luminance = 34.f;
    float min_luminance = exp2(min_log_luminance);

    float log_luminance_range = max_log_luminance - min_log_luminance;
    float inv_log_luminance_range = 1.f / log_luminance_range;

    int num_exposure_bins = 128;
    float exposure_bias = 0.f;

    uint32_t thread_elems_x = divideRoundUp(cfg.imgWidth,
                                            VulkanConfig::localWorkgroupX);
    uint32_t thread_elems_y = divideRoundUp(cfg.imgHeight,
                                            VulkanConfig::localWorkgroupY);

    vector<string> exposure_defines {
        string("NUM_BINS (") + to_string(num_exposure_bins) + ")",
        string("LOG_LUMINANCE_RANGE (") +
            floatToString(log_luminance_range) + "f)",
        string("INV_LOG_LUMINANCE_RANGE (") +
            floatToString(inv_log_luminance_range) + "f)",
        string("MIN_LOG_LUMINANCE (") +
            floatToString(min_log_luminance) + "f)",
        string("MAX_LOG_LUMINANCE (") +
            floatToString(max_log_luminance) + "f)",
        string("EXPOSURE_BIAS (") +
            floatToString(exposure_bias) + "f)",
        string("MIN_LUMINANCE (") +
            floatToString(min_luminance) + "f)",
        string("RES_X (") + to_string(cfg.imgWidth) + "u)",
        string("RES_Y (") + to_string(cfg.imgHeight) + "u)",
        string("EXPOSURE_THREAD_ELEMS_X (") + to_string(thread_elems_x) + "u)",
        string("EXPOSURE_THREAD_ELEMS_Y (") + to_string(thread_elems_y) + "u)",
    };

    vector<string> tonemap_defines {
        string("RES_X (") + to_string(cfg.imgWidth) + "u)",
        string("RES_Y (") + to_string(cfg.imgHeight) + "u)",
    };

    if (init_cfg.validate) {
        rt_defines.push_back("VALIDATE");
        exposure_defines.push_back("VALIDATE");
        tonemap_defines.push_back("VALIDATE");
    }

    auto bake_defines = rt_defines;
    bake_defines[3] = string("RES_X (") + to_string(PROBE_WIDTH) + "u)";
    bake_defines[4] = string("RES_Y (") + to_string(PROBE_HEIGHT) + "u)";

    ShaderPipeline::initCompiler();

    std::unique_ptr<ShaderPipeline> rt;
    if (cfg.mode == RenderMode::PathTracer) {
        rt = make_unique<ShaderPipeline>(ShaderPipeline(dev,
            { rt_name },
            {
                {0, 4, repeat_sampler, 1, 0},
                {0, 5, clamp_sampler, 1, 0},
                {
                    1, 1, VK_NULL_HANDLE,
                    VulkanConfig::max_scenes * (VulkanConfig::max_materials *
                                                VulkanConfig::textures_per_material),
                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                },
                {
                    2, 0, VK_NULL_HANDLE,
                    VulkanConfig::max_env_maps * 2,
                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                },
            },
            rt_defines,
                                                        STRINGIFY(SHADER_DIR)));
    }
    else {
        rt = make_unique<ShaderPipeline>(ShaderPipeline(dev,
            { rt_name },
            {
                {0, 4, repeat_sampler, 1, 0},
                {0, 5, clamp_sampler, 1, 0},
                {
                    1, 1, VK_NULL_HANDLE,
                    VulkanConfig::max_scenes * (VulkanConfig::max_materials *
                                                VulkanConfig::textures_per_material),
                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                },
                {
                    2, 0, VK_NULL_HANDLE,
                    VulkanConfig::max_env_maps * 2,
                    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                },
                // Descriptor set for probe maps
                {
                    // LUC THIS IS WHERE YOU CONTROL HOW MANY DESCRIPTORS ARE IN THE PROBE SET
                    3, 0, VK_NULL_HANDLE, PROBE_COUNT, 0
                }
            },
            rt_defines,
                                                        STRINGIFY(SHADER_DIR)));
    }

    ShaderPipeline exposure(dev,
        { "exposure_histogram.comp" },
        {},
        exposure_defines,
        STRINGIFY(SHADER_DIR));

    ShaderPipeline tonemap(dev,
        { "tonemap.comp" },
        {},
        tonemap_defines,
        STRINGIFY(SHADER_DIR));

    ShaderPipeline baking(dev,
        { "bake.comp" },
        {
            {0, 4, repeat_sampler, 1, 0},
            {0, 5, clamp_sampler, 1, 0},
            {
                1, 1, VK_NULL_HANDLE,
                VulkanConfig::max_scenes * (VulkanConfig::max_materials *
                VulkanConfig::textures_per_material),
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
            },
            {
                2, 0, VK_NULL_HANDLE,
                VulkanConfig::max_env_maps * 2,
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
            },
        },
        bake_defines,
        STRINGIFY(SHADER_DIR));

    return RenderState {
        repeat_sampler,
        clamp_sampler,
        move(*rt),
        move(exposure),
        move(tonemap),
        move(baking)
    };
}

static RenderPipelines makePipelines(const DeviceState &dev,
                                     const RenderState &render_state,
                                     const RenderConfig &cfg)
{
    // Pipeline cache (unsaved)
    VkPipelineCacheCreateInfo pcache_info {};
    pcache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VkPipelineCache pipeline_cache;
    REQ_VK(dev.dt.createPipelineCache(dev.hdl, &pcache_info, nullptr,
                                      &pipeline_cache));

    // Push constant
    VkPushConstantRange push_const {
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(RTPushConstant),
    };

    // Layout configuration
    vector<VkDescriptorSetLayout> desc_layouts {
        render_state.rt.getLayout(0),
        render_state.rt.getLayout(1),
        render_state.rt.getLayout(2),
    };
    
    if (cfg.mode == RenderMode::Biased) {
        desc_layouts.push_back(render_state.rt.getLayout(3));
    }

    VkPipelineLayoutCreateInfo pt_layout_info;
    pt_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pt_layout_info.pNext = nullptr;
    pt_layout_info.flags = 0;
    pt_layout_info.setLayoutCount =
        static_cast<uint32_t>(desc_layouts.size());
    pt_layout_info.pSetLayouts = desc_layouts.data();
    pt_layout_info.pushConstantRangeCount = 1;
    pt_layout_info.pPushConstantRanges = &push_const;

    VkPipelineLayout pt_layout;
    REQ_VK(dev.dt.createPipelineLayout(dev.hdl, &pt_layout_info, nullptr,
                                       &pt_layout));

    VkDescriptorSetLayout exposure_set_layout =
        render_state.exposure.getLayout(0);

    VkPipelineLayoutCreateInfo exposure_layout_info;
    exposure_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    exposure_layout_info.pNext = nullptr;
    exposure_layout_info.flags = 0;
    exposure_layout_info.setLayoutCount = 1;
    exposure_layout_info.pSetLayouts = &exposure_set_layout;
    exposure_layout_info.pushConstantRangeCount = 0;
    exposure_layout_info.pPushConstantRanges = nullptr;

    VkPipelineLayout exposure_layout;
    REQ_VK(dev.dt.createPipelineLayout(dev.hdl, &exposure_layout_info, nullptr,
                                       &exposure_layout));

    VkDescriptorSetLayout tonemap_set_layout =
        render_state.tonemap.getLayout(0);

    VkPipelineLayoutCreateInfo tonemap_layout_info;
    tonemap_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tonemap_layout_info.pNext = nullptr;
    tonemap_layout_info.flags = 0;
    tonemap_layout_info.setLayoutCount = 1;
    tonemap_layout_info.pSetLayouts = &tonemap_set_layout;
    tonemap_layout_info.pushConstantRangeCount = 0;
    tonemap_layout_info.pPushConstantRanges = nullptr;

    VkPipelineLayout tonemap_layout;
    REQ_VK(dev.dt.createPipelineLayout(dev.hdl, &tonemap_layout_info, nullptr,
                                       &tonemap_layout));

    array<VkComputePipelineCreateInfo, 4> compute_infos;

    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size;
    subgroup_size.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
    subgroup_size.pNext = nullptr;
    subgroup_size.requiredSubgroupSize = VulkanConfig::subgroupSize;

    compute_infos[0].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_infos[0].pNext = nullptr;
    compute_infos[0].flags = 0;
    compute_infos[0].stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        &subgroup_size,
        VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT,
        VK_SHADER_STAGE_COMPUTE_BIT,
        render_state.rt.getShader(0),
        "main",
        nullptr,
    };
    compute_infos[0].layout = pt_layout;
    compute_infos[0].basePipelineHandle = VK_NULL_HANDLE;
    compute_infos[0].basePipelineIndex = -1;

    compute_infos[3].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_infos[3].pNext = nullptr;
    compute_infos[3].flags = 0;
    compute_infos[3].stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        &subgroup_size,
        VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT,
        VK_SHADER_STAGE_COMPUTE_BIT,
        render_state.bake.getShader(0),
        "main",
        nullptr,
    };
    compute_infos[3].layout = pt_layout;
    compute_infos[3].basePipelineHandle = VK_NULL_HANDLE;
    compute_infos[3].basePipelineIndex = -1;

    compute_infos[1].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_infos[1].pNext = nullptr;
    compute_infos[1].flags = 0;
    compute_infos[1].stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        &subgroup_size,
        VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT,
        VK_SHADER_STAGE_COMPUTE_BIT,
        render_state.exposure.getShader(0),
        "main",
        nullptr,
    };
    compute_infos[1].layout = exposure_layout;
    compute_infos[1].basePipelineHandle = VK_NULL_HANDLE;
    compute_infos[1].basePipelineIndex = -1;

    compute_infos[2].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_infos[2].pNext = nullptr;
    compute_infos[2].flags = 0;
    compute_infos[2].stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        &subgroup_size,
        VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT,
        VK_SHADER_STAGE_COMPUTE_BIT,
        render_state.tonemap.getShader(0),
        "main",
        nullptr,
    };
    compute_infos[2].layout = tonemap_layout;
    compute_infos[2].basePipelineHandle = VK_NULL_HANDLE;
    compute_infos[2].basePipelineIndex = -1;

    array<VkPipeline, compute_infos.size()> pipelines;
    REQ_VK(dev.dt.createComputePipelines(dev.hdl, pipeline_cache,
                                         compute_infos.size(),
                                         compute_infos.data(), nullptr,
                                         pipelines.data()));

    return RenderPipelines {
        pipeline_cache,
        PipelineState {
            pt_layout,
            pipelines[0],
        },
        PipelineState {
            exposure_layout,
            pipelines[1],
        },
        PipelineState {
            tonemap_layout,
            pipelines[2],
        },
        PipelineState {
            // For now uses the same pipeline layout as path tracing
            pt_layout,
            pipelines[3]
        }
    };
}

static FramebufferState makeFramebuffer(const DeviceState &dev,
                                        const VulkanBackend::Config &cfg,
                                        const FramebufferConfig &fb_cfg,
                                        MemoryAllocator &alloc)
{
    vector<LocalBuffer> outputs;
    vector<VkDeviceMemory> backings;
    vector<CudaImportedBuffer> exported;

    uint32_t num_buffers = 2;
    uint32_t num_exported_buffers = 1;

    int output_idx = 0;
    int hdr_idx = 1;
    int normal_idx = -1;
    int albedo_idx = -1;
    int illuminance_idx = -1;
    int adaptive_idx = -1;

    if (cfg.auxiliaryOutputs) {
        num_buffers += 2;
        num_exported_buffers += 2;

        normal_idx = 2;
        albedo_idx = 3;
    }

    if (cfg.tonemap) {
        illuminance_idx = num_buffers++;
    }

    if (cfg.adaptiveSampling) {
        adaptive_idx = num_buffers++;
    }

    outputs.reserve(num_buffers);
    backings.reserve(num_buffers);
    exported.reserve(num_exported_buffers);

    auto [main_buffer, main_mem] =
        alloc.makeDedicatedBuffer(fb_cfg.outputBytes);

    outputs.emplace_back(move(main_buffer));
    backings.emplace_back(move(main_mem));
    exported.emplace_back(dev, cfg.gpuID, backings.back(),
                          fb_cfg.outputBytes);

    auto [hdr_buffer, hdr_mem] =
        alloc.makeDedicatedBuffer(fb_cfg.hdrBytes);

    outputs.emplace_back(move(hdr_buffer));
    backings.emplace_back(move(hdr_mem));

    if (cfg.auxiliaryOutputs) {
        auto [normal_buffer, normal_mem] =
            alloc.makeDedicatedBuffer(fb_cfg.normalBytes);

        outputs.emplace_back(move(normal_buffer));
        backings.emplace_back(move(normal_mem));
        exported.emplace_back(dev, cfg.gpuID, backings.back(),
                              fb_cfg.normalBytes);

        auto [albedo_buffer, albedo_mem] =
            alloc.makeDedicatedBuffer(fb_cfg.albedoBytes);

        outputs.emplace_back(move(albedo_buffer));
        backings.emplace_back(move(albedo_mem));
        exported.emplace_back(dev, cfg.gpuID, backings.back(),
                              fb_cfg.albedoBytes);
    }

    if (cfg.tonemap) {
        auto [illum_buffer, illum_mem] =
            alloc.makeDedicatedBuffer(fb_cfg.illuminanceBytes);

        outputs.emplace_back(move(illum_buffer));
        backings.emplace_back(move(illum_mem));
    }

    optional<HostBuffer> adaptive_readback;
    optional<HostBuffer> exposure_readback;

    if (cfg.adaptiveSampling) {
        auto [adaptive_buffer, adaptive_mem] =
            alloc.makeDedicatedBuffer(fb_cfg.adaptiveBytes);

        outputs.emplace_back(move(adaptive_buffer));
        backings.emplace_back(move(adaptive_mem));

        // FIXME: specific readback buffer type
        adaptive_readback = alloc.makeHostBuffer(fb_cfg.adaptiveBytes, true);
        exposure_readback = alloc.makeHostBuffer(fb_cfg.illuminanceBytes, true);
    }

    vector<LocalBuffer> reservoirs;
    vector<VkDeviceMemory> reservoir_mem;

    auto [prev_reservoir_buffer, prev_reservoir_mem] =
        alloc.makeDedicatedBuffer(fb_cfg.reservoirBytes);

    reservoirs.emplace_back(move(prev_reservoir_buffer));
    reservoir_mem.emplace_back(move(prev_reservoir_mem));

    auto [cur_reservoir_buffer, cur_reservoir_mem] =
        alloc.makeDedicatedBuffer(fb_cfg.reservoirBytes);

    reservoirs.emplace_back(move(cur_reservoir_buffer));
    reservoir_mem.emplace_back(move(cur_reservoir_mem));

    return FramebufferState {
        move(outputs),
        move(backings),
        move(exported),
        move(reservoirs),
        move(reservoir_mem),
        move(adaptive_readback),
        move(exposure_readback),
        output_idx,
        hdr_idx,
        normal_idx,
        albedo_idx,
        illuminance_idx,
        adaptive_idx,
    };
}

static PerBatchState makePerBatchState(const DeviceState &dev,
                                       const FramebufferConfig &fb_cfg,
                                       const FramebufferState &fb,
                                       const ParamBufferConfig &param_cfg,
                                       HostBuffer &param_buffer,
                                       const LocalBuffer &dev_param_buffer,
                                       optional<HostBuffer> &tile_input_buffer,
                                       FixedDescriptorPool &&rt_pool,
                                       FixedDescriptorPool &&exposure_pool,
                                       FixedDescriptorPool &&tonemap_pool,
                                       const BSDFPrecomputed &precomp_tex,
                                       bool auxiliary_outputs,
                                       bool tonemap,
                                       bool adaptive_sampling,
                                       bool need_present)
{
    array rt_sets {
        rt_pool.makeSet(),
        rt_pool.makeSet(),
    };

    VkDescriptorSet exposure_set = exposure_pool.makeSet();
    VkDescriptorSet tonemap_set = tonemap_pool.makeSet();

    VkCommandPool cmd_pool = makeCmdPool(dev, dev.computeQF);
    VkCommandBuffer render_cmd = makeCmdBuffer(dev, cmd_pool);

    half *output_buffer = (half *)fb.exported[0].getDevicePointer();

    half *normal_buffer = nullptr, *albedo_buffer = nullptr;
    if (auxiliary_outputs) {
        normal_buffer = (half *)fb.exported[1].getDevicePointer();

        albedo_buffer = (half *)fb.exported[2].getDevicePointer();
    }

    AdaptiveTile *adaptive_readback_ptr = nullptr;
    if (adaptive_sampling) {
        adaptive_readback_ptr = (AdaptiveTile *)fb.adaptiveReadback->ptr;
    }

    vector<VkWriteDescriptorSet> desc_set_updates;

    uint8_t *base_ptr =
        reinterpret_cast<uint8_t *>(param_buffer.ptr);

    InstanceTransform *transform_ptr =
        reinterpret_cast<InstanceTransform *>(base_ptr);

    uint32_t *material_ptr = reinterpret_cast<uint32_t *>(
        base_ptr + param_cfg.materialIndicesOffset);

    PackedLight *light_ptr =
        reinterpret_cast<PackedLight *>(base_ptr + param_cfg.lightsOffset);

    PackedEnv *env_ptr =
        reinterpret_cast<PackedEnv *>(base_ptr + param_cfg.envOffset);

    DescriptorUpdates desc_updates(14);

    VkDescriptorBufferInfo transform_info {
        dev_param_buffer.buffer,
        0,
        param_cfg.totalTransformBytes,
    };

    VkDescriptorBufferInfo mat_info {
        dev_param_buffer.buffer,
        param_cfg.materialIndicesOffset,
        param_cfg.totalMaterialIndexBytes,
    };

    VkDescriptorBufferInfo light_info {
        dev_param_buffer.buffer,
        param_cfg.lightsOffset,
        param_cfg.totalLightParamBytes,
    };

    VkDescriptorBufferInfo env_info {
        dev_param_buffer.buffer,
        param_cfg.envOffset,
        param_cfg.totalEnvParamBytes,
    };

    VkDescriptorImageInfo diffuse_avg_info {
        VK_NULL_HANDLE,
        precomp_tex.msDiffuseAverage.second,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo diffuse_dir_info {
        VK_NULL_HANDLE,
        precomp_tex.msDiffuseDirectional.second,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo ggx_avg_info {
        VK_NULL_HANDLE,
        precomp_tex.msGGXAverage.second,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo ggx_dir_info {
        VK_NULL_HANDLE,
        precomp_tex.msGGXDirectional.second,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo ggx_inv_info {
        VK_NULL_HANDLE,
        precomp_tex.msGGXInverse.second,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorBufferInfo out_info {
        fb.outputs[fb.outputIdx].buffer,
        0,
        fb_cfg.outputBytes,
    };

    VkDescriptorBufferInfo hdr_info {
        fb.outputs[fb.hdrIdx].buffer,
        0,
        fb_cfg.hdrBytes,
    };

    for (int i = 0; i < (int)rt_sets.size(); i++) {
        desc_updates.buffer(rt_sets[i], &transform_info, 0,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(rt_sets[i], &mat_info, 1,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(rt_sets[i], &light_info, 2,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(rt_sets[i], &env_info, 3,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.textures(rt_sets[i], &diffuse_avg_info, 1, 6);
        desc_updates.textures(rt_sets[i], &diffuse_dir_info, 1, 7);
        desc_updates.textures(rt_sets[i], &ggx_avg_info, 1, 8);
        desc_updates.textures(rt_sets[i], &ggx_dir_info, 1, 9);
        desc_updates.textures(rt_sets[i], &ggx_inv_info, 1, 10);
        desc_updates.buffer(rt_sets[i], &hdr_info, 13,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    VkDescriptorBufferInfo cur_reservoir_info {
        fb.reservoirs[0].buffer,
        0,
        fb_cfg.reservoirBytes,
    };

    desc_updates.buffer(rt_sets[0], &cur_reservoir_info, 11,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    desc_updates.buffer(rt_sets[1], &cur_reservoir_info, 12,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    VkDescriptorBufferInfo prev_reservoir_info {
        fb.reservoirs[1].buffer,
        0,
        fb_cfg.reservoirBytes,
    };

    desc_updates.buffer(rt_sets[0], &prev_reservoir_info, 12,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    desc_updates.buffer(rt_sets[1], &prev_reservoir_info, 11,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    VkDescriptorBufferInfo normal_info;
    VkDescriptorBufferInfo albedo_info;

    if (auxiliary_outputs) {
        normal_info = {
            fb.outputs[fb.normalIdx].buffer,
            0,
            fb_cfg.normalBytes,
        };

        albedo_info = {
            fb.outputs[fb.albedoIdx].buffer,
            0,
            fb_cfg.albedoBytes,
        };

        for (int i = 0; i < (int)rt_sets.size(); i++) {
            desc_updates.buffer(rt_sets[i], &normal_info, 14,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            desc_updates.buffer(rt_sets[i], &albedo_info, 15,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }
    }

    VkDescriptorBufferInfo illuminance_info;

    if (tonemap) {
        illuminance_info = {
            fb.outputs[fb.illuminanceIdx].buffer,
            0,
            fb_cfg.illuminanceBytes,
        };

        for (int i = 0; i < (int)rt_sets.size(); i++) {
            desc_updates.buffer(rt_sets[i], &illuminance_info, 16,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }

        desc_updates.buffer(exposure_set, &illuminance_info, 0,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(exposure_set, &hdr_info, 1,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(tonemap_set, &illuminance_info, 0,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(tonemap_set, &hdr_info, 1,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        desc_updates.buffer(tonemap_set, &out_info, 2,
                            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    VkDescriptorBufferInfo tile_input_info;
    VkDescriptorBufferInfo adaptive_info;
    if (adaptive_sampling) {
        tile_input_info = {
            tile_input_buffer->buffer,
            0,
            sizeof(InputTile) * VulkanConfig::max_tiles * 2,
        };

        adaptive_info = {
            fb.outputs[fb.adaptiveIdx].buffer,
            0,
            fb_cfg.adaptiveBytes,
        };

        for (int i = 0; i < (int)rt_sets.size(); i++) {
            desc_updates.buffer(rt_sets[i], &tile_input_info, 17,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            desc_updates.buffer(rt_sets[i], &adaptive_info, 18,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }
    }

    desc_updates.update(dev);

    return PerBatchState {
        makeFence(dev),
        need_present ? makeBinarySemaphore(dev) : VK_NULL_HANDLE,
        need_present ? makeBinarySemaphore(dev) : VK_NULL_HANDLE,
        cmd_pool,
        render_cmd,
        output_buffer,
        normal_buffer,
        albedo_buffer,
        move(rt_pool),
        move(rt_sets),
        move(exposure_pool),
        exposure_set,
        move(tonemap_pool),
        tonemap_set,
        transform_ptr,
        material_ptr,
        light_ptr,
        env_ptr,
        (InputTile *)tile_input_buffer->ptr,
        adaptive_readback_ptr,
    };
}

static BSDFPrecomputed loadPrecomputedTextures(const DeviceState &dev,
    MemoryAllocator &alloc,
    QueueState &render_queue,
    uint32_t qf_idx)
{
    const string dir = STRINGIFY(RLPBR_DATA_DIR);

    vector<pair<string, vector<uint32_t>>> names_and_dims {
        { "diffuse_avg_albedo.bin", { 16, 16 } },
        { "diffuse_dir_albedo.bin", { 16, 16, 16 } },
        { "ggx_avg_albedo.bin", { 32 } },
        { "ggx_dir_albedo.bin", { 32, 32 } },
        { "ggx_dir_inv.bin", { 128, 32 } },
    };

    vector<DynArray<float>> raw_data;
    raw_data.reserve(names_and_dims.size());

    // This code assumes all the LUTs are stored as R32 (4 byte alignment)
    vector<uint32_t> num_elems;
    num_elems.reserve(names_and_dims.size());
    vector<size_t> offsets;
    offsets.reserve(names_and_dims.size());
    vector<LocalTexture> imgs;
    imgs.reserve(names_and_dims.size());

    VkFormat fmt = alloc.getTextureFormat(TextureFormat::R32_SFLOAT);

    vector<VkImageMemoryBarrier> barriers;
    barriers.reserve(imgs.size());

    size_t cur_tex_offset = 0;
    for (const auto &[name, dims] : names_and_dims) {
        uint32_t total_elems = 1;
        for (uint32_t d : dims) {
            total_elems *= d;
        }
        num_elems.push_back(total_elems);

        LocalTexture tex;
        TextureRequirements tex_reqs;
        if (dims.size() == 1) {
            tie(tex, tex_reqs) = alloc.makeTexture1D(dims[0], 1, fmt);
        } else if (dims.size() == 2) {
            tie(tex, tex_reqs) = alloc.makeTexture2D(dims[0], dims[1], 1, fmt);
        } else if (dims.size() == 3) {
            tie(tex, tex_reqs) =
                alloc.makeTexture3D(dims[0], dims[1], dims[2], 1, fmt);
        } else {
            assert(false);
        }
        imgs.emplace_back(move(tex));

        cur_tex_offset = alignOffset(cur_tex_offset, tex_reqs.alignment);
        offsets.push_back(cur_tex_offset);

        cur_tex_offset += tex_reqs.size;

        barriers.push_back({
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            imgs.back().image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        });
    }

    size_t total_bytes = cur_tex_offset;

    HostBuffer staging = alloc.makeStagingBuffer(total_bytes);
    optional<VkDeviceMemory> backing_opt = alloc.alloc(total_bytes);

    if (!backing_opt.has_value()) {
        cerr << "Not enough memory to allocate BSDF LUTs" << endl;
        fatalExit();
    }

    VkDeviceMemory backing = backing_opt.value();

    vector<VkImageView> views;
    views.reserve(imgs.size());

    VkCommandPool cmd_pool = makeCmdPool(dev, qf_idx);

    VkCommandBuffer cmd = makeCmdBuffer(dev, cmd_pool);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    REQ_VK(dev.dt.beginCommandBuffer(cmd, &begin_info));

    for (int i = 0; i < (int)imgs.size(); i++) {
        char *cur = (char *)staging.ptr + offsets[i];

        ifstream file(dir + "/" + names_and_dims[i].first);
        file.read(cur, num_elems[i] * sizeof(float));

        REQ_VK(dev.dt.bindImageMemory(dev.hdl, imgs[i].image, backing,
                                      offsets[i]));

        const auto &dims = names_and_dims[i].second;

        VkImageViewCreateInfo view_info;
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.pNext = nullptr;
        view_info.flags = 0;
        view_info.image = imgs[i].image;

        if (dims.size() == 1) {
            view_info.viewType = VK_IMAGE_VIEW_TYPE_1D;
        } else if (dims.size() == 2) {
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        } else if (dims.size() == 3) {
            view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        }

        view_info.format = fmt;
        view_info.components = {
            VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A,
        };
        view_info.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0,
            1,
            0,
            1,
        };

        VkImageView view;
        REQ_VK(dev.dt.createImageView(dev.hdl, &view_info, nullptr, &view));
        views.push_back(view);
    }

    dev.dt.cmdPipelineBarrier(
        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
        0, nullptr, barriers.size(), barriers.data());

    for (int i = 0; i < (int)imgs.size(); i++) {
        const auto &dims = names_and_dims[i].second;

        VkBufferImageCopy copy_info {};
        copy_info.bufferOffset = offsets[i];
        copy_info.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy_info.imageSubresource.mipLevel = 0;
        copy_info.imageSubresource.baseArrayLayer = 0;
        copy_info.imageSubresource.layerCount = 1;
        copy_info.imageExtent.width = dims[0];

        if (dims.size() > 1) {
            copy_info.imageExtent.height = dims[1];
        } else {
            copy_info.imageExtent.height = 1;
        }

        if (dims.size() > 2) {
            copy_info.imageExtent.depth = dims[2];
        } else {
            copy_info.imageExtent.depth = 1;
        }

        dev.dt.cmdCopyBufferToImage(
            cmd, staging.buffer, imgs[i].image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy_info);

        VkImageMemoryBarrier &barrier = barriers[i];
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    staging.flush(dev);

    dev.dt.cmdPipelineBarrier(
        cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
        0, nullptr, barriers.size(), barriers.data());

    REQ_VK(dev.dt.endCommandBuffer(cmd));

    VkSubmitInfo render_submit {};
    render_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    render_submit.waitSemaphoreCount = 0;
    render_submit.pWaitSemaphores = nullptr;
    render_submit.pWaitDstStageMask = nullptr;
    render_submit.commandBufferCount = 1;
    render_submit.pCommandBuffers = &cmd;

    VkFence fence = makeFence(dev);

    render_queue.submit(dev, 1, &render_submit, fence);

    waitForFenceInfinitely(dev, fence);

    dev.dt.destroyFence(dev.hdl, fence, nullptr);
    dev.dt.destroyCommandPool(dev.hdl, cmd_pool, nullptr);

    return BSDFPrecomputed {
        backing,
        { imgs[0], views[0] },
        { imgs[1], views[1] },
        { imgs[2], views[2] },
        { imgs[3], views[3] },
        { imgs[4], views[4] },
    };
}

static DynArray<QueueState> initTransferQueues(const RenderConfig &cfg,
                                               const DeviceState &dev)
{
    bool transfer_shared = cfg.numLoaders > dev.numTransferQueues;

    DynArray<QueueState> queues(dev.numTransferQueues);

    for (int i = 0; i < (int)queues.size(); i++) {
        new (&queues[i])
            QueueState(makeQueue(dev, dev.transferQF, i), transfer_shared);
    }

    return queues;
}

static DynArray<QueueState> initComputeQueues(const RenderConfig &cfg,
                                              const DeviceState &dev)
{
    DynArray<QueueState> queues(dev.numComputeQueues);

    int num_transfer_compute_queues = max((int)dev.numComputeQueues - 2, 0);
    bool loaders_shared = (int)cfg.numLoaders > num_transfer_compute_queues;

    for (int i = 0; i < (int)queues.size(); i++) {
        bool shared = num_transfer_compute_queues == 0 ||
            (i >= 2 && loaders_shared);

        new (&queues[i]) QueueState(makeQueue(dev, dev.computeQF, i),
                                    shared);
    }

    return queues;
}

static glm::u32vec3 getLaunchSize(const RenderConfig &cfg)
{
    return {
        divideRoundUp(cfg.imgWidth, VulkanConfig::localWorkgroupX),
        divideRoundUp(cfg.imgHeight, VulkanConfig::localWorkgroupY),
        divideRoundUp(cfg.batchSize, VulkanConfig::localWorkgroupZ),
    };
}

static InstanceState makeInstance(const InitConfig &init_cfg)
{
    if (init_cfg.needPresent) {
        auto get_inst_addr = PresentationState::init();

        return InstanceState(get_inst_addr, init_cfg.validate, true,
                             PresentationState::getInstanceExtensions());

    } else {
        return InstanceState(nullptr, init_cfg.validate, false, {});
    }
}

static DeviceState makeDevice(const InstanceState &inst,
                              const RenderConfig &cfg,
                              const InitConfig &init_cfg)
{
    auto present_callback = init_cfg.needPresent ?
        PresentationState::deviceSupportCallback :
        nullptr;

    uint32_t num_compute_queues = 2 + cfg.numLoaders;

    return inst.makeDevice(getUUIDFromCudaID(cfg.gpuID),
                           1,
                           num_compute_queues, 
                           cfg.numLoaders,
                           present_callback);
}

VulkanBackend::VulkanBackend(const RenderConfig &cfg, bool validate)
    : VulkanBackend(cfg, getInitConfig(cfg, validate))
{}

VulkanBackend::VulkanBackend(const RenderConfig &cfg,
                             const InitConfig &init_cfg)
    : cfg_({
          cfg.gpuID,
          cfg.batchSize,
          cfg.numLoaders,
          cfg.maxTextureResolution == 0 ? ~0u :
              cfg.maxTextureResolution,
          cfg.spp,
          cfg.flags & RenderFlags::AuxiliaryOutputs,
          cfg.flags & RenderFlags::Tonemap,
          cfg.flags & RenderFlags::Randomize,
          cfg.flags & RenderFlags::AdaptiveSample,
          cfg.flags & RenderFlags::Denoise,
      }),
      rcfg_(cfg),
      inst(makeInstance(init_cfg)),
      dev(makeDevice(inst, cfg, init_cfg)),
      alloc(dev, inst),
      fb_cfg_(getFramebufferConfig(cfg)),
      param_cfg_(
          getParamBufferConfig(cfg.batchSize, alloc)),
      render_state_(makeRenderState(dev, cfg, init_cfg)),
      pipelines_(makePipelines(dev, render_state_, cfg)),
      transfer_queues_(initTransferQueues(cfg, dev)),
      compute_queues_(initComputeQueues(cfg, dev)),
      bsdf_precomp_(loadPrecomputedTextures(dev, alloc, compute_queues_[0],
                    dev.computeQF)),
      launch_size_(getLaunchSize(cfg)),
      num_loaders_(0),
      scene_pool_(render_state_.rt.makePool(1, 1)),
      shared_scene_state_(dev, scene_pool_, render_state_.rt.getLayout(1),
                          alloc),
      env_map_state_(
          make_unique<SharedEnvMapState>(dev, render_state_.rt, 2)),
      cur_env_maps_(nullptr),
      cur_queue_(0),
      frame_counter_(0),
      present_(init_cfg.needPresent ?
          make_optional<PresentationState>(inst, dev, dev.computeQF,
                                           1, glm::u32vec2(1, 1), true) :
          optional<PresentationState>()),
      denoiser_((cfg.flags & RenderFlags::Denoise || cfg.mode == RenderMode::Biased) ?
                make_optional<Denoiser>(alloc, cfg) :
                optional<Denoiser>())
{
    if (init_cfg.needPresent) {
        present_->forceTransition(dev, compute_queues_[0], dev.computeQF);
    }
}

LoaderImpl VulkanBackend::makeLoader()
{
    int loader_idx = num_loaders_.fetch_add(1, memory_order_acq_rel);
    assert(loader_idx < (int)cfg_.maxLoaders);

    int num_transfer_compute_queues = max((int)compute_queues_.size() - 2, 0);

    int compute_queue_idx = num_transfer_compute_queues == 0 ? 0 :
        loader_idx % num_transfer_compute_queues + 2;

    auto loader = new VulkanLoader(
        dev, alloc, transfer_queues_[loader_idx % transfer_queues_.size()],
        compute_queues_[compute_queue_idx], shared_scene_state_,
        env_map_state_.get(), dev.computeQF, cfg_.maxTextureResolution);

    return makeLoaderImpl<VulkanLoader>(loader);
}

EnvironmentImpl VulkanBackend::makeEnvironment(const shared_ptr<Scene> &scene,
                                               const Camera &cam)
{
    // FIXME: allow seeding, get rid of thread_local, need some kind of
    // VulkanBackend thread context
    static thread_local mt19937 rand_gen {random_device {}()};

    const VulkanScene &vk_scene = *static_cast<VulkanScene *>(scene.get());
    VulkanEnvironment *environment =
        new VulkanEnvironment(dev, vk_scene, cam, rand_gen,
                              cfg_.enableRandomization,
                              cur_env_maps_->texData.textures.size() / 2);
    return makeEnvironmentImpl<VulkanEnvironment>(environment);
}

BakerImpl VulkanBackend::makeBaker()
{
    auto baker = new VulkanBaker(
        dev, alloc, transfer_queues_[0], compute_queues_[0],
        pipelines_.bake.hdl, pipelines_.bake.layout
    );
    return makeBakerImpl<VulkanBaker>(baker);
}

void VulkanBackend::setActiveEnvironmentMaps(
    shared_ptr<EnvironmentMapGroup> env_maps)
{
    auto impl = static_pointer_cast<VulkanEnvMapGroup>(move(env_maps));
    cur_env_maps_ = move(impl);
}

RenderBatch::Handle VulkanBackend::makeRenderBatch()
{
    auto deleter = [](void *, BatchBackend *base_ptr) {
        auto ptr = static_cast<VulkanBatch *>(base_ptr);
        delete ptr;
    };

    auto fb = makeFramebuffer(dev, cfg_, fb_cfg_, alloc);

    // max_tiles * 2 to allow double buffering to hide cpu readback
    optional<HostBuffer> adaptive_input;

    if (cfg_.adaptiveSampling) {
        adaptive_input = alloc.makeHostBuffer(
            sizeof(InputTile) * VulkanConfig::max_tiles * 2, true);
    }

    auto render_input_staging =
        alloc.makeStagingBuffer(param_cfg_.totalParamBytes);

    auto render_input_dev =
        *alloc.makeLocalBuffer(param_cfg_.totalParamBytes, true);

    PerBatchState batch_state = makePerBatchState(
            dev, fb_cfg_, fb, param_cfg_,
            render_input_staging,
            render_input_dev,
            adaptive_input,
            FixedDescriptorPool(dev, render_state_.rt, 0, 2),
            FixedDescriptorPool(dev, render_state_.exposure, 0, 1),
            FixedDescriptorPool(dev, render_state_.tonemap, 0, 1),
            bsdf_precomp_, cfg_.auxiliaryOutputs, cfg_.tonemap,
            cfg_.adaptiveSampling, present_.has_value());

    auto backend = new VulkanBatch {
        {},
        move(fb),
        move(adaptive_input),
        move(render_input_staging),
        move(render_input_dev),
        move(batch_state),
        0,
    };

    return RenderBatch::Handle(backend, {nullptr, deleter});
}

void VulkanBackend::makeBakeOutput()
{
    auto fb = makeFramebuffer(dev, cfg_, fb_cfg_, alloc);

    // max_tiles * 2 to allow double buffering to hide cpu readback
    optional<HostBuffer> adaptive_input;

    if (cfg_.adaptiveSampling) {
        adaptive_input = alloc.makeHostBuffer(
            sizeof(InputTile) * VulkanConfig::max_tiles * 2, true);
    }

    auto render_input_staging =
        alloc.makeStagingBuffer(param_cfg_.totalParamBytes);

    auto render_input_dev =
        *alloc.makeLocalBuffer(param_cfg_.totalParamBytes, true);

    PerBatchState batch_state = makePerBatchState(
            dev, fb_cfg_, fb, param_cfg_,
            render_input_staging,
            render_input_dev,
            adaptive_input,
            FixedDescriptorPool(dev, render_state_.bake, 0, 2),
            FixedDescriptorPool(dev, render_state_.exposure, 0, 1),
            FixedDescriptorPool(dev, render_state_.tonemap, 0, 1),
            bsdf_precomp_, cfg_.auxiliaryOutputs, cfg_.tonemap,
            cfg_.adaptiveSampling, present_.has_value());
}

ProbeGenState VulkanBackend::makeProbeGenState()
{
    auto probe_config = getProbeFramebufferConfig(probe_width_, probe_height_);

    auto fb = makeFramebuffer(dev, cfg_, probe_config, alloc);

    // max_tiles * 2 to allow double buffering to hide cpu readback
    optional<HostBuffer> adaptive_input;

    if (cfg_.adaptiveSampling) {
        adaptive_input = alloc.makeHostBuffer(
            sizeof(InputTile) * VulkanConfig::max_tiles * 2, true);
    }

    auto render_input_staging =
        alloc.makeStagingBuffer(param_cfg_.totalParamBytes);

    auto render_input_dev =
        *alloc.makeLocalBuffer(param_cfg_.totalParamBytes, true);

    PerBatchState batch_state = makePerBatchState(
            dev, probe_config, fb, param_cfg_,
            render_input_staging,
            render_input_dev,
            adaptive_input,
            FixedDescriptorPool(dev, render_state_.bake, 0, 2),
            FixedDescriptorPool(dev, render_state_.exposure, 0, 1),
            FixedDescriptorPool(dev, render_state_.tonemap, 0, 1),
            bsdf_precomp_, cfg_.auxiliaryOutputs, cfg_.tonemap,
            cfg_.adaptiveSampling, present_.has_value());

    ProbeGenState res = {
        {
            {},
            move(fb),
            move(adaptive_input),
            move(render_input_staging),
            move(render_input_dev),
            move(batch_state),
            0,
        },
    };

    return res;
}

static PackedCamera packCamera(const Camera &cam)
{
    PackedCamera packed;

    // FIXME, consider elevating the quaternion representation to
    // external camera interface
    // FIXME: cam.aspectRatio is no longer respected
    glm::quat rotation =
        glm::quat_cast(glm::mat3(cam.right, -cam.up, cam.view));

    packed.rotation =
        glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);

    packed.posAndTanFOV = glm::vec4(cam.position, cam.tanFOV);

    return packed;
}

static VulkanBatch *getVkBatch(RenderBatch &batch)
{
    return static_cast<VulkanBatch *>(batch.getBackend());
}

void VulkanBackend::render(RenderBatch &batch)
{
    VulkanBatch &batch_backend = *getVkBatch(batch);
    Environment *envs = batch.getEnvironments();
    PerBatchState &batch_state = batch_backend.state;
    VkCommandBuffer render_cmd = batch_state.renderCmd;

    auto startRenderSetup = [&]() {
        REQ_VK(dev.dt.resetCommandPool(dev.hdl, batch_state.cmdPool, 0));

        VkCommandBufferBeginInfo begin_info {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        REQ_VK(dev.dt.beginCommandBuffer(render_cmd, &begin_info));

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.rt.hdl);

        RTPushConstant push_const = {
            frame_counter_,
            (uint32_t)PROBE_DIM.x,
            (uint32_t)PROBE_DIM.y,
            (uint32_t)PROBE_DIM.z,
            glm::vec4(envs->getScene()->envInit.defaultBBox.pMin, 0.0f),
            glm::vec4(envs->getScene()->envInit.defaultBBox.pMax, 0.0f),
            (uint32_t)probeIdx,
        };

        dev.dt.cmdPushConstants(render_cmd, pipelines_.rt.layout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(RTPushConstant),
                                &push_const);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.rt.layout, 0, 1,
                                     &batch_state.rtSets[batch_backend.curBuffer],
                                     0, nullptr);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.rt.layout, 2, 1,
                                     &cur_env_maps_->descSet.hdl,
                                     0, nullptr);

        if (rcfg_.mode == RenderMode::Biased) {
            dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                         pipelines_.rt.layout, 3, 1,
                                         &probe_dset_, 0, nullptr);
        }

        shared_scene_state_.lock.lock();
        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.rt.layout, 1, 1,
                                     &shared_scene_state_.descSet, 0, nullptr);

        shared_scene_state_.lock.unlock();
    };

    startRenderSetup();

    // TLAS build
    for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize; batch_idx++) {
        const Environment &env = envs[batch_idx];

        if (env.isDirty()) {
            VulkanEnvironment &env_backend =
                *(VulkanEnvironment *)(env.getBackend());
            const VulkanScene &scene =
                *static_cast<const VulkanScene *>(env.getScene().get());

            env_backend.tlas.build(dev, alloc, env.getInstances(),
                                   env.getTransforms(), env.getInstanceFlags(),
                                   scene.objectInfo, scene.blases, render_cmd);

            env.clearDirty();
        }
    }

    VkMemoryBarrier tlas_barrier;
    tlas_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    tlas_barrier.pNext = nullptr;
    tlas_barrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlas_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    dev.dt.cmdPipelineBarrier(render_cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
        &tlas_barrier, 0, nullptr, 0, nullptr);

    uint32_t inst_offset = 0;
    uint32_t material_offset = 0;
    uint32_t light_offset = 0;

    // Write environment data into linear buffers
    for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize; batch_idx++) {
        Environment &env = envs[batch_idx];
        VulkanEnvironment &env_backend =
            *static_cast<VulkanEnvironment *>(env.getBackend());
        const VulkanScene &scene_backend =
            *static_cast<const VulkanScene *>(env.getScene().get());

        PackedEnv &packed_env = batch_state.envPtr[batch_idx];

        packed_env.cam = packCamera(env.getCamera());
        packed_env.prevCam = packCamera(env_backend.prevCam);
        packed_env.data.x = scene_backend.sceneID->getID();

        // Set prevCam for next iteration
        env_backend.prevCam = env.getCamera();

        const auto &env_transforms = env.getTransforms();
        uint32_t num_instances = env.getNumInstances();
        memcpy(&batch_state.transformPtr[inst_offset], env_transforms.data(),
               sizeof(InstanceTransform) * num_instances);
        inst_offset += num_instances;

        const auto &env_mats = env.getInstanceMaterials();

        memcpy(&batch_state.materialPtr[material_offset], env_mats.data(),
               env_mats.size() * sizeof(uint32_t));

        packed_env.data.y = material_offset;
        material_offset += env_mats.size();

        memcpy(&batch_state.lightPtr[light_offset], env_backend.lights.data(),
               env_backend.lights.size() * sizeof(PackedLight));

        packed_env.data.z = light_offset;
        packed_env.data.w = env_backend.lights.size();
        light_offset += env_backend.lights.size();

        packed_env.tlasAddr = env_backend.tlas.tlasStorageDevAddr;
        //packed_env.reservoirGridAddr = env_backend.reservoirGrid.devAddr;
        packed_env.reservoirGridAddr = 0;

        packed_env.envMapRotation.x =
            env_backend.domainRandomization.envRotation.x;
        packed_env.envMapRotation.y =
            env_backend.domainRandomization.envRotation.y;
        packed_env.envMapRotation.z =
            env_backend.domainRandomization.envRotation.z;
        packed_env.envMapRotation.w =
            env_backend.domainRandomization.envRotation.w;

        packed_env.lightFilterAndEnvIdx.x =
            env_backend.domainRandomization.lightFilter.x;
        packed_env.lightFilterAndEnvIdx.y =
            env_backend.domainRandomization.lightFilter.y;
        packed_env.lightFilterAndEnvIdx.z =
            env_backend.domainRandomization.lightFilter.z;
        packed_env.lightFilterAndEnvIdx.w =
            glm::uintBitsToFloat(env_backend.domainRandomization.envMapIdx);
    }

    batch_backend.renderInputStaging.flush(dev);

    VkBufferCopy param_copy;
    param_copy.srcOffset = 0;
    param_copy.dstOffset = 0;
    param_copy.size = param_cfg_.totalParamBytes;

    dev.dt.cmdCopyBuffer(render_cmd,
                         batch_backend.renderInputStaging.buffer,
                         batch_backend.renderInputDev.buffer,
                         1, &param_copy);


    auto submitCmd = [&]() {
        REQ_VK(dev.dt.endCommandBuffer(render_cmd));

        VkSubmitInfo render_submit {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
            nullptr,
            0,
            nullptr,
            nullptr,
            1,
            &batch_state.renderCmd,
            0,
            nullptr,
        };

        uint32_t swapchain_idx = 0;
        if (present_.has_value()) {
            render_submit.pSignalSemaphores = &batch_state.renderSignal;
            render_submit.signalSemaphoreCount = 1;

            swapchain_idx = present_->acquireNext(dev, batch_state.swapchainReady);
        }

        compute_queues_[cur_queue_].submit(
            dev, 1, &render_submit, batch_state.fence);

        if (present_.has_value()) {
            array present_wait_semas {
                batch_state.swapchainReady,
                batch_state.renderSignal,
            };
            present_->present(dev, swapchain_idx, compute_queues_[cur_queue_],
                              present_wait_semas.size(),
                              present_wait_semas.data());
        }
    };

    VkMemoryBarrier comp_barrier;
    comp_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    comp_barrier.pNext = nullptr;
    comp_barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;
    comp_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    if (cfg_.adaptiveSampling) {
        dev.dt.cmdFillBuffer(render_cmd,
            batch_backend.fb.outputs[batch_backend.fb.adaptiveIdx].buffer,
            0, fb_cfg_.adaptiveBytes, 0);

        dev.dt.cmdFillBuffer(render_cmd,
            batch_backend.fb.outputs[batch_backend.fb.hdrIdx].buffer,
            0, fb_cfg_.hdrBytes, 0);

        if (cfg_.tonemap) {
            dev.dt.cmdFillBuffer(render_cmd,
                batch_backend.fb.outputs[batch_backend.fb.illuminanceIdx].buffer,
                0, fb_cfg_.illuminanceBytes, 0);
        }

        if (cfg_.auxiliaryOutputs) {
            dev.dt.cmdFillBuffer(render_cmd,
                batch_backend.fb.outputs[batch_backend.fb.normalIdx].buffer,
                0, fb_cfg_.normalBytes, 0);

            dev.dt.cmdFillBuffer(render_cmd,
                batch_backend.fb.outputs[batch_backend.fb.albedoIdx].buffer,
                0, fb_cfg_.albedoBytes, 0);
        }

        VkMemoryBarrier zero_barrier;
        zero_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        zero_barrier.pNext = nullptr;
        zero_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        zero_barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        dev.dt.cmdPipelineBarrier(render_cmd,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  1, &zero_barrier, 0, nullptr, 0, nullptr);

        int cur_tile_idx = 0;

        auto writeTile = [&cur_tile_idx, &batch_state](int batch_idx,
                                                       int tile_x,
                                                       int tile_y,
                                                       int sample_offset) {
            InputTile &cur_tile =
                batch_state.tileInputPtr[cur_tile_idx];
            cur_tile.batchIdx = batch_idx;
            cur_tile.xOffset = tile_x;
            cur_tile.yOffset = tile_y;
            cur_tile.sampleOffset = sample_offset;

            cur_tile_idx++;
        };

        for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize; batch_idx++) {
            for (int tile_y = 0; tile_y < (int)fb_cfg_.numTilesTall; tile_y++) {
                for (int tile_x = 0; tile_x < (int)fb_cfg_.numTilesWide;
                     tile_x++) {
                    for (int sample_idx = 0; sample_idx < (int)cfg_.spp;
                         sample_idx +=
                            VulkanConfig::adaptive_samples_per_thread) {
                        writeTile(batch_idx, tile_x, tile_y, sample_idx);
                    }
                }
            }
        }

        batch_backend.adaptiveInput->flush(dev);

        int num_tiles = cur_tile_idx;

        // FIXME: xy -> yz, z -> x (larger launch limit in X)
        dev.dt.cmdDispatch(render_cmd, num_tiles, 1, 1);

        auto adaptiveReadback = [&]() {
            VkMemoryBarrier readback_barrier;
            readback_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            readback_barrier.pNext = nullptr;
            readback_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            readback_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            dev.dt.cmdPipelineBarrier(
                render_cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                1, &readback_barrier, 0, nullptr, 0, nullptr);

            VkBufferCopy copy_info;
            copy_info.srcOffset = 0;
            copy_info.dstOffset = 0;
            copy_info.size = fb_cfg_.adaptiveBytes;

            dev.dt.cmdCopyBuffer(render_cmd,
                batch_backend.fb.outputs[batch_backend.fb.adaptiveIdx].buffer,
                batch_backend.fb.adaptiveReadback->buffer,
                1, &copy_info);

            VkBufferCopy exposure_copy_info;
            exposure_copy_info.srcOffset = 0;
            exposure_copy_info.dstOffset = 0;
            exposure_copy_info.size = fb_cfg_.illuminanceBytes;

            dev.dt.cmdCopyBuffer(render_cmd,
                batch_backend.fb.outputs[batch_backend.fb.illuminanceIdx].buffer,
                batch_backend.fb.exposureReadback->buffer,
                1, &exposure_copy_info);
        };

        adaptiveReadback();

        submitCmd();
        waitForFenceInfinitely(dev, batch_state.fence);
        resetFence(dev, batch_state.fence);

        constexpr float norm_variance_threshold = 5e-4;
        constexpr int max_adaptive_iters = 10000;

        auto processAdaptiveTile = [&](int batch_idx, int tile_x, int tile_y) {
            uint32_t linear_idx =
                batch_idx * fb_cfg_.numTilesTall * fb_cfg_.numTilesWide +
                    tile_y * fb_cfg_.numTilesWide + tile_x;
            AdaptiveTile &tile = batch_state.adaptiveReadbackPtr[linear_idx];
            float &tile_illuminance =
                ((float *)batch_backend.fb.exposureReadback->ptr)[linear_idx];

            float variance = tile.tileVarianceM2 / tile.numSamples;

            float norm_variance = tile_illuminance == 0.f ? 0.f :
                tile.tileMean / tile_illuminance;

            return;

            if ((norm_variance == 0.f && tile.numSamples < cfg_.spp * 10) ||
                norm_variance > norm_variance_threshold) {

                for (int sample_idx = 0; sample_idx < (int)cfg_.spp;
                     sample_idx +=
                        VulkanConfig::adaptive_samples_per_thread) {
                    writeTile(batch_idx, tile_x, tile_y, sample_idx);
                }
            }
        };

        for (int adaptive_iter = 0; adaptive_iter < max_adaptive_iters;
             adaptive_iter++) {
            cur_tile_idx = 0;
            frame_counter_ += cfg_.batchSize;

            for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize;
                 batch_idx++) {
                for (int tile_y = 0; tile_y < (int)fb_cfg_.numTilesTall;
                     tile_y++) {
                    for (int tile_x = 0; tile_x < (int)fb_cfg_.numTilesWide;
                         tile_x++) {
                        processAdaptiveTile(batch_idx, tile_x, tile_y);
                    }
                }
            }

            num_tiles = cur_tile_idx;

            if (num_tiles == 0) {
                break;
            }

            startRenderSetup();

            batch_backend.adaptiveInput->flush(dev);

            // Pipeline barrier on memory reads / writes from previous iter
            // Is this necessary given fence?
            dev.dt.cmdPipelineBarrier(render_cmd,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      0,
                                      1, &comp_barrier,
                                      0, nullptr, 0, nullptr);

            dev.dt.cmdDispatch(render_cmd, num_tiles, 1, 1);

            adaptiveReadback();

            submitCmd();
            waitForFenceInfinitely(dev, batch_state.fence);
            resetFence(dev, batch_state.fence);
        }

        startRenderSetup();
    } else {
        VkBufferMemoryBarrier param_barrier;
        param_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        param_barrier.pNext = nullptr;
        param_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        param_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        param_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        param_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        param_barrier.buffer = batch_backend.renderInputDev.buffer;
        param_barrier.offset = 0;
        param_barrier.size = param_cfg_.totalParamBytes;

        dev.dt.cmdPipelineBarrier(render_cmd,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0,
                                  0, nullptr,
                                  1, &param_barrier,
                                  0, nullptr);

        dev.dt.cmdDispatch(
            render_cmd,
            launch_size_.x,
            launch_size_.y,
            launch_size_.z);

        frame_counter_ += cfg_.batchSize;
    }

    if (cfg_.denoise && rcfg_.mode == RenderMode::PathTracer) {
        submitCmd();
        waitForFenceInfinitely(dev, batch_state.fence);
        resetFence(dev, batch_state.fence);

        denoiser_->denoise(dev, fb_cfg_, batch_state.cmdPool, 
            render_cmd, batch_state.fence, compute_queues_[cur_queue_],
            batch_backend.fb.outputs[batch_backend.fb.hdrIdx],
            batch_backend.fb.outputs[batch_backend.fb.albedoIdx],
            batch_backend.fb.outputs[batch_backend.fb.normalIdx],
            cfg_.batchSize);

        startRenderSetup();
    }

    if (cfg_.tonemap) {
        // comp barrier and stages here are worst case
        // assuming we're denoising
        dev.dt.cmdPipelineBarrier(render_cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
            &comp_barrier, 0, nullptr, 0, nullptr);

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.exposure.hdl);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.exposure.layout, 0, 1,
                                     &batch_state.exposureSet,
                                     0, nullptr);

        dev.dt.cmdDispatch(
            render_cmd,
            1,
            1,
            cfg_.batchSize);

        dev.dt.cmdPipelineBarrier(render_cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
            &comp_barrier, 0, nullptr, 0, nullptr);

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.tonemap.hdl);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.tonemap.layout, 0, 1,
                                     &batch_state.tonemapSet,
                                     0, nullptr);

        dev.dt.cmdDispatch(
            render_cmd,
            launch_size_.x,
            launch_size_.y,
            launch_size_.z);
    }

    submitCmd();

    batch_backend.curBuffer = (batch_backend.curBuffer + 1) & 1;
    cur_queue_ = (cur_queue_ + 1) & 1;
}

Probe *VulkanBackend::bakeProbe(glm::vec3 position, RenderBatch &batch)
{
    // First render
    ProbeGenState::State &batch_backend = probe_gen_->state;
    Environment *envs = batch.getEnvironments();
    PerBatchState &batch_state = batch_backend.state;
    VkCommandBuffer render_cmd = batch_state.renderCmd;

    auto prev_cur_buffer = batch_backend.curBuffer;
    batch_backend.curBuffer = 0;

    auto startRenderSetup = [&]() {
        REQ_VK(dev.dt.resetCommandPool(dev.hdl, batch_state.cmdPool, 0));

        VkCommandBufferBeginInfo begin_info {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        REQ_VK(dev.dt.beginCommandBuffer(render_cmd, &begin_info));

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.bake.hdl);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.bake.layout, 0, 1,
                                     &batch_state.rtSets[batch_backend.curBuffer],
                                     0, nullptr);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.bake.layout, 2, 1,
                                     &cur_env_maps_->descSet.hdl,
                                     0, nullptr);

        shared_scene_state_.lock.lock();
        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.bake.layout, 1, 1,
                                     &shared_scene_state_.descSet, 0, nullptr);
        shared_scene_state_.lock.unlock();
    };

    startRenderSetup();

    // TLAS build
    for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize; batch_idx++) {
        const Environment &env = envs[batch_idx];

        if (env.isDirty()) {
            VulkanEnvironment &env_backend =
                *(VulkanEnvironment *)(env.getBackend());
            const VulkanScene &scene =
                *static_cast<const VulkanScene *>(env.getScene().get());

            env_backend.tlas.build(dev, alloc, env.getInstances(),
                                   env.getTransforms(), env.getInstanceFlags(),
                                   scene.objectInfo, scene.blases, render_cmd);

            env.clearDirty();
        }
    }

    VkMemoryBarrier tlas_barrier;
    tlas_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    tlas_barrier.pNext = nullptr;
    tlas_barrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlas_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    dev.dt.cmdPipelineBarrier(render_cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
        &tlas_barrier, 0, nullptr, 0, nullptr);

    uint32_t inst_offset = 0;
    uint32_t material_offset = 0;
    uint32_t light_offset = 0;

    // Write environment data into linear buffers
    for (int batch_idx = 0; batch_idx < (int)cfg_.batchSize; batch_idx++) {
        Environment &env = envs[batch_idx];
        VulkanEnvironment &env_backend =
            *static_cast<VulkanEnvironment *>(env.getBackend());
        const VulkanScene &scene_backend =
            *static_cast<const VulkanScene *>(env.getScene().get());

        PackedEnv &packed_env = batch_state.envPtr[batch_idx];

        Camera new_cam = env.getCamera();
        new_cam.view = glm::normalize(glm::vec3(1.0f, -0.2f, 1.0f));
        new_cam.up = glm::vec3(0.0f, 1.0f, 0.0f);
        new_cam.right = glm::cross(new_cam.view, new_cam.up);

        glm::vec3 ms_scene_center = envs->getScene()->envInit.defaultBBox.pMin +
            envs->getScene()->envInit.defaultBBox.pMax;

        // glm::vec3 pos = scene_center + new_cam.view * 13.0f;

        // glm::vec3 pos = glm::vec3(-3.21665, 0.676769, -9.12807);
        glm::vec3 pos = position;

        packed_env.cam = packCamera(new_cam);
        packed_env.cam.posAndTanFOV = glm::vec4(pos, packed_env.cam.posAndTanFOV.w);
        packed_env.prevCam = packCamera(env_backend.prevCam);
        packed_env.data.x = scene_backend.sceneID->getID();

        // Set prevCam for next iteration
        env_backend.prevCam = env.getCamera();

        const auto &env_transforms = env.getTransforms();
        uint32_t num_instances = env.getNumInstances();
        memcpy(&batch_state.transformPtr[inst_offset], env_transforms.data(),
               sizeof(InstanceTransform) * num_instances);
        inst_offset += num_instances;

        const auto &env_mats = env.getInstanceMaterials();

        memcpy(&batch_state.materialPtr[material_offset], env_mats.data(),
               env_mats.size() * sizeof(uint32_t));

        packed_env.data.y = material_offset;
        material_offset += env_mats.size();

        memcpy(&batch_state.lightPtr[light_offset], env_backend.lights.data(),
               env_backend.lights.size() * sizeof(PackedLight));

        packed_env.data.z = light_offset;
        packed_env.data.w = env_backend.lights.size();
        light_offset += env_backend.lights.size();

        packed_env.tlasAddr = env_backend.tlas.tlasStorageDevAddr;
        //packed_env.reservoirGridAddr = env_backend.reservoirGrid.devAddr;
        packed_env.reservoirGridAddr = 0;

        packed_env.envMapRotation.x =
            env_backend.domainRandomization.envRotation.x;
        packed_env.envMapRotation.y =
            env_backend.domainRandomization.envRotation.y;
        packed_env.envMapRotation.z =
            env_backend.domainRandomization.envRotation.z;
        packed_env.envMapRotation.w =
            env_backend.domainRandomization.envRotation.w;

        packed_env.lightFilterAndEnvIdx.x =
            env_backend.domainRandomization.lightFilter.x;
        packed_env.lightFilterAndEnvIdx.y =
            env_backend.domainRandomization.lightFilter.y;
        packed_env.lightFilterAndEnvIdx.z =
            env_backend.domainRandomization.lightFilter.z;
        packed_env.lightFilterAndEnvIdx.w =
            glm::uintBitsToFloat(env_backend.domainRandomization.envMapIdx);
    }

    batch_backend.renderInputStaging.flush(dev);

    VkBufferCopy param_copy;
    param_copy.srcOffset = 0;
    param_copy.dstOffset = 0;
    param_copy.size = param_cfg_.totalParamBytes;

    dev.dt.cmdCopyBuffer(render_cmd,
                         batch_backend.renderInputStaging.buffer,
                         batch_backend.renderInputDev.buffer,
                         1, &param_copy);


    auto submitCmd = [&]() {
        REQ_VK(dev.dt.endCommandBuffer(render_cmd));

        VkSubmitInfo render_submit {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
            nullptr,
            0,
            nullptr,
            nullptr,
            1,
            &batch_state.renderCmd,
            0,
            nullptr,
        };

        uint32_t swapchain_idx = 0;
        if (present_.has_value()) {
            render_submit.pSignalSemaphores = &batch_state.renderSignal;
            render_submit.signalSemaphoreCount = 1;

            swapchain_idx = present_->acquireNext(dev, batch_state.swapchainReady);
        }

        compute_queues_[cur_queue_].submit(
            dev, 1, &render_submit, batch_state.fence);

        if (present_.has_value()) {
            array present_wait_semas {
                batch_state.swapchainReady,
                batch_state.renderSignal,
            };
            present_->present(dev, swapchain_idx, compute_queues_[cur_queue_],
                              present_wait_semas.size(),
                              present_wait_semas.data());
        }
    };

    VkMemoryBarrier comp_barrier;
    comp_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    comp_barrier.pNext = nullptr;
    comp_barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;
    comp_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkBufferMemoryBarrier param_barrier;
    param_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    param_barrier.pNext = nullptr;
    param_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    param_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    param_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    param_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    param_barrier.buffer = batch_backend.renderInputDev.buffer;
    param_barrier.offset = 0;
    param_barrier.size = param_cfg_.totalParamBytes;

    dev.dt.cmdPipelineBarrier(render_cmd,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0,
                              0, nullptr,
                              1, &param_barrier,
                              0, nullptr);

    VkMemoryBarrier zero_barrier;
    zero_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    zero_barrier.pNext = nullptr;
    zero_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zero_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    dev.dt.cmdFillBuffer(render_cmd,
        batch_backend.fb.outputs[batch_backend.fb.outputIdx].buffer,
        0, probe_width_ * probe_height_ * 4 * sizeof(half), 0);

    dev.dt.cmdFillBuffer(render_cmd,
        batch_backend.fb.outputs[batch_backend.fb.hdrIdx].buffer,
        0, probe_width_ * probe_height_ * 4 * sizeof(float), 0);

    dev.dt.cmdPipelineBarrier(render_cmd,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0,
                              1, &zero_barrier, 0, nullptr, 0, nullptr);

    RTPushConstant push_const {
        frame_counter_,
    };

    for (int i = 0; i < (int)cfg_.spp; i += 64) {
        push_const.probeIdx = i * 64;

        dev.dt.cmdPushConstants(render_cmd, pipelines_.bake.layout,
                                VK_SHADER_STAGE_COMPUTE_BIT,
                                0,
                                sizeof(RTPushConstant),
                                &push_const);



        dev.dt.cmdDispatch(
            render_cmd,
            launch_size_.x,
            launch_size_.y,
            launch_size_.z);

        std::cout << "Submit" << std::endl;
        submitCmd();
        std::cout << "Executed" << std::endl;

        waitForFenceInfinitely(dev, batch_state.fence);
        resetFence(dev, batch_state.fence);

        startRenderSetup();
    }
    frame_counter_ += cfg_.batchSize;

    if (/*cfg_.denoise*/ false) {
        submitCmd();
        waitForFenceInfinitely(dev, batch_state.fence);
        resetFence(dev, batch_state.fence);

        denoiser_->denoise(dev, fb_cfg_, batch_state.cmdPool, 
            render_cmd, batch_state.fence, compute_queues_[cur_queue_],
            batch_backend.fb.outputs[batch_backend.fb.hdrIdx],
            batch_backend.fb.outputs[batch_backend.fb.albedoIdx],
            batch_backend.fb.outputs[batch_backend.fb.normalIdx],
            cfg_.batchSize);

        startRenderSetup();
    }

#if 0
    if (cfg_.tonemap) {
        // comp barrier and stages here are worst case
        // assuming we're denoising
        dev.dt.cmdPipelineBarrier(render_cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
            &comp_barrier, 0, nullptr, 0, nullptr);

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.exposure.hdl);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.exposure.layout, 0, 1,
                                     &batch_state.exposureSet,
                                     0, nullptr);

        dev.dt.cmdDispatch(
            render_cmd,
            1,
            1,
            cfg_.batchSize);

        dev.dt.cmdPipelineBarrier(render_cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
            &comp_barrier, 0, nullptr, 0, nullptr);

        dev.dt.cmdBindPipeline(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               pipelines_.tonemap.hdl);

        dev.dt.cmdBindDescriptorSets(render_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                     pipelines_.tonemap.layout, 0, 1,
                                     &batch_state.tonemapSet,
                                     0, nullptr);

        dev.dt.cmdDispatch(
            render_cmd,
            launch_size_.x,
            launch_size_.y,
            launch_size_.z);
    }
#endif

    std::cout << "Submitting..." << std::endl;
    submitCmd();
    std::cout << "Executed" << std::endl;
    waitForFenceInfinitely(dev, batch_state.fence);
    resetFence(dev, batch_state.fence);

    // Make texture data now
    VkCommandPool cmd_pool = makeCmdPool(dev, dev.computeQF);
    VkCommandBuffer cmd = makeCmdBuffer(dev, cmd_pool);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQ_VK(dev.dt.beginCommandBuffer(cmd, &begin_info));

    Probe *probe = new Probe();
    makeProbeTextureData(probe, cmd);

    REQ_VK(dev.dt.endCommandBuffer(cmd));

    { // Submit command buffer
        VkSubmitInfo render_submit {};
        render_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render_submit.waitSemaphoreCount = 0;
        render_submit.pWaitSemaphores = nullptr;
        render_submit.pWaitDstStageMask = nullptr;
        render_submit.commandBufferCount = 1;
        render_submit.pCommandBuffers = &cmd;

        VkFence fence = makeFence(dev);

        compute_queues_[0].submit(dev, 1, &render_submit, fence);

        // dev.dt.deviceWaitIdle();

        waitForFenceInfinitely(dev, fence);

        dev.dt.destroyFence(dev.hdl, fence, nullptr);
        dev.dt.destroyCommandPool(dev.hdl, cmd_pool, nullptr);
    }

    batch_backend.curBuffer = prev_cur_buffer;

    return probe;
    // batch_backend.curBuffer = (batch_backend.curBuffer + 1) & 1;
    // cur_queue_ = (cur_queue_ + 1) & 1;
}

static const char *PROBES_BIN_PATH = "probes.bin";

void VulkanBackend::bake(RenderBatch &batch)
{
    probe_width_ = PROBE_WIDTH;
    probe_height_ = PROBE_HEIGHT;

    uint32_t probe_count = PROBE_DIM.x * PROBE_DIM.y * PROBE_DIM.z;


    if (std::filesystem::exists(PROBES_BIN_PATH)) {
        std::cout << "Can just load probes!" << std::endl;

        ifstream file(PROBES_BIN_PATH);

        glm::ivec3 dim;
        file.read((char *)&dim, sizeof(dim));

        probe_count = dim.x * dim.y * dim.z;

        assert(dim.x == PROBE_DIM.x && dim.y == PROBE_DIM.y && dim.z == PROBE_DIM.z);

        while (!file.eof()) {
            probes_.push_back(deserializeProbeFromFile(file));
        }
    }

        std::cout << "Need to generate the probes!" << std::endl;

        // Allocate resources to render a probe
        probe_gen_ = new ProbeGenState(makeProbeGenState());

        VulkanBatch &batch_backend = *getVkBatch(batch);

        Environment *envs = batch.getEnvironments();
        glm::vec3 min = envs->getScene()->envInit.defaultBBox.pMin;
        glm::vec3 max = envs->getScene()->envInit.defaultBBox.pMax;
        glm::vec3 center = (min + max) * 0.5f;

        std::cout << "min : " << glm::to_string(min) << std::endl;
        std::cout << "max : " << glm::to_string(max) << std::endl;

        float right = (max.x - min.x) / (float)(PROBE_DIM.x - 1);
        float up = (max.y - min.y) / (float)(PROBE_DIM.y - 1);
        float forward = (max.z - min.z) / (float)(PROBE_DIM.z - 1);

        std::vector<glm::vec3> positions;

        uint32_t left = probe_count - probes_.size();
        std::cout << "left = " << left << std::endl;

#if 1
        uint32_t start = probes_.size();
        for (int i = 0; i < left; ++i) {
            int idx = start + i;

            int x = idx % PROBE_DIM.x;
            int y = (idx/PROBE_DIM.x) % PROBE_DIM.y;
            int z = (idx/(PROBE_DIM.x * PROBE_DIM.y)) % PROBE_DIM.z;

            glm::vec3 pos = glm::vec3(right * x, up * y, forward * z) + min;
            positions.push_back(pos);
        }

#if 0
        for (int z = 0; z < PROBE_DIM.z; ++z) {
            for (int y = 0; y < PROBE_DIM.y; ++y) {
                for (int x = 0; x < PROBE_DIM.x; ++x) {
                    glm::vec3 pos = glm::vec3(right * x, up * y, forward * z) + min;
                    positions.push_back(pos);
                }
            }
        }
#endif
#endif

        // Save these to a file

        /*
         * 4-bytes dimx, 4-bytes dimy, 4-bytes dimz,
         * 4-bytes probe size, probe bytes
         * 4-bytes probe size, probe bytes
         * 4-bytes probe size, probe bytes
         * 4-bytes probe size, probe bytes
         * ...
         */
        ofstream file(PROBES_BIN_PATH, std::ios::app);
        file.write((const char *)&PROBE_DIM, sizeof(PROBE_DIM));


        std::cout << "Baking" << std::endl;
        for (int i = 0; i < positions.size(); ++i) {
            probes_.push_back(bakeProbe(positions[i], batch));
            std::cout << "Finished " << (start+i+1) << " / " << positions.size() << " probes (at " << glm::to_string(positions[i]) << ")" << std::endl;

            serializeProbeToFile(file, i);
            file.flush();
        }
        std::cout << "Finished baking : " << probes_.size() << std::endl;

    // Making descriptor sets
    makeProbeDescriptorSet();
}

Probe *VulkanBackend::deserializeProbeFromFile(std::ifstream &file) {
    // First create buffer
    VkDeviceSize size;
    file.read((char *)&size, sizeof(size));

    HostBuffer staging = alloc.makeStagingBuffer(size);
    file.read((char *)staging.ptr, size);




    VkCommandPool cmd_pool = makeCmdPool(dev, dev.computeQF);
    VkCommandBuffer cmd = makeCmdBuffer(dev, cmd_pool);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQ_VK(dev.dt.beginCommandBuffer(cmd, &begin_info));

    Probe *probe = new Probe();



    int width = probe_width_, height = probe_height_;
    size_t byte_size = 4 * sizeof(uint32_t) * width * height;

    Probe &pr = *probe;

    // Make the texture object
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    auto [tx, req] = alloc.makeTexture2D(width, height, 1, format);

    optional<VkDeviceMemory> backing_opt = alloc.alloc(req.size);

    pr.size = req.size;

    if (!backing_opt.has_value()) {
        cerr << "Not enough memory for probe texture" << endl;
        fatalExit();
    }

    VkDeviceMemory backing = backing_opt.value();

    // Bind the backing to the image object
    dev.dt.bindImageMemory(dev.hdl, tx.image, backing, 0);

    VkImageViewCreateInfo view_info;
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = nullptr;
    view_info.flags = 0;
    view_info.image = tx.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageView view;
    REQ_VK(dev.dt.createImageView(dev.hdl, &view_info, nullptr, &view));

    { // Prepare image for transfer
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    { // Do buffer to image copy
        VkBufferImageCopy copy_info = {};
        copy_info.bufferOffset = 0;
        copy_info.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy_info.imageSubresource.mipLevel = 0;
        copy_info.imageSubresource.baseArrayLayer = 0;
        copy_info.imageSubresource.layerCount = 1;
        copy_info.imageExtent.width = width;
        copy_info.imageExtent.height = height;
        copy_info.imageExtent.depth = 1;

        dev.dt.cmdCopyBufferToImage(
            cmd, staging.buffer, tx.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_info);
    }

    { // Prepare image for shader reading
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    pr.tx = tx;
    pr.view = view;
    pr.backing = backing;


    REQ_VK(dev.dt.endCommandBuffer(cmd));

    { // Submit command buffer
        VkSubmitInfo render_submit {};
        render_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render_submit.waitSemaphoreCount = 0;
        render_submit.pWaitSemaphores = nullptr;
        render_submit.pWaitDstStageMask = nullptr;
        render_submit.commandBufferCount = 1;
        render_submit.pCommandBuffers = &cmd;

        VkFence fence = makeFence(dev);

        compute_queues_[0].submit(dev, 1, &render_submit, fence);

        waitForFenceInfinitely(dev, fence);

        dev.dt.destroyFence(dev.hdl, fence, nullptr);
        dev.dt.destroyCommandPool(dev.hdl, cmd_pool, nullptr);
    }

    return probe;
}

void VulkanBackend::serializeProbeToFile(std::ofstream &file, int idx) {
    Probe &pr = *probes_[idx];

    VkCommandPool cmdpool = makeCmdPool(dev, dev.computeQF);
    VkCommandBuffer cmd = makeCmdBuffer(dev, cmdpool);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQ_VK(dev.dt.beginCommandBuffer(cmd, &begin_info));


    { // Prepare image for transfer
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            0,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            pr.tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    HostBuffer staging = alloc.makeStagingBuffer(pr.size);

    { // Do buffer to image copy
        VkBufferImageCopy copy_info = {};
        copy_info.bufferOffset = 0;
        copy_info.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy_info.imageSubresource.mipLevel = 0;
        copy_info.imageSubresource.baseArrayLayer = 0;
        copy_info.imageSubresource.layerCount = 1;
        copy_info.imageExtent.width = PROBE_WIDTH;
        copy_info.imageExtent.height = PROBE_HEIGHT;
        copy_info.imageExtent.depth = 1;

        dev.dt.cmdCopyImageToBuffer(
            cmd, pr.tx.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer,
            1, &copy_info);
    }

    { // Prepare image for shader reading
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            pr.tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    REQ_VK(dev.dt.endCommandBuffer(cmd));

    { // Submit command buffer
        VkSubmitInfo render_submit {};
        render_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render_submit.waitSemaphoreCount = 0;
        render_submit.pWaitSemaphores = nullptr;
        render_submit.pWaitDstStageMask = nullptr;
        render_submit.commandBufferCount = 1;
        render_submit.pCommandBuffers = &cmd;

        VkFence fence = makeFence(dev);

        compute_queues_[0].submit(dev, 1, &render_submit, fence);

        waitForFenceInfinitely(dev, fence);

        dev.dt.destroyFence(dev.hdl, fence, nullptr);
        dev.dt.destroyCommandPool(dev.hdl, cmdpool, nullptr);
    }

    // Now serialize those bytes
    file.write((const char *)&pr.size, sizeof(pr.size));
    file.write((const char *)staging.ptr, pr.size);
}

void VulkanBackend::makeProbeTextureData(Probe *probe, VkCommandBuffer &cmd)
{
    int width = probe_width_, height = probe_height_;
    size_t byte_size = 4 * sizeof(uint32_t) * width * height;

    Probe &pr = *probe;

    // Make the texture object
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    auto [tx, req] = alloc.makeTexture2D(width, height, 1, format);

    optional<VkDeviceMemory> backing_opt = alloc.alloc(req.size);

    pr.size = req.size;

    if (!backing_opt.has_value()) {
        cerr << "Not enough memory for probe texture" << endl;
        fatalExit();
    }

    VkDeviceMemory backing = backing_opt.value();

    // Bind the backing to the image object
    dev.dt.bindImageMemory(dev.hdl, tx.image, backing, 0);

    VkImageViewCreateInfo view_info;
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = nullptr;
    view_info.flags = 0;
    view_info.image = tx.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageView view;
    REQ_VK(dev.dt.createImageView(dev.hdl, &view_info, nullptr, &view));

    { // Prepare image for transfer
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    { // Do buffer to image copy
        VkBufferImageCopy copy_info = {};
        copy_info.bufferOffset = 0;
        copy_info.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy_info.imageSubresource.mipLevel = 0;
        copy_info.imageSubresource.baseArrayLayer = 0;
        copy_info.imageSubresource.layerCount = 1;
        copy_info.imageExtent.width = width;
        copy_info.imageExtent.height = height;
        copy_info.imageExtent.depth = 1;

        dev.dt.cmdCopyBufferToImage(
            cmd, probe_gen_->state.fb.outputs[1].buffer, tx.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_info);
    }

    { // Prepare image for shader reading
        VkImageMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            tx.image,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        dev.dt.cmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    pr.tx = tx;
    pr.view = view;
    pr.backing = backing;
}

// TODO: Move to double precision later
void VulkanBackend::makeProbeDescriptorSet()
{
#if 0
    // For transfer and stuff
    VkCommandPool cmd_pool = makeCmdPool(dev, dev.computeQF);
    VkCommandBuffer cmd = makeCmdBuffer(dev, cmd_pool);

    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQ_VK(dev.dt.beginCommandBuffer(cmd, &begin_info));

    for (auto &pr : probes_) {
        makeProbeTextureData(pr, cmd);
    }

    REQ_VK(dev.dt.endCommandBuffer(cmd));

    { // Submit command buffer
        VkSubmitInfo render_submit {};
        render_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        render_submit.waitSemaphoreCount = 0;
        render_submit.pWaitSemaphores = nullptr;
        render_submit.pWaitDstStageMask = nullptr;
        render_submit.commandBufferCount = 1;
        render_submit.pCommandBuffers = &cmd;

        VkFence fence = makeFence(dev);

        compute_queues_[0].submit(dev, 1, &render_submit, fence);

        waitForFenceInfinitely(dev, fence);

        dev.dt.destroyFence(dev.hdl, fence, nullptr);
        dev.dt.destroyCommandPool(dev.hdl, cmd_pool, nullptr);
    }
#endif
    
    probe_pool_ = new FixedDescriptorPool(dev, render_state_.rt, 3, 5);
    VkDescriptorSet dset = probe_pool_->makeSet();
    { // Make descriptor set for the probe textures
        DescriptorUpdates update(probes_.size());

        std::vector<VkDescriptorImageInfo> infos;
        infos.resize(probes_.size());

        for (int i = 0; i < probes_.size(); ++i) {
            infos[i] = { VK_NULL_HANDLE, probes_[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            update.textures(dset, &infos[i], 1, 0, i);
        }

        update.update(dev);
    }
    
    probe_dset_ = dset;
}

void VulkanBackend::waitForBatch(RenderBatch &batch)
{
    auto &batch_backend = *getVkBatch(batch);

    VkFence fence = batch_backend.state.fence;
    assert(fence != VK_NULL_HANDLE);
    waitForFenceInfinitely(dev, fence);
    resetFence(dev, fence);
}

half *VulkanBackend::getOutputPointer(RenderBatch &batch)
{
    auto &batch_backend = *getVkBatch(batch);

    return batch_backend.state.outputBuffer;
}

half *VulkanBackend::getBakeOutputPointer()
{
    auto &batch_backend = probe_gen_->state;

    return batch_backend.state.outputBuffer;
}

AuxiliaryOutputs VulkanBackend::getAuxiliaryOutputs(RenderBatch &batch)
{
    auto &batch_backend = *getVkBatch(batch);
    return {
        batch_backend.state.normalBuffer,
        batch_backend.state.albedoBuffer,
    };
}

}
}
