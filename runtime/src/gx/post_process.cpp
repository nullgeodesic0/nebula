#include "galaxy/gx/post_process.h"
#include "galaxy/gx/renderer_d3d12.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <utility>

// SMAA lookup textures (third_party/smaa, MIT).
#include "AreaTex.h"
#include "SearchTex.h"

// Pass shaders, compiled from post_process.hlsl by fxc at build time.
#include "post_fullscreen_vs.h"
#include "post_smaa_edges_vs.h"
#include "post_smaa_edges_ps_low.h"
#include "post_smaa_edges_ps_medium.h"
#include "post_smaa_edges_ps_high.h"
#include "post_smaa_edges_ps_ultra.h"
#include "post_smaa_weights_vs_low.h"
#include "post_smaa_weights_vs_medium.h"
#include "post_smaa_weights_vs_high.h"
#include "post_smaa_weights_vs_ultra.h"
#include "post_smaa_weights_ps_low.h"
#include "post_smaa_weights_ps_medium.h"
#include "post_smaa_weights_ps_high.h"
#include "post_smaa_weights_ps_ultra.h"
#include "post_smaa_blend_vs.h"
#include "post_smaa_blend_ps.h"
#include "post_fsr_easu_ps.h"
#include "post_fsr_rcas_ps.h"
#include "post_resample_ps.h"
#include "post_fxaa_ps.h"
#include "post_bloom_down_ps.h"
#include "post_bloom_up_ps.h"
#include "post_enhance_ps.h"
#include "post_ssao_ao_ps.h"
#include "post_ssao_blur_ps.h"
#include "post_ssao_apply_ps.h"
#include "post_dof_blur_ps.h"
#include "post_dof_apply_ps.h"
#include "post_motion_blur_ps.h"
#include "post_ssr_ps.h"

namespace galaxy::gx {
namespace {

using Microsoft::WRL::ComPtr;

struct ShaderBytes {
    const void* data;
    std::size_t size;
};

template <std::size_t N>
constexpr ShaderBytes bytes(const BYTE (&code)[N]) noexcept {
    return {code, N};
}

constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kEdgesFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
// Slots 0-3: SMAA/AA/scaling targets. 4: final_. 5-9: bloom down levels.
// 10-13: bloom up levels.
// 14-15: ambient occlusion. 16: depth of field.
constexpr unsigned kRtvSlots = 20u;
constexpr unsigned kSsaoAoSlot = 14u;
constexpr unsigned kSsaoBlurSlot = 15u;
constexpr unsigned kSceneTempSlot = 16u;
// R ambient occlusion, G contact shadow visibility.
constexpr DXGI_FORMAT kSsaoFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr unsigned kFinalSlot = 4u;
constexpr unsigned kBloomDownSlot = 5u;
constexpr unsigned kBloomUpSlot = 10u;

std::uint32_t bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }

// FsrEasuCon with the input viewport equal to the input resource.
void easu_constants(std::uint32_t (&con0)[4], std::uint32_t (&con1)[4],
                    std::uint32_t (&con2)[4], std::uint32_t (&con3)[4],
                    float in_w, float in_h, float out_w, float out_h) noexcept {
    con0[0] = bits(in_w / out_w);
    con0[1] = bits(in_h / out_h);
    con0[2] = bits(0.5f * in_w / out_w - 0.5f);
    con0[3] = bits(0.5f * in_h / out_h - 0.5f);
    con1[0] = bits(1.0f / in_w);
    con1[1] = bits(1.0f / in_h);
    con1[2] = bits(1.0f / in_w);
    con1[3] = bits(-1.0f / in_h);
    con2[0] = bits(-1.0f / in_w);
    con2[1] = bits(2.0f / in_h);
    con2[2] = bits(1.0f / in_w);
    con2[3] = bits(2.0f / in_h);
    con3[0] = bits(0.0f);
    con3[1] = bits(4.0f / in_h);
    con3[2] = 0u;
    con3[3] = 0u;
}

// FsrRcasCon: sharpness in stops, 0 = strongest. The 32-bit path reads con.x.
void rcas_constants(std::uint32_t (&con0)[4], unsigned sharpness_percent) noexcept {
    const float stops = 2.0f * (1.0f - std::clamp(sharpness_percent, 0u, 100u) / 100.0f);
    con0[0] = bits(std::exp2(-stops));
    con0[1] = 0u;
    con0[2] = 0u;
    con0[3] = 0u;
}

D3D12_SHADER_BYTECODE code(ShaderBytes shader) noexcept {
    return {shader.data, shader.size};
}

}  // namespace

PostProcessPlan plan_post_process(
    unsigned source_width, unsigned source_height,
    unsigned target_width, unsigned target_height,
    const PostProcessSettings& settings,
    bool smaa_available, bool fxaa_available) noexcept {
    PostProcessPlan plan{};
    if (source_width == 0u || source_height == 0u ||
        target_width == 0u || target_height == 0u) {
        return plan;
    }
    // Supersampled: at least 1.5 source pixels per window pixel on both axes.
    plan.supersampled =
        source_width * 2u >= target_width * 3u &&
        source_height * 2u >= target_height * 3u;
    const bool smaa_wanted = settings.aa == PostProcessSettings::Aa::kSmaa ||
        (settings.aa == PostProcessSettings::Aa::kAuto && !plan.supersampled);
    plan.aa_wanted = smaa_wanted || settings.aa == PostProcessSettings::Aa::kFxaa;
    plan.smaa = smaa_wanted && smaa_available;
    plan.fxaa = plan.aa_wanted && !plan.smaa && fxaa_available;
    const bool same_size =
        source_width == target_width && source_height == target_height;
    const bool enlarging = target_width >= source_width &&
        target_height >= source_height && !same_size;
    plan.easu = enlarging &&
        settings.upscaler == PostProcessSettings::Upscaler::kFsr1;
    plan.rcas = settings.sharpness > 0u;
    // The box filter handles reduction, mixed axes, bilinear enlargement and
    // the plain copy when neither EASU nor RCAS writes the window.
    plan.resample = !plan.easu && !(same_size && plan.rcas);
    plan.resample_to_target = plan.resample && !plan.rcas;
    return plan;
}

bool PostProcessor::create_pipeline(D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                    DXGI_FORMAT format, int stencil,
                                    ComPtr<ID3D12PipelineState>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_signature_.Get();
    pd.VS = vs;
    pd.PS = ps;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    // stencil: 0 none, 1 write 1 where the pixel shader keeps the pixel,
    // 2 draw only where the stencil is 1 (SMAA's edge mask).
    if (stencil != 0) {
        pd.DSVFormat = kStencilFormat;
        auto& ds = pd.DepthStencilState;
        ds.DepthEnable = FALSE;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = 0xFF;
        ds.StencilWriteMask = 0xFF;
        D3D12_DEPTH_STENCILOP_DESC op{};
        op.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        op.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        op.StencilPassOp = stencil == 1 ? D3D12_STENCIL_OP_REPLACE : D3D12_STENCIL_OP_KEEP;
        op.StencilFunc = stencil == 1 ? D3D12_COMPARISON_FUNC_ALWAYS
                                      : D3D12_COMPARISON_FUNC_EQUAL;
        ds.FrontFace = op;
        ds.BackFace = op;
    }
    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
}

// dest = dest * source for colour, alpha untouched: the ambient occlusion apply.
bool PostProcessor::create_pipeline_multiply(D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                             DXGI_FORMAT format,
                                             ComPtr<ID3D12PipelineState>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_signature_.Get();
    pd.VS = vs;
    pd.PS = ps;
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ZERO;
    rt.DestBlend = D3D12_BLEND_SRC_COLOR;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
}

// dest.rgb = lerp(dest.rgb, source.rgb, source.a), alpha untouched: the depth
// of field apply.
bool PostProcessor::create_pipeline_over(D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                         DXGI_FORMAT format,
                                         ComPtr<ID3D12PipelineState>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_signature_.Get();
    pd.VS = vs;
    pd.PS = ps;
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                               D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = format;
    pd.SampleDesc.Count = 1;
    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
}

bool PostProcessor::create_smaa_pipelines(unsigned quality,
                                          ComPtr<ID3D12PipelineState>& edges,
                                          ComPtr<ID3D12PipelineState>& weights) {
    static constexpr ShaderBytes kEdgesPs[4] = {
        bytes(g_post_smaa_edges_ps_low), bytes(g_post_smaa_edges_ps_medium),
        bytes(g_post_smaa_edges_ps_high), bytes(g_post_smaa_edges_ps_ultra)};
    static constexpr ShaderBytes kWeightsVs[4] = {
        bytes(g_post_smaa_weights_vs_low), bytes(g_post_smaa_weights_vs_medium),
        bytes(g_post_smaa_weights_vs_high), bytes(g_post_smaa_weights_vs_ultra)};
    static constexpr ShaderBytes kWeightsPs[4] = {
        bytes(g_post_smaa_weights_ps_low), bytes(g_post_smaa_weights_ps_medium),
        bytes(g_post_smaa_weights_ps_high), bytes(g_post_smaa_weights_ps_ultra)};
    const unsigned q = std::min(quality, 3u);
    return create_pipeline(code(bytes(g_post_smaa_edges_vs)), code(kEdgesPs[q]), kEdgesFormat, 1, edges) &&
           create_pipeline(code(kWeightsVs[q]), code(kWeightsPs[q]), kColorFormat, 2, weights);
}

void PostProcessor::apply_settings(const PostProcessSettings& settings,
                                   std::uint64_t frame_index, unsigned frames_in_flight) {
    PostProcessSettings next = settings;
    next.smaa_quality = std::min<std::uint8_t>(next.smaa_quality, 3u);
    if (ready_ && status_.smaa_ok && next.smaa_quality != settings_.smaa_quality) {
        ComPtr<ID3D12PipelineState> edges, weights;
        if (!create_smaa_pipelines(next.smaa_quality, edges, weights)) {
            std::cerr << "[post-process] SMAA quality " << unsigned(next.smaa_quality)
                      << " pipelines failed; keeping " << unsigned(settings_.smaa_quality) << '\n';
            next.smaa_quality = settings_.smaa_quality;
        } else {
            const std::uint64_t release = frame_index + frames_in_flight;
            retired_.push_back({nullptr, release, std::move(smaa_edges_)});
            retired_.push_back({nullptr, release, std::move(smaa_weights_)});
            smaa_edges_ = std::move(edges);
            smaa_weights_ = std::move(weights);
        }
    }
    settings_ = next;
    status_.live_aa = static_cast<std::uint8_t>(settings_.aa);
    status_.live_quality = settings_.smaa_quality;
}

namespace {

template <std::size_t N>
void set_reason(std::array<char, N>& out, const char* text) noexcept {
    std::snprintf(out.data(), out.size(), "%s", text);
}

// GALAXY_TEST_POST_FAULT=smaa|selftest|fxaa|targets (comma-separated) makes
// that step fail, so tests can show each fallback and status text.
bool test_fault(const char* name) noexcept {
    char value[64]{};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), "GALAXY_TEST_POST_FAULT") != 0 || length <= 1u) {
        return false;
    }
    const std::size_t n = std::strlen(name);
    for (const char* p = value; (p = std::strstr(p, name)) != nullptr; p += n) {
        const bool starts = p == value || p[-1] == ',';
        const bool ends = p[n] == '\0' || p[n] == ',';
        if (starts && ends) return true;
    }
    return false;
}

}  // namespace

bool PostProcessor::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                               const PostProcessSettings& settings) {
    shutdown();
    device_ = device;
    settings_ = settings;
    settings_.smaa_quality = std::min<std::uint8_t>(settings_.smaa_quality, 3u);
    status_ = {};
    status_.live_aa = static_cast<std::uint8_t>(settings_.aa);
    status_.live_quality = settings_.smaa_quality;
    set_reason(status_.smaa_reason, "INIT FAILED");
    set_reason(status_.fxaa_reason, "INIT FAILED");
    if (device_ == nullptr || queue == nullptr) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(Constants) / 4u;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (unsigned i = 0; i < 2u; ++i) {
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature_)))) {
        std::cerr << "[post-process] root signature failed\n";
        shutdown();
        return false;
    }

    // The scaling passes are required; each AA pass is optional and only
    // reported when it cannot be built.
    const bool scaling =
        create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_fsr_easu_ps)), kColorFormat, 0, easu_) &&
        create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_fsr_rcas_ps)), kColorFormat, 0, rcas_) &&
        create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_resample_ps)), kColorFormat, 0, resample_);
    if (!scaling) {
        std::cerr << "[post-process] pipeline creation failed\n";
        shutdown();
        return false;
    }
    // The look pass is optional: when any of its pipelines fails the chain
    // presents without it.
    if (!(create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_bloom_down_ps)), kBloomFormat, 0, bloom_down_) &&
          create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_bloom_up_ps)), kBloomFormat, 0, bloom_up_) &&
          create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_enhance_ps)), kColorFormat, 0, enhance_))) {
        std::cerr << "[post-process] look pass pipelines failed; presenting without it\n";
        bloom_down_.Reset();
        bloom_up_.Reset();
        enhance_.Reset();
    }
    if (!(create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_ssao_ao_ps)), kSsaoFormat, 0, ssao_ao_pipeline_) &&
          create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_ssao_blur_ps)), kSsaoFormat, 0, ssao_blur_pipeline_) &&
          create_pipeline_multiply(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_ssao_apply_ps)), kColorFormat, ssao_apply_pipeline_))) {
        std::cerr << "[post-process] ambient occlusion pipelines failed; it stays off\n";
        ssao_ao_pipeline_.Reset();
        ssao_blur_pipeline_.Reset();
        ssao_apply_pipeline_.Reset();
    }
    if (!(create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_dof_blur_ps)), kColorFormat, 0, dof_blur_pipeline_) &&
          create_pipeline_over(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_dof_apply_ps)), kColorFormat, dof_apply_pipeline_))) {
        std::cerr << "[post-process] depth of field pipelines failed; it stays off\n";
        dof_blur_pipeline_.Reset();
        dof_apply_pipeline_.Reset();
    }
    if (!create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_motion_blur_ps)), kColorFormat, 0, motion_blur_pipeline_)) {
        std::cerr << "[post-process] motion blur pipeline failed; it stays off\n";
        motion_blur_pipeline_.Reset();
    }
    if (!create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_ssr_ps)), kColorFormat, 0, ssr_pipeline_)) {
        std::cerr << "[post-process] reflection pipeline failed; it stays off\n";
        ssr_pipeline_.Reset();
    }
    fault_selftest_ = test_fault("selftest");
    fault_targets_ = test_fault("targets");
    const bool smaa_pipelines = !test_fault("smaa") &&
        create_smaa_pipelines(settings_.smaa_quality, smaa_edges_, smaa_weights_) &&
        create_pipeline(code(bytes(g_post_smaa_blend_vs)), code(bytes(g_post_smaa_blend_ps)), kColorFormat, 0, smaa_blend_);
    const bool fxaa_pipeline = !test_fault("fxaa") &&
        create_pipeline(code(bytes(g_post_fullscreen_vs)), code(bytes(g_post_fxaa_ps)), kColorFormat, 0, fxaa_);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtvSlots;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)))) {
        shutdown();
        return false;
    }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)))) {
        shutdown();
        return false;
    }
    rtv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto texture = [&](unsigned w, unsigned h, DXGI_FORMAT format,
                             ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = format;
        rd.SampleDesc.Count = 1;
        return SUCCEEDED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&out)));
    };
    const bool lookup_textures =
        texture(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, area_texture_) &&
        texture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, search_texture_);
    ready_ = true;

    set_reason(status_.smaa_reason, !smaa_pipelines ? "PIPELINES" : !lookup_textures ? "LOOKUP" : "");
    set_reason(status_.fxaa_reason, fxaa_pipeline ? "" : "PIPELINES");
    status_.smaa_ok = smaa_pipelines && lookup_textures;
    status_.fxaa_ok = fxaa_pipeline;
    self_test(queue);
    status_.initialized = true;
    std::cerr << "[post-process] self-test smaa=" << (status_.smaa_ok ? "pass" : "fail")
              << (status_.smaa_ok ? "" : " smaa-reason=") << status_.smaa_reason.data()
              << " fxaa=" << (status_.fxaa_ok ? "pass" : "fail")
              << (status_.fxaa_ok ? "" : " fxaa-reason=") << status_.fxaa_reason.data() << '\n';
    return true;
}

void PostProcessor::shutdown() noexcept {
    ready_ = false;
    status_.initialized = false;
    status_.smaa_ok = false;
    status_.fxaa_ok = false;
    smaa_edges_.Reset();
    smaa_weights_.Reset();
    smaa_blend_.Reset();
    fxaa_.Reset();
    easu_.Reset();
    rcas_.Reset();
    resample_.Reset();
    bloom_down_.Reset();
    bloom_up_.Reset();
    enhance_.Reset();
    ssao_ao_pipeline_.Reset();
    ssao_blur_pipeline_.Reset();
    ssao_apply_pipeline_.Reset();
    ssao_ao_ = {};
    ssao_blur_ = {};
    dof_blur_pipeline_.Reset();
    dof_apply_pipeline_.Reset();
    motion_blur_pipeline_.Reset();
    ssr_pipeline_.Reset();
    scene_temp_ = {};
    final_ = {};
    for (auto& t : bloom_down_targets_) t = {};
    for (auto& t : bloom_up_targets_) t = {};
    root_signature_.Reset();
    area_texture_.Reset();
    search_texture_.Reset();
    rtv_heap_.Reset();
    dsv_heap_.Reset();
    edges_ = {};
    weights_ = {};
    antialiased_ = {};
    scaled_ = {};
    stencil_ = {};
    retired_.clear();
    device_ = nullptr;
}

void PostProcessor::release_retired(std::uint64_t frame_index) noexcept {
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(), [&](const Retired& r) {
            return frame_index > r.release_after_frame;
        }),
        retired_.end());
}

bool PostProcessor::ensure_target(Target& target, unsigned width, unsigned height,
                                  DXGI_FORMAT format, unsigned rtv_slot,
                                  std::uint64_t frame_index, unsigned frames_in_flight) {
    if (target.resource && target.width == width && target.height == height &&
        target.format == format) {
        return true;
    }
    if (target.resource) {
        retired_.push_back({std::move(target.resource), frame_index + frames_in_flight});
    }
    target = {};
    const bool depth = format == kStencilFormat;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
                     : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;
    const D3D12_RESOURCE_STATES initial =
        depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &rd, initial, &clear,
            IID_PPV_ARGS(&target.resource)))) {
        return false;
    }
    target.width = width;
    target.height = height;
    target.format = format;
    target.state = initial;
    if (depth) {
        target.rtv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateDepthStencilView(target.resource.Get(), nullptr, target.rtv);
    } else {
        target.rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        target.rtv.ptr += static_cast<SIZE_T>(rtv_slot) * rtv_increment_;
        device_->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv);
    }
    return true;
}

void PostProcessor::transition(ID3D12GraphicsCommandList* list, Target& target,
                               D3D12_RESOURCE_STATES state) {
    if (target.state == state) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target.resource.Get();
    b.Transition.StateBefore = target.state;
    b.Transition.StateAfter = state;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
    target.state = state;
}

bool PostProcessor::record_lookup_upload(ID3D12GraphicsCommandList* list,
                                         std::vector<ComPtr<ID3D12Resource>>& staging) {
    const auto upload = [&](ID3D12Resource* destination, const unsigned char* data,
                            unsigned width, unsigned height, unsigned pitch,
                            DXGI_FORMAT format) {
        const unsigned aligned = (pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                                 ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(aligned) * height;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&buffer)))) {
            return false;
        }
        unsigned char* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        if (FAILED(buffer->Map(0, &none, reinterpret_cast<void**>(&mapped)))) {
            return false;
        }
        for (unsigned row = 0; row < height; ++row) {
            std::memcpy(mapped + static_cast<std::size_t>(row) * aligned,
                        data + static_cast<std::size_t>(row) * pitch, pitch);
        }
        buffer->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = format;
        src.PlacedFootprint.Footprint.Width = width;
        src.PlacedFootprint.Footprint.Height = height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = aligned;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = destination;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = destination;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        staging.push_back(std::move(buffer));
        return true;
    };
    return upload(area_texture_.Get(), areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT,
                  AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM) &&
           upload(search_texture_.Get(), searchTexBytes, SEARCHTEX_WIDTH,
                  SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM);
}

void PostProcessor::draw(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                         ID3D12PipelineState* pipeline,
                         std::initializer_list<ID3D12Resource*> inputs,
                         std::initializer_list<DXGI_FORMAT> formats,
                         const D3D12_VIEWPORT& viewport,
                         const Constants& constants) {
    const DescriptorRing::Table table = srv_ring.allocate(3);
    const UINT increment = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE slot = table.cpu;
    auto format = formats.begin();
    unsigned written = 0;
    for (ID3D12Resource* input : inputs) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = *format++;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(input, &sd, slot);
        slot.ptr += increment;
        ++written;
    }
    for (; written < 3u; ++written) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = kColorFormat;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(nullptr, &sd, slot);
        slot.ptr += increment;
    }
    list->SetPipelineState(pipeline);
    list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4u, &constants, 0);
    list->SetGraphicsRootDescriptorTable(1, table.gpu);
    const D3D12_RECT scissor{
        static_cast<LONG>(viewport.TopLeftX), static_cast<LONG>(viewport.TopLeftY),
        static_cast<LONG>(viewport.TopLeftX + viewport.Width),
        static_cast<LONG>(viewport.TopLeftY + viewport.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->DrawInstanced(3, 1, 0, 0);
}

bool PostProcessor::record_aa(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                              AaRun pass, ID3D12Resource* source,
                              unsigned width, unsigned height,
                              std::uint64_t frame_index, unsigned frames_in_flight) {
    if (pass == AaRun::kNone || (fault_targets_ && frame_index != 0u) ||
        !ensure_target(antialiased_, width, height, kColorFormat, 2u, frame_index, frames_in_flight)) {
        return false;
    }
    if (pass == AaRun::kSmaa &&
        !(ensure_target(edges_, width, height, kEdgesFormat, 0u, frame_index, frames_in_flight) &&
          ensure_target(weights_, width, height, kColorFormat, 1u, frame_index, frames_in_flight) &&
          ensure_target(stencil_, width, height, kStencilFormat, 0u, frame_index, frames_in_flight))) {
        return false;
    }
    list->SetGraphicsRootSignature(root_signature_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(height);
    c.rt_metrics[2] = static_cast<float>(width);
    c.rt_metrics[3] = static_cast<float>(height);
    if (pass == AaRun::kFxaa) {
        transition(list, antialiased_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &antialiased_.rtv, FALSE, nullptr);
        draw(list, srv_ring, fxaa_.Get(), {source}, {kColorFormat}, viewport, c);
        transition(list, antialiased_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    }
    constexpr float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = stencil_.rtv;
    transition(list, edges_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(edges_.rtv, kZero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &edges_.rtv, FALSE, &dsv);
    list->OMSetStencilRef(1);
    draw(list, srv_ring, smaa_edges_.Get(), {source}, {kColorFormat}, viewport, c);
    transition(list, edges_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, weights_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(weights_.rtv, kZero, 0, nullptr);
    list->OMSetRenderTargets(1, &weights_.rtv, FALSE, &dsv);
    draw(list, srv_ring, smaa_weights_.Get(),
         {edges_.resource.Get(), area_texture_.Get(), search_texture_.Get()},
         {kEdgesFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM},
         viewport, c);
    transition(list, weights_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, antialiased_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->OMSetRenderTargets(1, &antialiased_.rtv, FALSE, nullptr);
    draw(list, srv_ring, smaa_blend_.Get(), {source, weights_.resource.Get()},
         {kColorFormat, kColorFormat}, viewport, c);
    transition(list, antialiased_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void PostProcessor::self_test(ID3D12CommandQueue* queue) {
    const auto fail = [&](AaRun pass, const char* why) {
        if (pass != AaRun::kFxaa && status_.smaa_ok) {
            status_.smaa_ok = false;
            set_reason(status_.smaa_reason, why);
        }
        if (pass != AaRun::kSmaa && status_.fxaa_ok) {
            status_.fxaa_ok = false;
            set_reason(status_.fxaa_reason, why);
        }
    };
    if (!status_.smaa_ok && !status_.fxaa_ok) {
        return;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    DescriptorRing ring;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        !ring.initialize(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16u, 1u)) {
        fail(AaRun::kNone, "TEST SETUP");
        return;
    }
    ring.begin_frame(0);
    std::vector<ComPtr<ID3D12Resource>> keep;
    if (status_.smaa_ok && !record_lookup_upload(list.Get(), keep)) {
        fail(AaRun::kSmaa, "LOOKUP");
    }

    // A white-over-black edge stepping down one row every four columns: the
    // jagged case both passes exist for.
    constexpr unsigned kSize = 32u;
    constexpr unsigned kPitch = 256u;  // kSize * 4 rounded up to the copy alignment
    const auto edge_row = [](unsigned x) { return 10u + x / 4u; };
    std::array<unsigned char, kPitch * kSize> pattern{};
    for (unsigned y = 0; y < kSize; ++y) {
        for (unsigned x = 0; x < kSize; ++x) {
            unsigned char* p = &pattern[y * kPitch + x * 4u];
            const unsigned char v = y < edge_row(x) ? 255u : 0u;
            p[0] = p[1] = p[2] = v;
            p[3] = 255u;
        }
    }
    D3D12_HEAP_PROPERTIES def{}, up{}, rb{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kSize;
    td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = kColorFormat;
    td.SampleDesc.Count = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sizeof(pattern);
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> source, upload, readback;
    unsigned char* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&source))) ||
        FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&upload))) ||
        FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)))) {
        fail(AaRun::kNone, "TEST SETUP");
        return;
    }
    std::memcpy(mapped, pattern.data(), pattern.size());
    upload->Unmap(0, nullptr);
    bd.Width = 2u * sizeof(pattern);
    if (FAILED(device_->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&readback)))) {
        fail(AaRun::kNone, "TEST SETUP");
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {kColorFormat, kSize, kSize, 1, kPitch};
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = source.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = source.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[] = {ring.heap()};
    list->SetDescriptorHeaps(1, heaps);

    const AaRun passes[2] = {AaRun::kSmaa, AaRun::kFxaa};
    bool recorded[2] = {false, false};
    for (unsigned i = 0; i < 2u; ++i) {
        const bool ok = passes[i] == AaRun::kSmaa ? status_.smaa_ok : status_.fxaa_ok;
        if (!ok) continue;
        if (!record_aa(list.Get(), ring, passes[i], source.Get(), kSize, kSize, 0u, 0u)) {
            fail(passes[i], "NO MEMORY");
            continue;
        }
        transition(list.Get(), antialiased_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = antialiased_.resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * sizeof(pattern);
        dst.PlacedFootprint.Footprint = {kColorFormat, kSize, kSize, 1, kPitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list.Get(), antialiased_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        recorded[i] = true;
    }
    if (FAILED(list->Close())) {
        fail(AaRun::kNone, "TEST SETUP");
        return;
    }
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool done = event != nullptr && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
        WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    if (event != nullptr) CloseHandle(event);
    if (!done || device_->GetDeviceRemovedReason() != S_OK) {
        // The GPU may still own these: keep them for the processor's life.
        retired_.push_back({std::move(source), UINT64_MAX});
        retired_.push_back({std::move(upload), UINT64_MAX});
        retired_.push_back({std::move(readback), UINT64_MAX});
        for (auto& r : keep) retired_.push_back({std::move(r), UINT64_MAX});
        retired_.push_back({std::move(edges_.resource), UINT64_MAX});
        retired_.push_back({std::move(weights_.resource), UINT64_MAX});
        retired_.push_back({std::move(antialiased_.resource), UINT64_MAX});
        retired_.push_back({std::move(stencil_.resource), UINT64_MAX});
        edges_ = weights_ = antialiased_ = stencil_ = {};
        fail(AaRun::kNone, done ? "DEVICE LOST" : "GPU TIMEOUT");
        return;
    }
    const D3D12_RANGE all{0, static_cast<SIZE_T>(2u * sizeof(pattern))};
    unsigned char* out = nullptr;
    if (FAILED(readback->Map(0, &all, reinterpret_cast<void**>(&out)))) {
        fail(AaRun::kNone, "TEST SETUP");
        return;
    }
    for (unsigned i = 0; i < 2u; ++i) {
        if (!recorded[i]) continue;
        unsigned changed = 0u, stray = 0u;
        for (unsigned y = 0; y < kSize; ++y) {
            for (unsigned x = 0; x < kSize; ++x) {
                const unsigned char* a = &pattern[y * kPitch + x * 4u];
                const unsigned char* c = out + i * sizeof(pattern) + y * kPitch + x * 4u;
                int delta = 0;
                for (unsigned k = 0; k < 3u; ++k) delta = std::max(delta, std::abs(int(a[k]) - int(c[k])));
                const int distance = std::abs(int(y) - int(edge_row(x)));
                if (delta > 24) ++changed;
                if (distance >= 3 && delta > 4) ++stray;
            }
        }
        // A working pass blends a few dozen pixels along the steps and
        // leaves flat areas alone.
        const bool passed = changed >= 8u && stray == 0u &&
            !(fault_selftest_ && passes[i] == AaRun::kSmaa);
        std::cerr << "[post-process] self-test " << (passes[i] == AaRun::kSmaa ? "smaa" : "fxaa")
                  << " changed=" << changed << " stray=" << stray << '\n';
        if (!passed) fail(passes[i], "SELF-TEST");
    }
    readback->Unmap(0, &none);
    // The GPU is idle: drop the 32x32 targets now instead of at the first frame.
    edges_ = weights_ = antialiased_ = stencil_ = {};
}

void PostProcessor::record(ID3D12GraphicsCommandList* list,
                           DescriptorRing& srv_ring,
                           ID3D12Resource* source,
                           unsigned source_width,
                           unsigned source_height,
                           D3D12_CPU_DESCRIPTOR_HANDLE target_rtv,
                           const D3D12_VIEWPORT& viewport,
                           std::uint64_t frame_index,
                           unsigned frames_in_flight) {
    release_retired(frame_index);
    const unsigned target_width = static_cast<unsigned>(viewport.Width);
    const unsigned target_height = static_cast<unsigned>(viewport.Height);
    PostProcessPlan plan = plan_post_process(
        source_width, source_height, target_width, target_height, settings_,
        status_.smaa_ok, status_.fxaa_ok);

    ID3D12Resource* image = source;
    AaRun ran = AaRun::kNone;
    if (plan.smaa || plan.fxaa) {
        const AaRun pass = plan.smaa ? AaRun::kSmaa : AaRun::kFxaa;
        if (record_aa(list, srv_ring, pass, source, source_width, source_height,
                      frame_index, frames_in_flight)) {
            ran = pass;
        } else if (pass == AaRun::kSmaa && status_.fxaa_ok &&
                   record_aa(list, srv_ring, AaRun::kFxaa, source, source_width, source_height,
                             frame_index, frames_in_flight)) {
            ran = AaRun::kFxaa;
        }
        if (ran != AaRun::kNone) image = antialiased_.resource.Get();
    }
    ++status_.frames;
    status_.last_run = ran;
    status_.last_wanted = plan.aa_wanted;
    status_.last_supersampled = plan.supersampled;
    status_.smaa_frames += ran == AaRun::kSmaa;
    status_.fxaa_frames += ran == AaRun::kFxaa;
    if (plan.aa_wanted) {
        ++status_.aa_wanted_frames;
        if (ran == AaRun::kNone) {
            ++status_.aa_skipped_frames;
            set_reason(status_.skip_reason, plan.smaa || plan.fxaa ? "NO MEMORY" : "FAILED");
        }
    }

    list->SetGraphicsRootSignature(root_signature_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT window_sized{
        0.0f, 0.0f, static_cast<float>(target_width), static_cast<float>(target_height), 0.0f, 1.0f};
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(source_width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(source_height);
    c.rt_metrics[2] = static_cast<float>(source_width);
    c.rt_metrics[3] = static_cast<float>(source_height);

    // Window-sized intermediate for a scaling step that RCAS then reads.
    const bool needs_scaled = (plan.easu || plan.resample) && plan.rcas;
    if (needs_scaled &&
        !ensure_target(scaled_, target_width, target_height, kColorFormat, 3u,
                       frame_index, frames_in_flight)) {
        plan.rcas = false;
        plan.resample = !plan.easu;
        plan.resample_to_target = plan.resample;
    }

    const auto into = [&](bool to_target, D3D12_VIEWPORT& out_viewport,
                          Constants& constants) {
        if (to_target) {
            list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
            out_viewport = viewport;
            constants.origin[0] = static_cast<std::int32_t>(viewport.TopLeftX);
            constants.origin[1] = static_cast<std::int32_t>(viewport.TopLeftY);
        } else {
            transition(list, scaled_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->OMSetRenderTargets(1, &scaled_.rtv, FALSE, nullptr);
            out_viewport = window_sized;
            constants.origin[0] = 0;
            constants.origin[1] = 0;
        }
    };

    // The look pass needs the finished window-sized picture as a texture, so the
    // chain's last step renders into final_ instead of the back buffer.
    const bool enhance =
        settings_.enhance_active() && enhance_ && bloom_down_ && bloom_up_ &&
        ensure_target(final_, target_width, target_height, kColorFormat, kFinalSlot,
                      frame_index, frames_in_flight) &&
        ensure_enhance_targets(target_width, target_height, frame_index, frames_in_flight);
    const auto into_out = [&](D3D12_VIEWPORT& out_viewport, Constants& constants) {
        if (!enhance) {
            into(true, out_viewport, constants);
            return;
        }
        transition(list, final_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &final_.rtv, FALSE, nullptr);
        out_viewport = window_sized;
        constants.origin[0] = 0;
        constants.origin[1] = 0;
    };

    if (plan.easu) {
        Constants e = c;
        easu_constants(e.con0, e.con1, e.con2, e.con3,
                       static_cast<float>(source_width), static_cast<float>(source_height),
                       static_cast<float>(target_width), static_cast<float>(target_height));
        D3D12_VIEWPORT vp{};
        if (!plan.rcas) into_out(vp, e); else into(false, vp, e);
        draw(list, srv_ring, easu_.Get(), {image}, {kColorFormat}, vp, e);
        if (plan.rcas) {
            transition(list, scaled_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            image = scaled_.resource.Get();
        }
    } else if (plan.resample) {
        Constants r = c;
        r.params[0] = static_cast<float>(source_width);
        r.params[1] = static_cast<float>(source_height);
        r.params[2] = static_cast<float>(target_width);
        r.params[3] = static_cast<float>(target_height);
        D3D12_VIEWPORT vp{};
        if (plan.resample_to_target) into_out(vp, r); else into(false, vp, r);
        draw(list, srv_ring, resample_.Get(), {image}, {kColorFormat}, vp, r);
        if (!plan.resample_to_target) {
            transition(list, scaled_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            image = scaled_.resource.Get();
        }
    }

    if (plan.rcas) {
        Constants s = c;
        rcas_constants(s.con0, settings_.sharpness);
        s.params[0] = static_cast<float>(target_width);
        s.params[1] = static_cast<float>(target_height);
        D3D12_VIEWPORT vp{};
        into_out(vp, s);
        draw(list, srv_ring, rcas_.Get(), {image}, {kColorFormat}, vp, s);
    }

    if (enhance) {
        transition(list, final_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        record_enhance(list, srv_ring, final_.resource.Get(), target_width, target_height,
                       target_rtv, viewport);
    }
}

bool PostProcessor::record_ssao(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                                const SsaoInputs& in, std::uint64_t frame_index,
                                unsigned frames_in_flight) {
    if (!ssao_ready() || in.depth == nullptr || in.width == 0u || in.height == 0u ||
        !ensure_target(ssao_ao_, in.width, in.height, kSsaoFormat, kSsaoAoSlot,
                       frame_index, frames_in_flight) ||
        !ensure_target(ssao_blur_, in.width, in.height, kSsaoFormat, kSsaoBlurSlot,
                       frame_index, frames_in_flight)) {
        return false;
    }
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(in.width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(in.height);
    c.rt_metrics[2] = static_cast<float>(in.width);
    c.rt_metrics[3] = static_cast<float>(in.height);
    for (unsigned i = 0; i < 4u; ++i) c.params[i] = in.viewport[i];
    c.con0[0] = bits(in.camera_near);
    c.con0[1] = bits(in.camera_far);
    c.con0[2] = bits(in.proj[0]);
    c.con0[3] = bits(in.proj[1]);
    c.con1[0] = bits(in.proj[2]);
    c.con1[1] = bits(in.proj[3]);
    c.con1[2] = bits(in.radius);
    c.con1[3] = bits(in.strength);
    c.con2[0] = bits(in.bias);
    c.con2[1] = bits(in.light[0]);
    c.con2[2] = bits(in.light[1]);
    c.con2[3] = bits(in.light[2]);
    c.con3[0] = bits(in.shadow_strength);
    c.con3[1] = bits(in.shadow_length);
    c.con3[2] = bits(in.shadow_thickness);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(in.width),
                            static_cast<float>(in.height), 0.0f, 1.0f};
    list->SetGraphicsRootSignature(root_signature_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    transition(list, ssao_ao_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->OMSetRenderTargets(1, &ssao_ao_.rtv, FALSE, nullptr);
    draw(list, srv_ring, ssao_ao_pipeline_.Get(), {in.depth}, {kDepthSrvFormat}, vp, c);
    transition(list, ssao_ao_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    transition(list, ssao_blur_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->OMSetRenderTargets(1, &ssao_blur_.rtv, FALSE, nullptr);
    draw(list, srv_ring, ssao_blur_pipeline_.Get(), {in.depth, ssao_ao_.resource.Get()},
         {kDepthSrvFormat, kSsaoFormat}, vp, c);
    transition(list, ssao_blur_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE color = in.color_rtv;
    list->OMSetRenderTargets(1, &color, FALSE, nullptr);
    draw(list, srv_ring, ssao_apply_pipeline_.Get(), {ssao_blur_.resource.Get()}, {kSsaoFormat}, vp, c);
    return true;
}

bool PostProcessor::record_dof(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                               const DofInputs& in, std::uint64_t frame_index,
                               unsigned frames_in_flight) {
    if (!dof_ready() || in.depth == nullptr || in.color == nullptr || in.width == 0u ||
        in.height == 0u ||
        !ensure_target(scene_temp_, in.width, in.height, kColorFormat, kSceneTempSlot,
                       frame_index, frames_in_flight)) {
        return false;
    }
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(in.width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(in.height);
    c.rt_metrics[2] = static_cast<float>(in.width);
    c.rt_metrics[3] = static_cast<float>(in.height);
    for (unsigned i = 0; i < 4u; ++i) c.params[i] = in.viewport[i];
    c.con0[0] = bits(in.camera_near);
    c.con0[1] = bits(in.camera_far);
    c.con1[0] = bits(in.max_radius);
    c.con1[1] = bits(in.focus_start);
    c.con1[2] = bits(in.focus_full);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(in.width),
                            static_cast<float>(in.height), 0.0f, 1.0f};
    record_scene_blend(list, srv_ring, dof_blur_pipeline_.Get(), in.color, in.color_rtv,
                       {in.depth, in.color}, {kDepthSrvFormat, kColorFormat}, vp, c);
    return true;
}

bool PostProcessor::record_ssr(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                               ID3D12Resource* color, const SsaoInputs& in,
                               std::uint64_t frame_index, unsigned frames_in_flight) {
    if (!ssr_ready() || color == nullptr || in.depth == nullptr || in.width == 0u ||
        in.height == 0u ||
        !ensure_target(scene_temp_, in.width, in.height, kColorFormat, kSceneTempSlot,
                       frame_index, frames_in_flight)) {
        return false;
    }
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(in.width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(in.height);
    c.rt_metrics[2] = static_cast<float>(in.width);
    c.rt_metrics[3] = static_cast<float>(in.height);
    for (unsigned i = 0; i < 4u; ++i) c.params[i] = in.viewport[i];
    c.con0[0] = bits(in.camera_near);
    c.con0[1] = bits(in.camera_far);
    c.con0[2] = bits(in.proj[0]);
    c.con0[3] = bits(in.proj[1]);
    c.con1[0] = bits(in.proj[2]);
    c.con1[1] = bits(in.proj[3]);
    c.con1[2] = bits(in.radius);
    c.con1[3] = bits(in.strength);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(in.width),
                            static_cast<float>(in.height), 0.0f, 1.0f};
    record_scene_blend(list, srv_ring, ssr_pipeline_.Get(), color, in.color_rtv,
                       {in.depth, color}, {kDepthSrvFormat, kColorFormat}, vp, c);
    return true;
}

bool PostProcessor::record_motion_blur(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                                       const MotionBlurInputs& in, std::uint64_t frame_index,
                                       unsigned frames_in_flight) {
    if (!motion_blur_ready() || in.color == nullptr || in.width == 0u || in.height == 0u ||
        !ensure_target(scene_temp_, in.width, in.height, kColorFormat, kSceneTempSlot,
                       frame_index, frames_in_flight)) {
        return false;
    }
    Constants c{};
    c.rt_metrics[0] = 1.0f / static_cast<float>(in.width);
    c.rt_metrics[1] = 1.0f / static_cast<float>(in.height);
    c.rt_metrics[2] = static_cast<float>(in.width);
    c.rt_metrics[3] = static_cast<float>(in.height);
    for (unsigned i = 0; i < 4u; ++i) c.params[i] = in.viewport[i];
    for (unsigned i = 0; i < 4u; ++i) c.con0[i] = bits(in.proj[i]);
    for (unsigned i = 0; i < 3u; ++i) {
        c.con1[i] = bits(in.rotation[i]);
        c.con2[i] = bits(in.rotation[3u + i]);
        c.con3[i] = bits(in.rotation[6u + i]);
    }
    c.con1[3] = bits(in.max_length);
    c.con2[3] = bits(in.shutter);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(in.width),
                            static_cast<float>(in.height), 0.0f, 1.0f};
    record_scene_blend(list, srv_ring, motion_blur_pipeline_.Get(), in.color, in.color_rtv,
                       {in.color}, {kColorFormat}, vp, c);
    return true;
}

void PostProcessor::record_scene_blend(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                                       ID3D12PipelineState* pipeline, ID3D12Resource* color,
                                       D3D12_CPU_DESCRIPTOR_HANDLE color_rtv,
                                       std::initializer_list<ID3D12Resource*> inputs,
                                       std::initializer_list<DXGI_FORMAT> formats,
                                       const D3D12_VIEWPORT& vp, const Constants& c) {
    list->SetGraphicsRootSignature(root_signature_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = color;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &bar);
    transition(list, scene_temp_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->OMSetRenderTargets(1, &scene_temp_.rtv, FALSE, nullptr);
    draw(list, srv_ring, pipeline, inputs, formats, vp, c);
    transition(list, scene_temp_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    std::swap(bar.Transition.StateBefore, bar.Transition.StateAfter);
    list->ResourceBarrier(1, &bar);

    list->OMSetRenderTargets(1, &color_rtv, FALSE, nullptr);
    draw(list, srv_ring, dof_apply_pipeline_.Get(), {scene_temp_.resource.Get()}, {kColorFormat}, vp, c);
}

bool PostProcessor::ensure_enhance_targets(unsigned width, unsigned height,
                                           std::uint64_t frame_index,
                                           unsigned frames_in_flight) {
    for (unsigned k = 0; k < kBloomLevels; ++k) {
        const unsigned w = std::max(1u, width >> (k + 1u));
        const unsigned h = std::max(1u, height >> (k + 1u));
        if (!ensure_target(bloom_down_targets_[k], w, h, kBloomFormat, kBloomDownSlot + k,
                           frame_index, frames_in_flight)) {
            return false;
        }
        if (k + 1u < kBloomLevels &&
            !ensure_target(bloom_up_targets_[k], w, h, kBloomFormat, kBloomUpSlot + k,
                           frame_index, frames_in_flight)) {
            return false;
        }
    }
    return true;
}

void PostProcessor::record_enhance(ID3D12GraphicsCommandList* list, DescriptorRing& srv_ring,
                                   ID3D12Resource* scene, unsigned width, unsigned height,
                                   D3D12_CPU_DESCRIPTOR_HANDLE target_rtv,
                                   const D3D12_VIEWPORT& viewport) {
    // The pyramid is skipped at bloom 0; the look pass then reads the scene as a
    // stand-in bloom texture, scaled by zero.
    ID3D12Resource* bloom = scene;
    DXGI_FORMAT bloom_format = kColorFormat;
    ID3D12Resource* flare = scene;  // the bright pass at quarter size, or a zero-weighted stand-in
    if (settings_.bloom > 0u) {
        const auto pass = [&](ID3D12PipelineState* pipeline, Target& dst, float src_w, float src_h,
                              std::initializer_list<ID3D12Resource*> inputs,
                              std::initializer_list<DXGI_FORMAT> formats, float scalar) {
            Constants c{};
            c.rt_metrics[0] = 1.0f / src_w;
            c.rt_metrics[1] = 1.0f / src_h;
            c.rt_metrics[2] = src_w;
            c.rt_metrics[3] = src_h;
            c.params[0] = 1.0f / static_cast<float>(dst.width);
            c.params[1] = 1.0f / static_cast<float>(dst.height);
            c.params[2] = static_cast<float>(dst.width);
            c.params[3] = static_cast<float>(dst.height);
            c.con0[0] = bits(scalar);
            c.con1[0] = bits(0.62f);  // brightness threshold
            c.con1[1] = bits(0.25f);  // soft knee
            const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(dst.width),
                                    static_cast<float>(dst.height), 0.0f, 1.0f};
            transition(list, dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->OMSetRenderTargets(1, &dst.rtv, FALSE, nullptr);
            draw(list, srv_ring, pipeline, inputs, formats, vp, c);
            transition(list, dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        };
        list->SetGraphicsRootSignature(root_signature_.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (unsigned k = 0; k < kBloomLevels; ++k) {
            ID3D12Resource* src = k == 0u ? scene : bloom_down_targets_[k - 1u].resource.Get();
            const float sw = k == 0u ? static_cast<float>(width) : static_cast<float>(bloom_down_targets_[k - 1u].width);
            const float sh = k == 0u ? static_cast<float>(height) : static_cast<float>(bloom_down_targets_[k - 1u].height);
            pass(bloom_down_.Get(), bloom_down_targets_[k], sw, sh, {src},
                 {k == 0u ? kColorFormat : kBloomFormat}, k == 0u ? 1.0f : 0.0f);
        }
        for (unsigned k = kBloomLevels - 1u; k-- > 0u;) {
            Target& coarse = k + 2u == kBloomLevels ? bloom_down_targets_[kBloomLevels - 1u]
                                                    : bloom_up_targets_[k + 1u];
            pass(bloom_up_.Get(), bloom_up_targets_[k], static_cast<float>(coarse.width),
                 static_cast<float>(coarse.height),
                 {coarse.resource.Get(), bloom_down_targets_[k].resource.Get()},
                 {kBloomFormat, kBloomFormat}, 1.0f);
        }
        bloom = bloom_up_targets_[0].resource.Get();
        bloom_format = kBloomFormat;
        flare = bloom_down_targets_[1].resource.Get();
    }

    Constants c{};
    c.params[0] = static_cast<float>(width);
    c.params[1] = static_cast<float>(height);
    c.con0[0] = bits(settings_.bloom > 0u ? 0.55f * static_cast<float>(settings_.bloom) / 100.0f : 0.0f);
    c.con0[1] = bits(static_cast<float>(settings_.saturation) / 100.0f);
    c.con0[2] = bits(static_cast<float>(settings_.contrast) / 100.0f);
    c.con0[3] = bits(0.7f * static_cast<float>(settings_.vignette) / 100.0f);
    c.con1[0] = bits(settings_.bloom > 0u ? static_cast<float>(settings_.lens_flare) / 100.0f : 0.0f);
    c.origin[0] = static_cast<std::int32_t>(viewport.TopLeftX);
    c.origin[1] = static_cast<std::int32_t>(viewport.TopLeftY);
    list->SetGraphicsRootSignature(root_signature_.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
    draw(list, srv_ring, enhance_.Get(), {scene, bloom, flare},
         {kColorFormat, bloom_format, bloom_format}, viewport, c);
}

namespace {

std::mutex g_status_mutex;
PostProcessStatus g_status{};

}  // namespace

PostProcessStatus get_post_process_status() {
    const std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
}

void set_post_process_status(const PostProcessStatus& status) {
    const std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = status;
}

// Rows stay within 39 characters: the overlay's 1040 px panel at text scale 4.
std::string anti_aliasing_row_text(unsigned draft_aa, unsigned draft_quality,
                                   const PostProcessStatus& status) {
    static constexpr const char* kAa[] = {"OFF", "SMAA", "AUTO", "FXAA"};
    static constexpr const char* kQuality[] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};
    const unsigned aa = std::min(draft_aa, 3u);
    const std::string label = "ANTI-ALIASING: ";
    const std::string name = label + kAa[aa];
    if (aa == 0u) return name;
    const auto reason = [](const std::array<char, 40>& r) {
        return std::string(r[0] != '\0' ? r.data() : "UNKNOWN");
    };
    if (!status.initialized) return label + "NOT RUNNING (" + reason(status.skip_reason) + ")";
    // An unapplied choice, or nothing presented yet: the name alone.
    const bool uses_quality = aa == 1u || aa == 2u;
    if (aa != status.live_aa || (uses_quality && std::min(draft_quality, 3u) != status.live_quality) ||
        status.frames == 0u) {
        return name;
    }
    const std::string smaa = std::string("SMAA ") + kQuality[std::min(draft_quality, 3u)];
    switch (status.last_run) {
    case AaRun::kSmaa:
        return aa == 2u ? name + " (" + smaa + ")" : label + smaa;
    case AaRun::kFxaa:
        if (aa == 3u) return name;
        if (aa == 2u) return name + " (FXAA)";
        return label + "FXAA (SMAA: " + reason(status.smaa_reason) + ")";
    case AaRun::kNone:
        break;
    }
    if (aa == 2u && !status.last_wanted) return name + " (SUPERSAMPLED)";
    return label + "NOT RUNNING (" + reason(status.skip_reason) + ")";
}

std::string post_process_caps_text(const PostProcessStatus& status) {
    if (!status.initialized) {
        return std::string("POST-PROCESS: OFF (") +
               (status.skip_reason[0] != '\0' ? status.skip_reason.data() : "UNKNOWN") + ")";
    }
    return std::string("POST-PROCESS: SMAA ") + (status.smaa_ok ? "PASS" : "FAIL") +
           ", FXAA " + (status.fxaa_ok ? "PASS" : "FAIL");
}

}  // namespace galaxy::gx
