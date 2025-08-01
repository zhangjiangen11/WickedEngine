#include "wiRenderer.h"
#include "wiHairParticle.h"
#include "wiEmittedParticle.h"
#include "wiSprite.h"
#include "wiScene.h"
#include "wiHelper.h"
#include "wiTextureHelper.h"
#include "wiEnums.h"
#include "wiBacklog.h"
#include "wiProfiler.h"
#include "wiOcean.h"
#include "wiGPUSortLib.h"
#include "wiGPUBVH.h"
#include "wiJobSystem.h"
#include "wiSpinLock.h"
#include "wiEventHandler.h"
#include "wiPlatform.h"
#include "wiSheenLUT.h"
#include "wiShaderCompiler.h"
#include "wiTimer.h"
#include "wiUnorderedMap.h" // leave it here for shader dump!
#include "wiFont.h"
#include "wiVoxelGrid.h"
#include "wiPathQuery.h"
#include "wiTrailRenderer.h"

#include "shaders/ShaderInterop_Postprocess.h"
#include "shaders/ShaderInterop_Raytracing.h"
#include "shaders/ShaderInterop_BVH.h"
#include "shaders/ShaderInterop_DDGI.h"
#include "shaders/ShaderInterop_VXGI.h"
#include "shaders/ShaderInterop_FSR2.h"
#include "shaders/uvsphere.hlsli"
#include "shaders/cone.hlsli"

#include <algorithm>
#include <atomic>
#include <mutex>

using namespace wi::primitive;
using namespace wi::graphics;
using namespace wi::enums;
using namespace wi::scene;
using namespace wi::ecs;

namespace wi::renderer
{

GraphicsDevice*& device = GetDevice();

Shader				shaders[SHADERTYPE_COUNT];
Texture				textures[TEXTYPE_COUNT];
InputLayout			inputLayouts[ILTYPE_COUNT];
RasterizerState		rasterizers[RSTYPE_COUNT];
DepthStencilState	depthStencils[DSSTYPE_COUNT];
BlendState			blendStates[BSTYPE_COUNT];
GPUBuffer			buffers[BUFFERTYPE_COUNT];
Sampler				samplers[SAMPLER_COUNT];

#if __has_include("wiShaderDump.h")
// In this case, wiShaderDump.h contains precompiled shader binary data
#include "wiShaderDump.h"
#define SHADERDUMP_ENABLED
size_t GetShaderDumpCount()
{
	return wiShaderDump::shaderdump.size();
}
#else
// In this case, shaders can only be loaded from file
size_t GetShaderDumpCount()
{
	return 0;
}
#endif // SHADERDUMP

#ifdef SHADERDUMP_ENABLED
// Note: when using Shader Dump, use relative directory, because the dump will contain relative names too
std::string SHADERPATH = "shaders/";
std::string SHADERSOURCEPATH = "../WickedEngine/shaders/";
#else
// Note: when NOT using Shader Dump, use absolute directory, to avoid the case when something (eg. file dialog) overrides working directory
std::string SHADERPATH = wi::helper::GetCurrentPath() + "/shaders/";
std::string SHADERSOURCEPATH = SHADER_INTEROP_PATH;
#endif // SHADERDUMP_ENABLED

// define this to use raytracing pipeline for raytraced reflections:
//	Currently the DX12 device could crash for unknown reasons with the global root signature export
//#define RTREFLECTION_WITH_RAYTRACING_PIPELINE

static thread_local wi::vector<GPUBarrier> barrier_stack;
void FlushBarriers(CommandList cmd)
{
	if (barrier_stack.empty())
		return;
	device->Barrier(barrier_stack.data(), (uint32_t)barrier_stack.size(), cmd);
	barrier_stack.clear();
}
void PushBarrier(const GPUBarrier& barrier)
{
	barrier_stack.push_back(barrier);
}

bool wireRender = false;
bool debugBoneLines = false;
bool debugPartitionTree = false;
bool debugEmitters = false;
bool freezeCullingCamera = false;
bool debugEnvProbes = false;
bool debugForceFields = false;
bool debugCameras = false;
bool debugColliders = false;
bool debugSprings = false;
bool gridHelper = false;
bool advancedLightCulling = true;
bool variableRateShadingClassification = false;
bool variableRateShadingClassificationDebug = false;
float GameSpeed = 1;
bool debugLightCulling = false;
bool occlusionCulling = true;
bool temporalAA = false;
bool temporalAADEBUG = false;
uint32_t raytraceBounceCount = 8;
bool raytraceDebugVisualizer = false;
bool raytracedShadows = false;
bool tessellationEnabled = true;
bool disableAlbedoMaps = false;
bool forceDiffuseLighting = false;
bool SHADOWS_ENABLED = true;
bool SCREENSPACESHADOWS = false;
bool SURFELGI = false;
SURFEL_DEBUG SURFELGI_DEBUG = SURFEL_DEBUG_NONE;
bool DDGI_ENABLED = false;
bool DDGI_DEBUG_ENABLED = false;
uint32_t DDGI_RAYCOUNT = 256u;
float DDGI_BLEND_SPEED = 0.1f;
float GI_BOOST = 1.0f;
bool MESH_SHADER_ALLOWED = false;
bool MESHLET_OCCLUSION_CULLING = false;
std::atomic<size_t> SHADER_ERRORS{ 0 };
std::atomic<size_t> SHADER_MISSING{ 0 };
bool VXGI_ENABLED = false;
bool VXGI_REFLECTIONS_ENABLED = true;
bool VXGI_DEBUG = false;
int VXGI_DEBUG_CLIPMAP = 0;
bool CAPSULE_SHADOW_ENABLED = false;
float CAPSULE_SHADOW_ANGLE = XM_PIDIV4;
float CAPSULE_SHADOW_FADE = 0.2f;
bool SHADOW_LOD_OVERRIDE = true;

Texture shadowMapAtlas;
Texture shadowMapAtlas_Transparent;
int max_shadow_resolution_2D = 1024;
int max_shadow_resolution_cube = 256;

GPUBuffer indirectDebugStatsReadback[GraphicsDevice::GetBufferCount()];
bool indirectDebugStatsReadback_available[GraphicsDevice::GetBufferCount()] = {};

wi::vector<std::pair<XMFLOAT4X4, XMFLOAT4>> renderableBoxes;
wi::vector<std::pair<XMFLOAT4X4, XMFLOAT4>> renderableBoxes_depth;
wi::vector<std::pair<Sphere, XMFLOAT4>> renderableSpheres;
wi::vector<std::pair<Sphere, XMFLOAT4>> renderableSpheres_depth;
wi::vector<std::pair<Capsule, XMFLOAT4>> renderableCapsules;
wi::vector<std::pair<Capsule, XMFLOAT4>> renderableCapsules_depth;
wi::vector<RenderableLine> renderableLines;
wi::vector<RenderableLine> renderableLines_depth;
wi::vector<RenderableLine2D> renderableLines2D;
wi::vector<RenderablePoint> renderablePoints;
wi::vector<RenderablePoint> renderablePoints_depth;
wi::vector<RenderableTriangle> renderableTriangles_solid;
wi::vector<RenderableTriangle> renderableTriangles_wireframe;
wi::vector<RenderableTriangle> renderableTriangles_solid_depth;
wi::vector<RenderableTriangle> renderableTriangles_wireframe_depth;
wi::vector<uint8_t> debugTextStorage; // A stream of DebugText struct + text characters
wi::vector<PaintRadius> paintrads;
wi::vector<PaintTextureParams> painttextures;
wi::vector<PaintDecalParams> paintdecals;
wi::vector<const wi::VoxelGrid*> renderableVoxelgrids;
wi::vector<const wi::PathQuery*> renderablePathqueries;
wi::vector<const wi::TrailRenderer*> renderableTrails;

wi::SpinLock deferredMIPGenLock;
wi::vector<std::pair<Texture, bool>> deferredMIPGens;
wi::vector<std::pair<Texture, Texture>> deferredBCQueue;

static const uint32_t vertexCount_uvsphere = arraysize(UVSPHERE);
static const uint32_t vertexCount_cone = arraysize(CONE);

std::atomic_bool initialized{ false };

bool volumetric_clouds_precomputed = false;
Texture texture_shapeNoise;
Texture texture_detailNoise;
Texture texture_curlNoise;
Texture texture_weatherMap;

// Direct reference to a renderable instance:
struct alignas(16) RenderBatch
{
	uint32_t meshIndex;
	uint32_t instanceIndex;
	uint16_t distance;
	uint8_t camera_mask;
	uint8_t lod_override; // if overriding the base object LOD is needed, specify less than 0xFF in this
	uint32_t sort_bits; // an additional bitmask for sorting only, it should be used to reduce pipeline changes

	inline void Create(uint32_t meshIndex, uint32_t instanceIndex, float distance, uint32_t sort_bits, uint8_t camera_mask = 0xFF, uint8_t lod_override = 0xFF)
	{
		this->meshIndex = meshIndex;
		this->instanceIndex = instanceIndex;
		this->distance = XMConvertFloatToHalf(distance);
		this->sort_bits = sort_bits;
		this->camera_mask = camera_mask;
		this->lod_override = lod_override;
	}

	inline float GetDistance() const
	{
		return XMConvertHalfToFloat(HALF(distance));
	}
	constexpr uint32_t GetMeshIndex() const
	{
		return meshIndex;
	}
	constexpr uint32_t GetInstanceIndex() const
	{
		return instanceIndex;
	}

	// opaque sorting
	//	Priority is set to mesh index to have more instancing
	//	distance is second priority (front to back Z-buffering)
	constexpr bool operator<(const RenderBatch& other) const
	{
		union SortKey
		{
			struct
			{
				// The order of members is important here, it means the sort priority (low to high)!
				uint64_t distance : 16;
				uint64_t meshIndex : 16;
				uint64_t sort_bits : 32;
			} bits;
			uint64_t value;
		};
		static_assert(sizeof(SortKey) == sizeof(uint64_t));
		SortKey a = {};
		a.bits.distance = distance;
		a.bits.meshIndex = meshIndex;
		a.bits.sort_bits = sort_bits;
		SortKey b = {};
		b.bits.distance = other.distance;
		b.bits.meshIndex = other.meshIndex;
		b.bits.sort_bits = other.sort_bits;
		return a.value < b.value;
	}
	// transparent sorting
	//	Priority is distance for correct alpha blending (back to front rendering)
	//	mesh index is second priority for instancing
	constexpr bool operator>(const RenderBatch& other) const
	{
		union SortKey
		{
			struct
			{
				// The order of members is important here, it means the sort priority (low to high)!
				uint64_t meshIndex : 16;
				uint64_t sort_bits : 32;
				uint64_t distance : 16;
			} bits;
			uint64_t value;
		};
		static_assert(sizeof(SortKey) == sizeof(uint64_t));
		SortKey a = {};
		a.bits.distance = distance;
		a.bits.sort_bits = sort_bits;
		a.bits.meshIndex = meshIndex;
		SortKey b = {};
		b.bits.distance = other.distance;
		b.bits.sort_bits = other.sort_bits;
		b.bits.meshIndex = other.meshIndex;
		return a.value > b.value;
	}
};
static_assert(sizeof(RenderBatch) == 16ull);

// This is a utility that points to a linear array of render batches:
struct RenderQueue
{
	wi::vector<RenderBatch> batches;

	inline void init()
	{
		batches.clear();
	}
	inline void add(uint32_t meshIndex, uint32_t instanceIndex, float distance, uint32_t sort_bits, uint8_t camera_mask = 0xFF, uint8_t lod_override = 0xFF)
	{
		batches.emplace_back().Create(meshIndex, instanceIndex, distance, sort_bits, camera_mask, lod_override);
	}
	inline void add(const RenderBatch& batch)
	{
		batches.push_back(batch);
	}
	inline void sort_transparent()
	{
		std::sort(batches.begin(), batches.end(), std::greater<RenderBatch>());
	}
	inline void sort_opaque()
	{
		std::sort(batches.begin(), batches.end(), std::less<RenderBatch>());
	}
	inline bool empty() const
	{
		return batches.empty();
	}
	inline size_t size() const
	{
		return batches.size();
	}
};
static thread_local RenderQueue renderQueue;
static thread_local RenderQueue renderQueue_transparent;


const Sampler* GetSampler(SAMPLERTYPES id)
{
	return &samplers[id];
}
const Shader* GetShader(SHADERTYPE id)
{
	return &shaders[id];
}
const InputLayout* GetInputLayout(ILTYPES id)
{
	return &inputLayouts[id];
}
const RasterizerState* GetRasterizerState(RSTYPES id)
{
	return &rasterizers[id];
}
const DepthStencilState* GetDepthStencilState(DSSTYPES id)
{
	return &depthStencils[id];
}
const BlendState* GetBlendState(BSTYPES id)
{
	return &blendStates[id];
}
const GPUBuffer* GetBuffer(BUFFERTYPES id)
{
	return &buffers[id];
}
const Texture* GetTexture(TEXTYPES id)
{
	return &textures[id];
}

enum OBJECT_MESH_SHADER_PSO
{
	OBJECT_MESH_SHADER_PSO_DISABLED,
	OBJECT_MESH_SHADER_PSO_ENABLED,
	OBJECT_MESH_SHADER_PSO_COUNT
};

union ObjectRenderingVariant
{
	struct
	{
		uint32_t renderpass : 4;	// wi::enums::RENDERPASS
		uint32_t shadertype : 8;	// MaterialComponent::SHADERTYPE
		uint32_t blendmode : 4;		// wi::enums::BLENDMODE
		uint32_t cullmode : 2;		// wi::graphics::CullMode
		uint32_t tessellation : 1;	// bool
		uint32_t alphatest : 1;		// bool
		uint32_t sample_count : 4;	// 1, 2, 4, 8
		uint32_t mesh_shader : 1;	// bool
	} bits;
	uint32_t value;
};
static_assert(sizeof(ObjectRenderingVariant) == sizeof(uint32_t));
inline PipelineState* GetObjectPSO(ObjectRenderingVariant variant)
{
	static wi::unordered_map<uint32_t, PipelineState> PSO_object[RENDERPASS_COUNT][MaterialComponent::SHADERTYPE_COUNT][OBJECT_MESH_SHADER_PSO_COUNT];
	return &PSO_object[variant.bits.renderpass][variant.bits.shadertype][variant.bits.mesh_shader][variant.value];
}
wi::jobsystem::context mesh_shader_ctx;
wi::jobsystem::context object_pso_job_ctx;
PipelineState PSO_object_wire;
PipelineState PSO_object_wire_tessellation;
PipelineState PSO_object_wire_mesh_shader;

wi::jobsystem::context raytracing_ctx;
wi::jobsystem::context objectps_ctx;

wi::vector<CustomShader> customShaders;
int RegisterCustomShader(const CustomShader& customShader)
{
	static std::mutex locker;
	std::scoped_lock lck(locker);
	int result = (int)customShaders.size();
	customShaders.push_back(customShader);
	return result;
}
const wi::vector<CustomShader>& GetCustomShaders()
{
	return customShaders;
}

SHADERTYPE GetASTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest, bool transparent, bool mesh_shader)
{
	if (!mesh_shader)
		return SHADERTYPE_COUNT;

	return ASTYPE_OBJECT;
}
SHADERTYPE GetMSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest, bool transparent, bool mesh_shader)
{
	if (!mesh_shader)
		return SHADERTYPE_COUNT;

	SHADERTYPE realMS = SHADERTYPE_COUNT;

	switch (renderPass)
	{
	case RENDERPASS_MAIN:
		realMS = MSTYPE_OBJECT;
		break;
	case RENDERPASS_PREPASS:
	case RENDERPASS_PREPASS_DEPTHONLY:
		if (alphatest)
		{
			realMS = MSTYPE_OBJECT_PREPASS_ALPHATEST;
		}
		else
		{
			realMS = MSTYPE_OBJECT_PREPASS;
		}
		break;
	case RENDERPASS_SHADOW:
		if (transparent)
		{
			realMS = MSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realMS = MSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realMS = MSTYPE_SHADOW;
			}
		}
		break;
	case RENDERPASS_RAINBLOCKER:
		realMS = MSTYPE_SHADOW;
		break;
	default:
		break;
	}

	return realMS;
}
SHADERTYPE GetVSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest, bool transparent)
{
	SHADERTYPE realVS = VSTYPE_OBJECT_SIMPLE;

	switch (renderPass)
	{
	case RENDERPASS_MAIN:
		if (tessellation)
		{
			realVS = VSTYPE_OBJECT_COMMON_TESSELLATION;
		}
		else
		{
			realVS = VSTYPE_OBJECT_COMMON;
		}
		break;
	case RENDERPASS_PREPASS:
	case RENDERPASS_PREPASS_DEPTHONLY:
		if (tessellation)
		{
			if (alphatest)
			{
				realVS = VSTYPE_OBJECT_PREPASS_ALPHATEST_TESSELLATION;
			}
			else
			{
				realVS = VSTYPE_OBJECT_PREPASS_TESSELLATION;
			}
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_OBJECT_PREPASS;
			}
		}
		break;
	case RENDERPASS_ENVMAPCAPTURE:
		realVS = VSTYPE_ENVMAP;
		break;
	case RENDERPASS_SHADOW:
		if (transparent)
		{
			realVS = VSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realVS = VSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realVS = VSTYPE_SHADOW;
			}
		}
		break;
	case RENDERPASS_VOXELIZE:
		realVS = VSTYPE_VOXELIZER;
		break;
	case RENDERPASS_RAINBLOCKER:
		realVS = VSTYPE_SHADOW;
		break;
	default:
		break;
	}

	return realVS;
}
SHADERTYPE GetGSTYPE(RENDERPASS renderPass, bool alphatest, bool transparent)
{
	SHADERTYPE realGS = SHADERTYPE_COUNT;

	switch (renderPass)
	{
#ifdef VOXELIZATION_GEOMETRY_SHADER_ENABLED
	case RENDERPASS_VOXELIZE:
		realGS = GSTYPE_VOXELIZER;
		break;
#endif // VOXELIZATION_GEOMETRY_SHADER_ENABLED

	case RENDERPASS_PREPASS:
#ifdef PLATFORM_PS5
		if (alphatest)
		{
			realGS = GSTYPE_OBJECT_PRIMITIVEID_EMULATION_ALPHATEST;
		}
		else
		{
			realGS = GSTYPE_OBJECT_PRIMITIVEID_EMULATION;
		}
#endif // PLATFORM_PS5
		break;
	default:
		break;
	}

	return realGS;
}
SHADERTYPE GetHSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest)
{
	if (tessellation)
	{
		switch (renderPass)
		{
		case RENDERPASS_PREPASS:
		case RENDERPASS_PREPASS_DEPTHONLY:
			if (alphatest)
			{
				return HSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				return HSTYPE_OBJECT_PREPASS;
			}
			break;
		case RENDERPASS_MAIN:
			return HSTYPE_OBJECT;
			break;
		default:
			break;
		}
	}

	return SHADERTYPE_COUNT;
}
SHADERTYPE GetDSTYPE(RENDERPASS renderPass, bool tessellation, bool alphatest)
{
	if (tessellation)
	{
		switch (renderPass)
		{
		case RENDERPASS_PREPASS:
		case RENDERPASS_PREPASS_DEPTHONLY:
			if (alphatest)
			{
				return DSTYPE_OBJECT_PREPASS_ALPHATEST;
			}
			else
			{
				return DSTYPE_OBJECT_PREPASS;
			}
		case RENDERPASS_MAIN:
			return DSTYPE_OBJECT;
		default:
			break;
		}
	}

	return SHADERTYPE_COUNT;
}
SHADERTYPE GetPSTYPE(RENDERPASS renderPass, bool alphatest, bool transparent, MaterialComponent::SHADERTYPE shaderType)
{
	SHADERTYPE realPS = SHADERTYPE_COUNT;

	switch (renderPass)
	{
	case RENDERPASS_MAIN:
		realPS = SHADERTYPE((transparent ? PSTYPE_OBJECT_TRANSPARENT_PERMUTATION_BEGIN : PSTYPE_OBJECT_PERMUTATION_BEGIN) + shaderType);
		break;
	case RENDERPASS_PREPASS:
		if (alphatest)
		{
			realPS = PSTYPE_OBJECT_PREPASS_ALPHATEST;
		}
		else
		{
			realPS = PSTYPE_OBJECT_PREPASS;
		}
		break;
	case RENDERPASS_PREPASS_DEPTHONLY:
		if (alphatest)
		{
			realPS = PSTYPE_OBJECT_PREPASS_DEPTHONLY_ALPHATEST;
		}
		break;
	case RENDERPASS_ENVMAPCAPTURE:
		realPS = PSTYPE_ENVMAP;
		break;
	case RENDERPASS_SHADOW:
		if (transparent)
		{
			realPS = shaderType == MaterialComponent::SHADERTYPE_WATER ? PSTYPE_SHADOW_WATER : PSTYPE_SHADOW_TRANSPARENT;
		}
		else
		{
			if (alphatest)
			{
				realPS = PSTYPE_SHADOW_ALPHATEST;
			}
			else
			{
				realPS = SHADERTYPE_COUNT;
			}
		}
		break;
	case RENDERPASS_VOXELIZE:
		realPS = PSTYPE_VOXELIZER;
		break;
	default:
		break;
	}

	return realPS;
}

PipelineState PSO_occlusionquery;
PipelineState PSO_impostor[RENDERPASS_COUNT];
PipelineState PSO_impostor_wire;
PipelineState PSO_captureimpostor;

PipelineState PSO_lightvisualizer[LightComponent::LIGHTTYPE_COUNT];
PipelineState PSO_volumetriclight[LightComponent::LIGHTTYPE_COUNT];

PipelineState PSO_renderlightmap;
PipelineState PSO_paintdecal;

PipelineState PSO_lensflare;

PipelineState PSO_downsampledepthbuffer;
PipelineState PSO_upsample_bilateral;
PipelineState PSO_volumetricclouds_upsample;
PipelineState PSO_outline;
PipelineState PSO_copyDepth;
PipelineState PSO_copyStencilBit[8];
PipelineState PSO_copyStencilBit_MSAA[8];
PipelineState PSO_extractStencilBit[8];

PipelineState PSO_waveeffect;

RaytracingPipelineState RTPSO_reflection;

enum SKYRENDERING
{
	SKYRENDERING_STATIC,
	SKYRENDERING_DYNAMIC,
	SKYRENDERING_SUN,
	SKYRENDERING_ENVMAPCAPTURE_STATIC,
	SKYRENDERING_ENVMAPCAPTURE_DYNAMIC,
	SKYRENDERING_COUNT
};
PipelineState PSO_sky[SKYRENDERING_COUNT];

enum DEBUGRENDERING
{
	DEBUGRENDERING_ENVPROBE,
	DEBUGRENDERING_DDGI,
	DEBUGRENDERING_GRID,
	DEBUGRENDERING_CUBE,
	DEBUGRENDERING_CUBE_DEPTH,
	DEBUGRENDERING_LINES,
	DEBUGRENDERING_LINES_DEPTH,
	DEBUGRENDERING_TRIANGLE_SOLID,
	DEBUGRENDERING_TRIANGLE_WIREFRAME,
	DEBUGRENDERING_TRIANGLE_SOLID_DEPTH,
	DEBUGRENDERING_TRIANGLE_WIREFRAME_DEPTH,
	DEBUGRENDERING_EMITTER,
	DEBUGRENDERING_PAINTRADIUS,
	DEBUGRENDERING_VOXEL,
	DEBUGRENDERING_FORCEFIELD_POINT,
	DEBUGRENDERING_FORCEFIELD_PLANE,
	DEBUGRENDERING_RAYTRACE_BVH,
	DEBUGRENDERING_COUNT
};
PipelineState PSO_debug[DEBUGRENDERING_COUNT];

size_t GetShaderErrorCount()
{
	return SHADER_ERRORS.load();
}
size_t GetShaderMissingCount()
{
	return SHADER_MISSING.load();
}

bool LoadShader(
	ShaderStage stage,
	Shader& shader,
	const std::string& filename,
	ShaderModel minshadermodel,
	const wi::vector<std::string>& permutation_defines
)
{
	std::string shaderbinaryfilename = SHADERPATH + filename;

	if (!permutation_defines.empty())
	{
		std::string ext = wi::helper::GetExtensionFromFileName(shaderbinaryfilename);
		shaderbinaryfilename = wi::helper::RemoveExtension(shaderbinaryfilename);
		for (auto& def : permutation_defines)
		{
			shaderbinaryfilename += "_" + def;
		}
		shaderbinaryfilename += "." + ext;
	}

	if (device != nullptr)
	{
#ifdef SHADERDUMP_ENABLED
		// Loading shader from precompiled dump:
		auto it = wiShaderDump::shaderdump.find(shaderbinaryfilename);
		if (it != wiShaderDump::shaderdump.end())
		{
			wi::vector<uint8_t> decompressed;
			bool success = wi::helper::Decompress(it->second.data, it->second.size, decompressed);
			if (success)
			{
				return device->CreateShader(stage, decompressed.data(), decompressed.size(), &shader);
			}
			wi::backlog::post("shader dump decompression failure: " + shaderbinaryfilename, wi::backlog::LogLevel::Error);
		}
		else
		{
			wi::backlog::post("shader dump doesn't contain shader: " + shaderbinaryfilename, wi::backlog::LogLevel::Error);
		}
#endif // SHADERDUMP_ENABLED
	}

	wi::shadercompiler::RegisterShader(shaderbinaryfilename);

	if (wi::shadercompiler::IsShaderOutdated(shaderbinaryfilename))
	{
		wi::shadercompiler::CompilerInput input;
		input.format = device->GetShaderFormat();
		input.stage = stage;
		input.minshadermodel = minshadermodel;
		input.defines = permutation_defines;

		std::string sourcedir = SHADERSOURCEPATH;
		wi::helper::MakePathAbsolute(sourcedir);
		input.include_directories.push_back(sourcedir);
		input.include_directories.push_back(sourcedir + wi::helper::GetDirectoryFromPath(filename));
		input.shadersourcefilename = wi::helper::ReplaceExtension(sourcedir + filename, "hlsl");

		wi::shadercompiler::CompilerOutput output;
		wi::shadercompiler::Compile(input, output);

		if (output.IsValid())
		{
			wi::shadercompiler::SaveShaderAndMetadata(shaderbinaryfilename, output);

			if (!output.error_message.empty())
			{
				wi::backlog::post(output.error_message, wi::backlog::LogLevel::Warning);
			}
			wi::backlog::post("shader compiled: " + shaderbinaryfilename);
			return device->CreateShader(stage, output.shaderdata, output.shadersize, &shader);
		}
		else
		{
			wi::backlog::post("shader compile FAILED: " + shaderbinaryfilename + "\n" + output.error_message, wi::backlog::LogLevel::Error);
			SHADER_ERRORS.fetch_add(1);
		}
	}

	if (device != nullptr)
	{
		wi::vector<uint8_t> buffer;
		if (wi::helper::FileRead(shaderbinaryfilename, buffer))
		{
			bool success = device->CreateShader(stage, buffer.data(), buffer.size(), &shader);
			if (success)
			{
				device->SetName(&shader, shaderbinaryfilename.c_str());
			}
			return success;
		}
		else
		{
			SHADER_MISSING.fetch_add(1);
		}
	}

	return false;
}


void LoadShaders()
{
	wi::jobsystem::Wait(raytracing_ctx);
	raytracing_ctx.priority = wi::jobsystem::Priority::Low;

	wi::jobsystem::Wait(objectps_ctx);
	objectps_ctx.priority = wi::jobsystem::Priority::Low;

	wi::jobsystem::context ctx;

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_DEBUG], "objectVS_debug.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_COMMON], "objectVS_common.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS], "objectVS_prepass.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_ALPHATEST], "objectVS_prepass_alphatest.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_SIMPLE], "objectVS_simple.cso");
		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		inputLayouts[ILTYPE_VERTEXCOLOR].elements =
		{
			{ "POSITION", 0, Format::R32G32B32A32_FLOAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
			{ "TEXCOORD", 0, Format::R32G32B32A32_FLOAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
		};
		LoadShader(ShaderStage::VS, shaders[VSTYPE_VERTEXCOLOR], "vertexcolorVS.cso");
		});

	inputLayouts[ILTYPE_POSITION].elements =
	{
		{ "POSITION", 0, Format::R32G32B32A32_FLOAT, 0, InputLayout::APPEND_ALIGNED_ELEMENT, InputClassification::PER_VERTEX_DATA },
	};

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_COMMON_TESSELLATION], "objectVS_common_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_TESSELLATION], "objectVS_prepass_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_PREPASS_ALPHATEST_TESSELLATION], "objectVS_prepass_alphatest_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OBJECT_SIMPLE_TESSELLATION], "objectVS_simple_tessellation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_IMPOSTOR], "impostorVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_DIRECTIONAL], "volumetriclight_directionalVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_POINT], "volumetriclight_pointVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_SPOT], "volumetriclight_spotVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOLUMETRICLIGHT_RECTANGLE], "volumetriclight_rectangleVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT], "vSpotLightVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT], "vPointLightVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LIGHTVISUALIZER_RECTLIGHT], "vRectLightVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SPHERE], "sphereVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_OCCLUDEE], "occludeeVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SKY], "skyVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOXELIZER], "objectVS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_VOXEL], "voxelVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_FORCEFIELDVISUALIZER_POINT], "forceFieldPointVisualizerVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE], "forceFieldPlaneVisualizerVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_RAYTRACE_SCREEN], "raytrace_screenVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_POSTPROCESS], "postprocessVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_LENSFLARE], "lensFlareVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_DDGI_DEBUG], "ddgi_debugVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SCREEN], "screenVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_PAINTDECAL], "paintdecalVS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP], "envMapVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_ENVMAP_SKY], "envMap_skyVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW], "shadowVS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW_ALPHATEST], "shadowVS_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::VS, shaders[VSTYPE_SHADOW_TRANSPARENT], "shadowVS_transparent.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR], "impostorPS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_HOLOGRAM], "objectPS_hologram.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_DEBUG], "objectPS_debug.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PAINTRADIUS], "objectPS_paintradius.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_SIMPLE], "objectPS_simple.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PREPASS], "objectPS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PREPASS_ALPHATEST], "objectPS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_OBJECT_PREPASS_DEPTHONLY_ALPHATEST], "objectPS_prepass_depthonly_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR_PREPASS], "impostorPS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR_PREPASS_DEPTHONLY], "impostorPS_prepass_depthonly.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_IMPOSTOR_SIMPLE], "impostorPS_simple.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_LIGHTVISUALIZER], "lightVisualizerPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_LIGHTVISUALIZER_RECTLIGHT], "vRectLightPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL], "volumetricLight_DirectionalPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_POINT], "volumetricLight_PointPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_SPOT], "volumetricLight_SpotPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOLUMETRICLIGHT_RECTANGLE], "volumetriclight_rectanglePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP], "envMapPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP_SKY_STATIC], "envMap_skyPS_static.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_ENVMAP_SKY_DYNAMIC], "envMap_skyPS_dynamic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_CAPTUREIMPOSTOR], "captureImpostorPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_CUBEMAP], "cubeMapPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VERTEXCOLOR], "vertexcolorPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SKY_STATIC], "skyPS_static.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SKY_DYNAMIC], "skyPS_dynamic.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SUN], "sunPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_ALPHATEST], "shadowPS_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_TRANSPARENT], "shadowPS_transparent.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_SHADOW_WATER], "shadowPS_water.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOXELIZER], "objectPS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_VOXEL], "voxelPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_FORCEFIELDVISUALIZER], "forceFieldVisualizerPS.cso"); });
	
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_RAYTRACE_DEBUGBVH], "raytrace_debugbvhPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_DOWNSAMPLEDEPTHBUFFER], "downsampleDepthBuffer4xPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL], "upsample_bilateralPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_POSTPROCESS_OUTLINE], "outlinePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_LENSFLARE], "lensFlarePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_DDGI_DEBUG], "ddgi_debugPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_UPSAMPLE], "volumetricCloud_upsamplePS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_COPY_DEPTH], "copyDepthPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_COPY_STENCIL_BIT], "copyStencilBitPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_COPY_STENCIL_BIT_MSAA], "copyStencilBitPS.cso", ShaderModel::SM_6_0, {"MSAA"}); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_EXTRACT_STENCIL_BIT], "extractStencilBitPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_PAINTDECAL], "paintdecalPS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::PS, shaders[PSTYPE_WAVE_EFFECT], "waveeffectPS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_VOXELIZER], "objectGS_voxelizer.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_VOXEL], "voxelGS.cso"); });

#ifdef PLATFORM_PS5
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_OBJECT_PRIMITIVEID_EMULATION], "objectGS_primitiveID_emulation.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::GS, shaders[GSTYPE_OBJECT_PRIMITIVEID_EMULATION_ALPHATEST], "objectGS_primitiveID_emulation_alphatest.cso"); });
#endif // PLATFORM_PS5

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LUMINANCE_PASS1], "luminancePass1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LUMINANCE_PASS2], "luminancePass2CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SHADINGRATECLASSIFICATION], "shadingRateClassificationCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SHADINGRATECLASSIFICATION_DEBUG], "shadingRateClassificationCS_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING], "lightCullingCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_DEBUG], "lightCullingCS_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_ADVANCED], "lightCullingCS_ADVANCED.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTCULLING_ADVANCED_DEBUG], "lightCullingCS_ADVANCED_DEBUG.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RESOLVEMSAADEPTHSTENCIL], "resolveMSAADepthStencilCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VXGI_OFFSETPREV], "vxgi_offsetprevCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VXGI_TEMPORAL], "vxgi_temporalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VXGI_SDF_JUMPFLOOD], "vxgi_sdf_jumpfloodCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VXGI_RESOLVE_DIFFUSE], "vxgi_resolve_diffuseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VXGI_RESOLVE_SPECULAR], "vxgi_resolve_specularCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_TRANSMITTANCELUT], "skyAtmosphere_transmittanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], "skyAtmosphere_multiScatteredLuminanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_SKYVIEWLUT], "skyAtmosphere_skyViewLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_SKYLUMINANCELUT], "skyAtmosphere_skyLuminanceLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKYATMOSPHERE_CAMERAVOLUMELUT], "skyAtmosphere_cameraVolumeLutCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4], "generateMIPChain2DCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4], "generateMIPChain3DCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4], "generateMIPChainCubeCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4], "generateMIPChainCubeArrayCS_float4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC1], "blockcompressCS_BC1.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC3], "blockcompressCS_BC3.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC4], "blockcompressCS_BC4.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC5], "blockcompressCS_BC5.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC6H], "blockcompressCS_BC6H.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_BLOCKCOMPRESS_BC6H_CUBEMAP], "blockcompressCS_BC6H_cubemap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_FILTERENVMAP], "filterEnvMapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_FLOAT4], "copytexture2D_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_COPYTEXTURE2D_FLOAT4_BORDEREXPAND], "copytexture2D_float4_borderexpandCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SKINNING], "skinningCS.cso"); });
	
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_PAINT_TEXTURE], "paint_textureCS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT1], "blur_gaussian_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4], "blur_gaussian_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT1], "blur_gaussian_wide_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT4], "blur_gaussian_wide_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT1], "blur_bilateral_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4], "blur_bilateral_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT1], "blur_bilateral_wide_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT4], "blur_bilateral_wide_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSAO], "ssaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_HBAO], "hbaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS1], "msao_preparedepthbuffers1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS2], "msao_preparedepthbuffers2CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_INTERLEAVE], "msao_interleaveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO], "msaoCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE], "msao_blurupsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_BLENDOUT], "msao_blurupsampleCS_blendout.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN], "msao_blurupsampleCS_premin.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN_BLENDOUT], "msao_blurupsampleCS_premin_blendout.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_HORIZONTAL], "ssr_tileMaxRoughness_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_VERTICAL], "ssr_tileMaxRoughness_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_DEPTHHIERARCHY], "ssr_depthHierarchyCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE], "ssr_raytraceCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_EARLYEXIT], "ssr_raytraceCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_CHEAP], "ssr_raytraceCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], "ssr_resolveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], "ssr_temporalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSR_UPSAMPLE], "ssr_upsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_LIGHTSHAFTS], "lightShaftsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_HORIZONTAL], "depthoffield_tileMaxCOC_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_VERTICAL], "depthoffield_tileMaxCOC_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_NEIGHBORHOODMAXCOC], "depthoffield_neighborhoodMaxCOCCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], "depthoffield_prepassCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS_EARLYEXIT], "depthoffield_prepassCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN], "depthoffield_mainCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_EARLYEXIT], "depthoffield_mainCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_CHEAP], "depthoffield_mainCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_POSTFILTER], "depthoffield_postfilterCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_UPSAMPLE], "depthoffield_upsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_HORIZONTAL], "motionblur_tileMaxVelocity_horizontalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_VERTICAL], "motionblur_tileMaxVelocity_verticalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_NEIGHBORHOODMAXVELOCITY], "motionblur_neighborhoodMaxVelocityCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR], "motionblurCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_EARLYEXIT], "motionblurCS_earlyexit.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_CHEAP], "motionblurCS_cheap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_BLOOMSEPARATE], "bloomseparateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE], "aerialPerspectiveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE_CAPTURE], "aerialPerspectiveCS_capture.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE_CAPTURE_MSAA], "aerialPerspectiveCS_capture_MSAA.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHAPENOISE], "volumetricCloud_shapenoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_DETAILNOISE], "volumetricCloud_detailnoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_CURLNOISE], "volumetricCloud_curlnoiseCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_WEATHERMAP], "volumetricCloud_weathermapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER], "volumetricCloud_renderCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER_CAPTURE], "volumetricCloud_renderCS_capture.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER_CAPTURE_MSAA], "volumetricCloud_renderCS_capture_MSAA.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_REPROJECT], "volumetricCloud_reprojectCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHADOW_RENDER], "volumetricCloud_shadow_renderCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FXAA], "fxaaCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_TEMPORALAA], "temporalaaCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SHARPEN], "sharpenCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_TONEMAP], "tonemapCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UNDERWATER], "underwaterCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR_UPSCALING], "fsr_upscalingCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR_SHARPEN], "fsr_sharpenCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_AUTOGEN_REACTIVE_PASS], "ffx-fsr2/ffx_fsr2_autogen_reactive_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_COMPUTE_LUMINANCE_PYRAMID_PASS], "ffx-fsr2/ffx_fsr2_compute_luminance_pyramid_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_PREPARE_INPUT_COLOR_PASS], "ffx-fsr2/ffx_fsr2_prepare_input_color_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_RECONSTRUCT_PREVIOUS_DEPTH_PASS], "ffx-fsr2/ffx_fsr2_reconstruct_previous_depth_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_DEPTH_CLIP_PASS], "ffx-fsr2/ffx_fsr2_depth_clip_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_LOCK_PASS], "ffx-fsr2/ffx_fsr2_lock_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_ACCUMULATE_PASS], "ffx-fsr2/ffx_fsr2_accumulate_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_FSR2_RCAS_PASS], "ffx-fsr2/ffx_fsr2_rcas_pass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_CHROMATIC_ABERRATION], "chromatic_aberrationCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT1], "upsample_bilateral_float1CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4], "upsample_bilateral_float4CS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_DOWNSAMPLE4X], "downsample4xCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_LINEARDEPTH], "lineardepthCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_NORMALSFROMDEPTH], "normalsfromdepthCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SCREENSPACESHADOW], "screenspaceshadowCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSGI_DEINTERLEAVE], "ssgi_deinterleaveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSGI], "ssgiCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSGI_WIDE], "ssgiCS.cso", wi::graphics::ShaderModel::SM_5_0, { "WIDE" }); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSGI_UPSAMPLE], "ssgi_upsampleCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_SSGI_UPSAMPLE_WIDE], "ssgi_upsampleCS.cso", wi::graphics::ShaderModel::SM_5_0, { "WIDE" }); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_SPATIAL], "rtdiffuse_spatialCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_TEMPORAL], "rtdiffuse_temporalCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_UPSAMPLE], "rtdiffuse_upsampleCS.cso"); });

	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTDIFFUSE], "rtdiffuseCS.cso", ShaderModel::SM_6_5); });

		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTREFLECTION], "rtreflectionCS.cso", ShaderModel::SM_6_5); });

		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW], "rtshadowCS.cso", ShaderModel::SM_6_5); });
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TILECLASSIFICATION], "rtshadow_denoise_tileclassificationCS.cso"); });
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_FILTER], "rtshadow_denoise_filterCS.cso"); });
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TEMPORAL], "rtshadow_denoise_temporalCS.cso"); });

		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO], "rtaoCS.cso", ShaderModel::SM_6_5); });
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_TILECLASSIFICATION], "rtao_denoise_tileclassificationCS.cso"); });
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_FILTER], "rtao_denoise_filterCS.cso"); });

	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_POSTPROCESS_RTSHADOW_UPSAMPLE], "rtshadow_upsampleCS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_COVERAGE], "surfel_coverageCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_INDIRECTPREPARE], "surfel_indirectprepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_UPDATE], "surfel_updateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_GRIDOFFSETS], "surfel_gridoffsetsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_BINNING], "surfel_binningCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_INTEGRATE], "surfel_integrateCS.cso"); });
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_RAYTRACE], "surfel_raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_SURFEL_RAYTRACE], "surfel_raytraceCS.cso"); });
	}

	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RAYTRACE], "raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_RAYTRACE], "raytraceCS.cso"); });
	}

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_RESOLVE], "visibility_resolveCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_RESOLVE_MSAA], "visibility_resolveCS_MSAA.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_SKY], "visibility_skyCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VISIBILITY_VELOCITY], "visibility_velocityCS.cso"); });

	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_RAYTRACE], "ddgi_raytraceCS_rtapi.cso", ShaderModel::SM_6_5); });
	}
	else
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_RAYTRACE], "ddgi_raytraceCS.cso"); });
	}
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_RAYALLOCATION], "ddgi_rayallocationCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_INDIRECTPREPARE], "ddgi_indirectprepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_UPDATE], "ddgi_updateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DDGI_UPDATE_DEPTH], "ddgi_updateCS_depth.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TERRAIN_VIRTUALTEXTURE_UPDATE_BASECOLORMAP], "terrainVirtualTextureUpdateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TERRAIN_VIRTUALTEXTURE_UPDATE_NORMALMAP], "terrainVirtualTextureUpdateCS_normalmap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TERRAIN_VIRTUALTEXTURE_UPDATE_SURFACEMAP], "terrainVirtualTextureUpdateCS_surfacemap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_TERRAIN_VIRTUALTEXTURE_UPDATE_EMISSIVEMAP], "terrainVirtualTextureUpdateCS_emissivemap.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_MESHLET_PREPARE], "meshlet_prepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_IMPOSTOR_PREPARE], "impostor_prepareCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VIRTUALTEXTURE_TILEREQUESTS], "virtualTextureTileRequestsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VIRTUALTEXTURE_TILEALLOCATE], "virtualTextureTileAllocateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_VIRTUALTEXTURE_RESIDENCYUPDATE], "virtualTextureResidencyUpdateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_WIND], "windCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_YUV_TO_RGB], "yuv_to_rgbCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_YUV_TO_RGB_ARRAY], "yuv_to_rgbCS.cso", ShaderModel::SM_6_0, {"ARRAY"}); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_WETMAP_UPDATE], "wetmap_updateCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_CAUSTICS], "causticsCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DEPTH_REPROJECT], "depth_reprojectCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_DEPTH_PYRAMID], "depth_pyramidCS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::CS, shaders[CSTYPE_LIGHTMAP_EXPAND], "lightmap_expandCS.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT], "objectHS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_PREPASS], "objectHS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_PREPASS_ALPHATEST], "objectHS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::HS, shaders[HSTYPE_OBJECT_SIMPLE], "objectHS_simple.cso"); });

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT], "objectDS.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_PREPASS], "objectDS_prepass.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_PREPASS_ALPHATEST], "objectDS_prepass_alphatest.cso"); });
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::DS, shaders[DSTYPE_OBJECT_SIMPLE], "objectDS_simple.cso"); });

	wi::jobsystem::Dispatch(objectps_ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		LoadShader(
			ShaderStage::PS,
			shaders[PSTYPE_OBJECT_PERMUTATION_BEGIN + args.jobIndex],
			"objectPS.cso",
			ShaderModel::SM_6_0,
			MaterialComponent::shaderTypeDefines[args.jobIndex] // permutation defines
		);

	});

	wi::jobsystem::Dispatch(objectps_ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		auto defines = MaterialComponent::shaderTypeDefines[args.jobIndex];
		defines.push_back("TRANSPARENT");
		LoadShader(
			ShaderStage::PS,
			shaders[PSTYPE_OBJECT_TRANSPARENT_PERMUTATION_BEGIN + args.jobIndex],
			"objectPS.cso",
			ShaderModel::SM_6_0,
			defines // permutation defines
		);

	});

	wi::jobsystem::Wait(ctx);

	if (device->CheckCapability(GraphicsDeviceCapability::MESH_SHADER))
	{
		// Note: Mesh shader loading is very slow in Vulkan, so all mesh shader loading will be executed on a separate context
		//	and only waited by mesh shader PSO jobs, not holding back the rest of initialization
		wi::jobsystem::Wait(mesh_shader_ctx);
		mesh_shader_ctx.priority = wi::jobsystem::Priority::Low;

		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) {
			LoadShader(ShaderStage::AS, shaders[ASTYPE_OBJECT], "objectAS.cso");
			LoadShader(ShaderStage::MS, shaders[MSTYPE_OBJECT_SIMPLE], "objectMS_simple.cso");

			PipelineStateDesc desc;
			desc.as = &shaders[ASTYPE_OBJECT];
			desc.ms = &shaders[MSTYPE_OBJECT_SIMPLE];
			desc.ps = &shaders[PSTYPE_OBJECT_SIMPLE]; // this is created in a different thread, so wait for the ctx before getting here
			desc.rs = &rasterizers[RSTYPE_WIRE];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			device->CreatePipelineState(&desc, &PSO_object_wire_mesh_shader);
		});

		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_OBJECT], "objectMS.cso"); });
		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_OBJECT_PREPASS], "objectMS_prepass.cso"); });
		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_OBJECT_PREPASS_ALPHATEST], "objectMS_prepass_alphatest.cso"); });

		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_SHADOW], "shadowMS.cso"); });
		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_SHADOW_ALPHATEST], "shadowMS_alphatest.cso"); });
		wi::jobsystem::Execute(mesh_shader_ctx, [](wi::jobsystem::JobArgs args) { LoadShader(ShaderStage::MS, shaders[MSTYPE_SHADOW_TRANSPARENT], "shadowMS_transparent.cso"); });
	}

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SURFACE_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_surfaceCS.cso",
			ShaderModel::SM_6_0,
			MaterialComponent::shaderTypeDefines[args.jobIndex] // permutation defines
		);

		});

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		auto defines = MaterialComponent::shaderTypeDefines[args.jobIndex];
		defines.push_back("REDUCED");
		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SURFACE_REDUCED_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_surfaceCS.cso",
			ShaderModel::SM_6_0,
			defines // permutation defines
		);

		});

	wi::jobsystem::Dispatch(ctx, MaterialComponent::SHADERTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {

		LoadShader(
			ShaderStage::CS,
			shaders[CSTYPE_VISIBILITY_SHADE_PERMUTATION_BEGIN + args.jobIndex],
			"visibility_shadeCS.cso",
			ShaderModel::SM_6_0,
			MaterialComponent::shaderTypeDefines[args.jobIndex] // permutation defines
		);

		});

	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE];
		desc.ps = &shaders[PSTYPE_OBJECT_SIMPLE];
		desc.rs = &rasterizers[RSTYPE_WIRE];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];

		device->CreatePipelineState(&desc, &PSO_object_wire);

		desc.pt = PrimitiveTopology::PATCHLIST;
		desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE_TESSELLATION];
		desc.hs = &shaders[HSTYPE_OBJECT_SIMPLE];
		desc.ds = &shaders[DSTYPE_OBJECT_SIMPLE];
		device->CreatePipelineState(&desc, &PSO_object_wire_tessellation);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OCCLUDEE];
		desc.rs = &rasterizers[RSTYPE_OCCLUDEE];
		desc.bs = &blendStates[BSTYPE_COLORWRITEDISABLE];
		desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
		desc.pt = PrimitiveTopology::TRIANGLESTRIP;

		device->CreatePipelineState(&desc, &PSO_occlusionquery);
		});
	wi::jobsystem::Dispatch(ctx, RENDERPASS_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		const bool impostorRequest =
			args.jobIndex != RENDERPASS_VOXELIZE &&
			args.jobIndex != RENDERPASS_SHADOW &&
			args.jobIndex != RENDERPASS_ENVMAPCAPTURE;
		if (!impostorRequest)
		{
			return;
		}

		PipelineStateDesc desc;
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];
		desc.il = nullptr;

		switch (args.jobIndex)
		{
		case RENDERPASS_MAIN:
			desc.dss = &depthStencils[DSSTYPE_DEPTHREADEQUAL];
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR];
			break;
		case RENDERPASS_PREPASS:
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR_PREPASS];
			break;
		case RENDERPASS_PREPASS_DEPTHONLY:
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR_PREPASS_DEPTHONLY];
			break;
		default:
			desc.vs = &shaders[VSTYPE_IMPOSTOR];
			desc.ps = &shaders[PSTYPE_IMPOSTOR_PREPASS];
			break;
		}

		device->CreatePipelineState(&desc, &PSO_impostor[args.jobIndex]);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_IMPOSTOR];
		desc.ps = &shaders[PSTYPE_IMPOSTOR_SIMPLE];
		desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT];
		desc.il = nullptr;

		device->CreatePipelineState(&desc, &PSO_impostor_wire);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_OBJECT_COMMON];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_CAPTUREIMPOSTOR];

		desc.ps = &shaders[PSTYPE_CAPTUREIMPOSTOR];
		device->CreatePipelineState(&desc, &PSO_captureimpostor);
		});

	wi::jobsystem::Dispatch(ctx, LightComponent::LIGHTTYPE_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		// deferred lights:

		desc.pt = PrimitiveTopology::TRIANGLELIST;


		// light visualizers:
		if (args.jobIndex != LightComponent::DIRECTIONAL)
		{

			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.ps = &shaders[PSTYPE_LIGHTVISUALIZER];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			desc.il = nullptr;

			switch (args.jobIndex)
			{
			case LightComponent::POINT:
				desc.bs = &blendStates[BSTYPE_ADDITIVE];
				desc.vs = &shaders[VSTYPE_LIGHTVISUALIZER_POINTLIGHT];
				desc.rs = &rasterizers[RSTYPE_FRONT];
				desc.il = &inputLayouts[ILTYPE_POSITION];
				break;
			case LightComponent::SPOT:
				desc.bs = &blendStates[BSTYPE_ADDITIVE];
				desc.vs = &shaders[VSTYPE_LIGHTVISUALIZER_SPOTLIGHT];
				desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
				break;
			case LightComponent::RECTANGLE:
				desc.bs = &blendStates[BSTYPE_TRANSPARENT];
				desc.vs = &shaders[VSTYPE_LIGHTVISUALIZER_RECTLIGHT];
				desc.ps = &shaders[PSTYPE_LIGHTVISUALIZER_RECTLIGHT];
				desc.rs = &rasterizers[RSTYPE_FRONT];
				desc.pt = PrimitiveTopology::TRIANGLESTRIP;
				desc.dss = &depthStencils[DSSTYPE_DEFAULT];
				break;
			}

			device->CreatePipelineState(&desc, &PSO_lightvisualizer[args.jobIndex]);
		}


		// volumetric lights:
		if (args.jobIndex <= LightComponent::RECTANGLE)
		{
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.rs = &rasterizers[RSTYPE_BACK];
			desc.pt = PrimitiveTopology::TRIANGLELIST;

			switch (args.jobIndex)
			{
			case LightComponent::DIRECTIONAL:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_DIRECTIONAL];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_DIRECTIONAL];
				break;
			case LightComponent::POINT:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_POINT];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_POINT];
				break;
			case LightComponent::SPOT:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_SPOT];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_SPOT];
				break;
			case LightComponent::RECTANGLE:
				desc.vs = &shaders[VSTYPE_VOLUMETRICLIGHT_RECTANGLE];
				desc.ps = &shaders[PSTYPE_VOLUMETRICLIGHT_RECTANGLE];
				desc.rs = &rasterizers[RSTYPE_FRONT];
				desc.pt = PrimitiveTopology::TRIANGLESTRIP;
				break;
			}

			device->CreatePipelineState(&desc, &PSO_volumetriclight[args.jobIndex]);
		}


		});
	wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) {
		LoadShader(ShaderStage::VS, shaders[VSTYPE_RENDERLIGHTMAP], "renderlightmapVS.cso");
		if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		{
			LoadShader(ShaderStage::PS, shaders[PSTYPE_RENDERLIGHTMAP], "renderlightmapPS_rtapi.cso", ShaderModel::SM_6_5);
		}
		else
		{
			LoadShader(ShaderStage::PS, shaders[PSTYPE_RENDERLIGHTMAP], "renderlightmapPS.cso");
		}
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_RENDERLIGHTMAP];
		desc.ps = &shaders[PSTYPE_RENDERLIGHTMAP];
		desc.rs = &rasterizers[RSTYPE_LIGHTMAP];
		desc.bs = &blendStates[BSTYPE_TRANSPARENT];
		desc.dss = &depthStencils[DSSTYPE_DEFAULT]; // Note: depth is used to disallow overlapped pixel/primitive writes with conservative rasterization!

		device->CreatePipelineState(&desc, &PSO_renderlightmap);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_PAINTDECAL];
		desc.ps = &shaders[PSTYPE_PAINTDECAL];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_TRANSPARENT];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		RenderPassInfo renderpass_info;
		renderpass_info.rt_count = 1;
		renderpass_info.rt_formats[0] = Format::R8G8B8A8_UNORM;

		device->CreatePipelineState(&desc, &PSO_paintdecal, &renderpass_info);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_DOWNSAMPLEDEPTHBUFFER];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.dss = &depthStencils[DSSTYPE_WRITEONLY];

		device->CreatePipelineState(&desc, &PSO_downsampledepthbuffer);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_upsample_bilateral);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_UPSAMPLE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_volumetricclouds_upsample);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_POSTPROCESS];
		desc.ps = &shaders[PSTYPE_POSTPROCESS_OUTLINE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.bs = &blendStates[BSTYPE_TRANSPARENT];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];

		device->CreatePipelineState(&desc, &PSO_outline);
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		desc.vs = &shaders[VSTYPE_SCREEN];
		desc.ps = &shaders[PSTYPE_COPY_DEPTH];
		desc.bs = &blendStates[BSTYPE_OPAQUE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.dss = &depthStencils[DSSTYPE_WRITEONLY];
		device->CreatePipelineState(&desc, &PSO_copyDepth);

		desc.ps = &shaders[PSTYPE_COPY_STENCIL_BIT];
		for (int i = 0; i < 8; ++i)
		{
			desc.dss = &depthStencils[DSSTYPE_COPY_STENCIL_BIT_0 + i];
			device->CreatePipelineState(&desc, &PSO_copyStencilBit[i]);
		}

		desc.ps = &shaders[PSTYPE_COPY_STENCIL_BIT_MSAA];
		for (int i = 0; i < 8; ++i)
		{
			desc.dss = &depthStencils[DSSTYPE_COPY_STENCIL_BIT_0 + i];
			device->CreatePipelineState(&desc, &PSO_copyStencilBit_MSAA[i]);
		}

		desc.ps = &shaders[PSTYPE_EXTRACT_STENCIL_BIT];
		for (int i = 0; i < 8; ++i)
		{
			desc.dss = &depthStencils[DSSTYPE_EXTRACT_STENCIL_BIT_0 + i];
			device->CreatePipelineState(&desc, &PSO_extractStencilBit[i]);
		}
		});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		desc.vs = &shaders[VSTYPE_SCREEN];
		desc.ps = &shaders[PSTYPE_WAVE_EFFECT];
		desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
		device->CreatePipelineState(&desc, &PSO_waveeffect);
	});
	wi::jobsystem::Execute(ctx, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.vs = &shaders[VSTYPE_LENSFLARE];
		desc.ps = &shaders[PSTYPE_LENSFLARE];
		desc.bs = &blendStates[BSTYPE_ADDITIVE];
		desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
		desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
		desc.pt = PrimitiveTopology::TRIANGLESTRIP;

		device->CreatePipelineState(&desc, &PSO_lensflare);
		});
	wi::jobsystem::Dispatch(ctx, SKYRENDERING_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;
		desc.rs = &rasterizers[RSTYPE_SKY];
		desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];

		switch (args.jobIndex)
		{
		case SKYRENDERING_STATIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SKY_STATIC];
			break;
		case SKYRENDERING_DYNAMIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SKY_DYNAMIC];
			break;
		case SKYRENDERING_SUN:
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.vs = &shaders[VSTYPE_SKY];
			desc.ps = &shaders[PSTYPE_SUN];
			break;
		case SKYRENDERING_ENVMAPCAPTURE_STATIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_ENVMAP_SKY];
			desc.ps = &shaders[PSTYPE_ENVMAP_SKY_STATIC];
			break;
		case SKYRENDERING_ENVMAPCAPTURE_DYNAMIC:
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.vs = &shaders[VSTYPE_ENVMAP_SKY];
			desc.ps = &shaders[PSTYPE_ENVMAP_SKY_DYNAMIC];
			break;
		}

		device->CreatePipelineState(&desc, &PSO_sky[args.jobIndex]);
		});
	wi::jobsystem::Dispatch(ctx, DEBUGRENDERING_COUNT, 1, [](wi::jobsystem::JobArgs args) {
		PipelineStateDesc desc;

		switch (args.jobIndex)
		{
		case DEBUGRENDERING_ENVPROBE:
			desc.vs = &shaders[VSTYPE_SPHERE];
			desc.ps = &shaders[PSTYPE_CUBEMAP];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_DDGI:
			desc.vs = &shaders[VSTYPE_DDGI_DEBUG];
			desc.ps = &shaders[PSTYPE_DDGI_DEBUG];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_GRID:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_CUBE:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_CUBE_DEPTH:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_LINES:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_LINES_DEPTH:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::LINELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_SOLID:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_WIREFRAME:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_SOLID_DEPTH:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_TRIANGLE_WIREFRAME_DEPTH:
			desc.vs = &shaders[VSTYPE_VERTEXCOLOR];
			desc.ps = &shaders[PSTYPE_VERTEXCOLOR];
			desc.il = &inputLayouts[ILTYPE_VERTEXCOLOR];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_EMITTER:
			desc.vs = &shaders[VSTYPE_OBJECT_DEBUG];
			desc.ps = &shaders[PSTYPE_OBJECT_DEBUG];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_PAINTRADIUS:
			desc.vs = &shaders[VSTYPE_OBJECT_SIMPLE];
			desc.ps = &shaders[PSTYPE_OBJECT_PAINTRADIUS];
			desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_VOXEL:
			desc.vs = &shaders[VSTYPE_VOXEL];
			desc.ps = &shaders[PSTYPE_VOXEL];
			desc.gs = &shaders[GSTYPE_VOXEL];
			desc.dss = &depthStencils[DSSTYPE_DEFAULT];
			desc.rs = &rasterizers[RSTYPE_BACK];
			desc.bs = &blendStates[BSTYPE_OPAQUE];
			desc.pt = PrimitiveTopology::POINTLIST;
			break;
		case DEBUGRENDERING_FORCEFIELD_POINT:
			desc.vs = &shaders[VSTYPE_FORCEFIELDVISUALIZER_POINT];
			desc.ps = &shaders[PSTYPE_FORCEFIELDVISUALIZER];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_BACK];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		case DEBUGRENDERING_FORCEFIELD_PLANE:
			desc.vs = &shaders[VSTYPE_FORCEFIELDVISUALIZER_PLANE];
			desc.ps = &shaders[PSTYPE_FORCEFIELDVISUALIZER];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_FRONT];
			desc.bs = &blendStates[BSTYPE_ADDITIVE];
			desc.pt = PrimitiveTopology::TRIANGLESTRIP;
			break;
		case DEBUGRENDERING_RAYTRACE_BVH:
			desc.vs = &shaders[VSTYPE_RAYTRACE_SCREEN];
			desc.ps = &shaders[PSTYPE_RAYTRACE_DEBUGBVH];
			desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
			desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
			desc.bs = &blendStates[BSTYPE_TRANSPARENT];
			desc.pt = PrimitiveTopology::TRIANGLELIST;
			break;
		}

		device->CreatePipelineState(&desc, &PSO_debug[args.jobIndex]);
		});

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
	if(device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		wi::jobsystem::Execute(raytracing_ctx, [](wi::jobsystem::JobArgs args) {

			bool success = LoadShader(ShaderStage::LIB, shaders[RTTYPE_RTREFLECTION], "rtreflectionLIB.cso");
			assert(success);

			RaytracingPipelineStateDesc rtdesc;
			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_Raygen";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::RAYGENERATION;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_ClosestHit";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::CLOSESTHIT;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_AnyHit";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::ANYHIT;

			rtdesc.shader_libraries.emplace_back();
			rtdesc.shader_libraries.back().shader = &shaders[RTTYPE_RTREFLECTION];
			rtdesc.shader_libraries.back().function_name = "RTReflection_Miss";
			rtdesc.shader_libraries.back().type = ShaderLibrary::Type::MISS;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::GENERAL;
			rtdesc.hit_groups.back().name = "RTReflection_Raygen";
			rtdesc.hit_groups.back().general_shader = 0;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::GENERAL;
			rtdesc.hit_groups.back().name = "RTReflection_Miss";
			rtdesc.hit_groups.back().general_shader = 3;

			rtdesc.hit_groups.emplace_back();
			rtdesc.hit_groups.back().type = ShaderHitGroup::Type::TRIANGLES;
			rtdesc.hit_groups.back().name = "RTReflection_Hitgroup";
			rtdesc.hit_groups.back().closest_hit_shader = 1;
			rtdesc.hit_groups.back().any_hit_shader = 2;

			rtdesc.max_trace_recursion_depth = 1;
			rtdesc.max_payload_size_in_bytes = sizeof(float4);
			rtdesc.max_attribute_size_in_bytes = sizeof(float2); // bary
			success = device->CreateRaytracingPipelineState(&rtdesc, &RTPSO_reflection);


		});
	};
#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

	// Clear custom shaders (Custom shaders coming from user will need to be handled by the user in case of shader reload):
	customShaders.clear();

	// Hologram sample shader will be registered as custom shader:
	//	It's best to register all custom shaders from the same thread, so under here
	//	or after engine has been completely initialized
	//	This is because RegisterCustomShader() will give out IDs in increasing order
	//	and you can keep the order stable by ensuring they are registered in order.
	{
		SHADERTYPE realVS = GetVSTYPE(RENDERPASS_MAIN, false, false, true);

		PipelineStateDesc desc;
		desc.vs = &shaders[realVS];
		desc.ps = &shaders[PSTYPE_OBJECT_HOLOGRAM];

		desc.bs = &blendStates[BSTYPE_ADDITIVE];
		desc.rs = &rasterizers[RSTYPE_FRONT];
		desc.dss = &depthStencils[DSSTYPE_HOLOGRAM];
		desc.pt = PrimitiveTopology::TRIANGLELIST;

		PipelineState pso;
		device->CreatePipelineState(&desc, &pso);

		CustomShader customShader;
		customShader.name = "Hologram";
		customShader.filterMask = FILTER_TRANSPARENT;
		customShader.pso[RENDERPASS_MAIN] = pso;
		RegisterCustomShader(customShader);
	}

	wi::jobsystem::Wait(ctx);


	// default objectshaders:
	//	We don't wait for these here, because then it can slow down the init time a lot
	//	We compile PipelineStates on backround threads, but we will add them to be useable by GetObjectPSO on the main thread (wi::eventhandler::EVENT_THREAD_SAFE_POINT)
	//	The RenderMeshes that uses these pipeline states will be checking the PipelineState.IsValid() and skip draws if the pipeline is not yet valid
	wi::jobsystem::Wait(object_pso_job_ctx);
	object_pso_job_ctx.priority = wi::jobsystem::Priority::Low;
	for (uint32_t renderPass = 0; renderPass < RENDERPASS_COUNT; ++renderPass)
	{
		for (uint32_t shaderType = 0; shaderType < MaterialComponent::SHADERTYPE_COUNT; ++shaderType)
		{
			for (uint32_t mesh_shader = 0; mesh_shader <= (device->CheckCapability(GraphicsDeviceCapability::MESH_SHADER) ? 1u : 0u); ++mesh_shader)
			{
				wi::jobsystem::Execute(object_pso_job_ctx, [=](wi::jobsystem::JobArgs args) {
					for (uint32_t blendMode = 0; blendMode < BLENDMODE_COUNT; ++blendMode)
					{
						for (uint32_t cullMode = 0; cullMode <= 3; ++cullMode)
						{
							for (uint32_t tessellation = 0; tessellation <= 1; ++tessellation)
							{
								if (tessellation && renderPass > RENDERPASS_PREPASS_DEPTHONLY)
									continue;
								for (uint32_t alphatest = 0; alphatest <= 1; ++alphatest)
								{
									const bool transparency = blendMode != BLENDMODE_OPAQUE;
									if ((renderPass == RENDERPASS_PREPASS || renderPass == RENDERPASS_PREPASS_DEPTHONLY) && transparency)
										continue;

									PipelineStateDesc desc;

									if (mesh_shader)
									{
										if (tessellation)
											continue;
										SHADERTYPE realAS = GetASTYPE((RENDERPASS)renderPass, tessellation, alphatest, transparency, mesh_shader);
										SHADERTYPE realMS = GetMSTYPE((RENDERPASS)renderPass, tessellation, alphatest, transparency, mesh_shader);
										if (realMS == SHADERTYPE_COUNT)
											continue;
										desc.as = realAS < SHADERTYPE_COUNT ? &shaders[realAS] : nullptr;
										desc.ms = realMS < SHADERTYPE_COUNT ? &shaders[realMS] : nullptr;
									}
									else
									{
										SHADERTYPE realVS = GetVSTYPE((RENDERPASS)renderPass, tessellation, alphatest, transparency);
										SHADERTYPE realHS = GetHSTYPE((RENDERPASS)renderPass, tessellation, alphatest);
										SHADERTYPE realDS = GetDSTYPE((RENDERPASS)renderPass, tessellation, alphatest);
										SHADERTYPE realGS = GetGSTYPE((RENDERPASS)renderPass, alphatest, transparency);

										if (tessellation && (realHS == SHADERTYPE_COUNT || realDS == SHADERTYPE_COUNT))
											continue;

										desc.vs = realVS < SHADERTYPE_COUNT ? &shaders[realVS] : nullptr;
										desc.hs = realHS < SHADERTYPE_COUNT ? &shaders[realHS] : nullptr;
										desc.ds = realDS < SHADERTYPE_COUNT ? &shaders[realDS] : nullptr;
										desc.gs = realGS < SHADERTYPE_COUNT ? &shaders[realGS] : nullptr;
									}

									SHADERTYPE realPS = GetPSTYPE((RENDERPASS)renderPass, alphatest, transparency, (MaterialComponent::SHADERTYPE)shaderType);
									desc.ps = realPS < SHADERTYPE_COUNT ? &shaders[realPS] : nullptr;

									switch (blendMode)
									{
									case BLENDMODE_OPAQUE:
										desc.bs = &blendStates[BSTYPE_OPAQUE];
										break;
									case BLENDMODE_ALPHA:
										desc.bs = &blendStates[BSTYPE_TRANSPARENT];
										break;
									case BLENDMODE_ADDITIVE:
										desc.bs = &blendStates[BSTYPE_ADDITIVE];
										break;
									case BLENDMODE_PREMULTIPLIED:
										desc.bs = &blendStates[BSTYPE_PREMULTIPLIED];
										break;
									case BLENDMODE_MULTIPLY:
										desc.bs = &blendStates[BSTYPE_MULTIPLY];
										break;
									case BLENDMODE_INVERSE:
										desc.bs = &blendStates[BSTYPE_INVERSE];
										break;
									default:
										assert(0);
										break;
									}

									switch (renderPass)
									{
									case RENDERPASS_SHADOW:
										desc.bs = &blendStates[transparency ? BSTYPE_TRANSPARENTSHADOW : BSTYPE_COLORWRITEDISABLE];
										break;
									case RENDERPASS_RAINBLOCKER:
										desc.bs = &blendStates[BSTYPE_COLORWRITEDISABLE];
										break;
									default:
										break;
									}

									switch (renderPass)
									{
									case RENDERPASS_SHADOW:
										desc.dss = &depthStencils[transparency ? DSSTYPE_DEPTHREAD : DSSTYPE_SHADOW];
										break;
									case RENDERPASS_MAIN:
										if (blendMode == BLENDMODE_ADDITIVE)
										{
											desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
										}
										else
										{
											desc.dss = &depthStencils[transparency ? DSSTYPE_TRANSPARENT : DSSTYPE_DEPTHREADEQUAL];
										}
										break;
									case RENDERPASS_ENVMAPCAPTURE:
										desc.dss = &depthStencils[DSSTYPE_ENVMAP];
										break;
									case RENDERPASS_VOXELIZE:
										desc.dss = &depthStencils[DSSTYPE_DEPTHDISABLED];
										break;
									case RENDERPASS_RAINBLOCKER:
										desc.dss = &depthStencils[DSSTYPE_DEFAULT];
										break;
									default:
										if (blendMode == BLENDMODE_ADDITIVE)
										{
											desc.dss = &depthStencils[DSSTYPE_DEPTHREAD];
										}
										else
										{
											desc.dss = &depthStencils[DSSTYPE_DEFAULT];
										}
										break;
									}

									switch (renderPass)
									{
									case RENDERPASS_SHADOW:
										desc.rs = &rasterizers[cullMode == (int)CullMode::NONE ? RSTYPE_SHADOW_DOUBLESIDED : RSTYPE_SHADOW];
										break;
									case RENDERPASS_VOXELIZE:
										desc.rs = &rasterizers[RSTYPE_VOXELIZE];
										break;
									default:
										switch ((CullMode)cullMode)
										{
										default:
										case CullMode::BACK:
											desc.rs = &rasterizers[RSTYPE_FRONT];
											break;
										case CullMode::NONE:
											desc.rs = &rasterizers[RSTYPE_DOUBLESIDED];
											break;
										case CullMode::FRONT:
											desc.rs = &rasterizers[RSTYPE_BACK];
											break;
										}
										break;
									}

									if (tessellation)
									{
										desc.pt = PrimitiveTopology::PATCHLIST;
									}
									else
									{
										desc.pt = PrimitiveTopology::TRIANGLELIST;
									}

									wi::jobsystem::Wait(objectps_ctx);
									if (mesh_shader)
									{
										wi::jobsystem::Wait(mesh_shader_ctx);
									}

									if (wi::jobsystem::IsShuttingDown())
										return;

									ObjectRenderingVariant variant = {};
									variant.bits.renderpass = renderPass;
									variant.bits.shadertype = shaderType;
									variant.bits.blendmode = blendMode;
									variant.bits.cullmode = cullMode;
									variant.bits.tessellation = tessellation;
									variant.bits.alphatest = alphatest;
									variant.bits.sample_count = 1;
									variant.bits.mesh_shader = mesh_shader;

									switch (renderPass)
									{
									case RENDERPASS_MAIN:
									case RENDERPASS_PREPASS:
									case RENDERPASS_PREPASS_DEPTHONLY:
									{
										RenderPassInfo renderpass_info;
										if (renderPass == RENDERPASS_PREPASS_DEPTHONLY)
										{
											renderpass_info.rt_count = 0;
											renderpass_info.rt_formats[0] = Format::UNKNOWN;
										}
										else
										{
											renderpass_info.rt_count = 1;
											renderpass_info.rt_formats[0] = renderPass == RENDERPASS_MAIN ? format_rendertarget_main : format_idbuffer;
										}
										renderpass_info.ds_format = format_depthbuffer_main;
										const uint32_t msaa_support[] = { 1,2,4,8 };
										for (uint32_t msaa : msaa_support)
										{
											variant.bits.sample_count = msaa;
											renderpass_info.sample_count = msaa;
											PipelineState pso;
											device->CreatePipelineState(&desc, &pso, &renderpass_info);
											wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
												*GetObjectPSO(variant) = pso;
												});
										}
									}
									break;

									case RENDERPASS_ENVMAPCAPTURE:
									{
										RenderPassInfo renderpass_info;
										renderpass_info.rt_count = 1;
										renderpass_info.rt_formats[0] = format_rendertarget_envprobe;
										renderpass_info.ds_format = format_depthbuffer_envprobe;
										const uint32_t msaa_support[] = { 1,8 };
										for (uint32_t msaa : msaa_support)
										{
											variant.bits.sample_count = msaa;
											renderpass_info.sample_count = msaa;
											PipelineState pso;
											device->CreatePipelineState(&desc, &pso, &renderpass_info);
											wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
												*GetObjectPSO(variant) = pso;
												});
										}
									}
									break;

									case RENDERPASS_SHADOW:
									{
										RenderPassInfo renderpass_info;
										renderpass_info.rt_count = 1;
										renderpass_info.rt_formats[0] = format_rendertarget_shadowmap;
										renderpass_info.ds_format = format_depthbuffer_shadowmap;
										PipelineState pso;
										device->CreatePipelineState(&desc, &pso, &renderpass_info);
										wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
											*GetObjectPSO(variant) = pso;
											});
									}
									break;

									case RENDERPASS_RAINBLOCKER:
									{
										RenderPassInfo renderpass_info;
										renderpass_info.rt_count = 0;
										renderpass_info.ds_format = format_depthbuffer_shadowmap;
										PipelineState pso;
										device->CreatePipelineState(&desc, &pso, &renderpass_info);
										wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
											*GetObjectPSO(variant) = pso;
											});
									}
									break;

									default:
										PipelineState pso;
										device->CreatePipelineState(&desc, &pso);
										wi::eventhandler::Subscribe_Once(wi::eventhandler::EVENT_THREAD_SAFE_POINT, [=](uint64_t userdata) {
											*GetObjectPSO(variant) = pso;
											});
										break;
									}
								}
							}
						}
					}
				});
			}
		}
	}

}
int IsPipelineCreationActive()
{
	int ret = 0;
	ret += raytracing_ctx.counter.load();
	ret += objectps_ctx.counter.load();
	ret += mesh_shader_ctx.counter.load();
	ret += object_pso_job_ctx.counter.load();
	return ret;
}
void LoadBuffers()
{
	GPUBufferDesc bd;
	bd.usage = Usage::DEFAULT;
	bd.size = sizeof(FrameCB);
	bd.bind_flags = BindFlag::CONSTANT_BUFFER;
	device->CreateBuffer(&bd, nullptr, &buffers[BUFFERTYPE_FRAMECB]);
	device->SetName(&buffers[BUFFERTYPE_FRAMECB], "buffers[BUFFERTYPE_FRAMECB]");

	bd.size = sizeof(IndirectDrawArgsInstanced) + (sizeof(XMFLOAT4) + sizeof(XMFLOAT4)) * 1000;
	bd.bind_flags = BindFlag::VERTEX_BUFFER | BindFlag::UNORDERED_ACCESS;
	bd.misc_flags = ResourceMiscFlag::BUFFER_RAW | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBufferZeroed(&bd, &buffers[BUFFERTYPE_INDIRECT_DEBUG_0]);
	device->SetName(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], "buffers[BUFFERTYPE_INDIRECT_DEBUG_0]");
	device->CreateBufferZeroed(&bd, &buffers[BUFFERTYPE_INDIRECT_DEBUG_1]);
	device->SetName(&buffers[BUFFERTYPE_INDIRECT_DEBUG_1], "buffers[BUFFERTYPE_INDIRECT_DEBUG_1]");

	bd.size = sizeof(IndirectDrawArgsInstanced);
	bd.usage = Usage::READBACK;
	bd.bind_flags = {};
	bd.misc_flags = {};
	for (auto& buf : indirectDebugStatsReadback)
	{
		device->CreateBufferZeroed(&bd, &buf);
		device->SetName(&buf, "indirectDebugStatsReadback");
	}

	{
		TextureDesc desc;
		desc.bind_flags = BindFlag::SHADER_RESOURCE;
		desc.format = Format::R8_UNORM;
		desc.height = 16;
		desc.width = 16;
		SubresourceData InitData;
		InitData.data_ptr = sheenLUTdata;
		InitData.row_pitch = desc.width;
		device->CreateTexture(&desc, &InitData, &textures[TEXTYPE_2D_SHEENLUT]);
		device->SetName(&textures[TEXTYPE_2D_SHEENLUT], "textures[TEXTYPE_2D_SHEENLUT]");
	}

	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_3D;
		desc.format = Format::R16_FLOAT;
		desc.width = 32;
		desc.height = 32;
		desc.depth = 32;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_3D_WIND]);
		device->SetName(&textures[TEXTYPE_3D_WIND], "textures[TEXTYPE_3D_WIND]");
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_3D_WIND_PREV]);
		device->SetName(&textures[TEXTYPE_3D_WIND_PREV], "textures[TEXTYPE_3D_WIND_PREV]");
	}
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.format = Format::R8G8B8A8_UNORM;
		desc.width = 256;
		desc.height = 256;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_CAUSTICS]);
		device->SetName(&textures[TEXTYPE_2D_CAUSTICS], "textures[TEXTYPE_2D_CAUSTICS]");
	}
}
void SetUpStates()
{
	RasterizerState rs;
	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_FRONT] = rs;


	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	if (IsFormatUnorm(format_depthbuffer_shadowmap))
	{
		rs.depth_bias = -1;
	}
	else
	{
		rs.depth_bias = -1000;
	}
	rs.slope_scaled_depth_bias = -6.0f;
	rs.depth_bias_clamp = 0;
	rs.depth_clip_enable = false;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_SHADOW] = rs;
	rs.cull_mode = CullMode::NONE;
	rasterizers[RSTYPE_SHADOW_DOUBLESIDED] = rs;

	rs.fill_mode = FillMode::WIREFRAME;
	rs.cull_mode = CullMode::BACK;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_WIRE] = rs;
	rs.antialiased_line_enable = true;
	rasterizers[RSTYPE_WIRE_SMOOTH] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_DOUBLESIDED] = rs;

	rs.fill_mode = FillMode::WIREFRAME;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_WIRE_DOUBLESIDED] = rs;
	rs.antialiased_line_enable = true;
	rasterizers[RSTYPE_WIRE_DOUBLESIDED_SMOOTH] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::FRONT;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_BACK] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = false;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_OCCLUDEE] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::FRONT;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = false;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
	rs.conservative_rasterization_enable = false;
	rasterizers[RSTYPE_SKY] = rs;

	rs.fill_mode = FillMode::SOLID;
	rs.cull_mode = CullMode::NONE;
	rs.front_counter_clockwise = true;
	rs.depth_bias = 0;
	rs.depth_bias_clamp = 0;
	rs.slope_scaled_depth_bias = 0;
	rs.depth_clip_enable = true;
	rs.multisample_enable = false;
	rs.antialiased_line_enable = false;
#ifdef VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	if (device->CheckCapability(GraphicsDeviceCapability::CONSERVATIVE_RASTERIZATION))
	{
		rs.conservative_rasterization_enable = true;
	}
	else
#endif // VOXELIZATION_CONSERVATIVE_RASTERIZATION_ENABLED
	{
		rs.forced_sample_count = 8;
	}
	rasterizers[RSTYPE_VOXELIZE] = rs;


	rs = rasterizers[RSTYPE_DOUBLESIDED];
	if (device->CheckCapability(GraphicsDeviceCapability::CONSERVATIVE_RASTERIZATION))
	{
		rs.conservative_rasterization_enable = true;
	}
	rasterizers[RSTYPE_LIGHTMAP] = rs;



	DepthStencilState dsd;
	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;

	dsd.stencil_enable = true;
	dsd.stencil_read_mask = 0;
	dsd.stencil_write_mask = 0xFF;
	dsd.front_face.stencil_func = ComparisonFunc::ALWAYS;
	dsd.front_face.stencil_pass_op = StencilOp::REPLACE;
	dsd.front_face.stencil_fail_op = StencilOp::KEEP;
	dsd.front_face.stencil_depth_fail_op = StencilOp::KEEP;
	dsd.back_face.stencil_func = ComparisonFunc::ALWAYS;
	dsd.back_face.stencil_pass_op = StencilOp::REPLACE;
	dsd.back_face.stencil_fail_op = StencilOp::KEEP;
	dsd.back_face.stencil_depth_fail_op = StencilOp::KEEP;
	depthStencils[DSSTYPE_DEFAULT] = dsd;

	dsd.depth_func = ComparisonFunc::GREATER_EQUAL;
	depthStencils[DSSTYPE_TRANSPARENT] = dsd;
	dsd.depth_func = ComparisonFunc::GREATER;

	dsd.depth_write_mask = DepthWriteMask::ZERO;
	depthStencils[DSSTYPE_HOLOGRAM] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_SHADOW] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_CAPTUREIMPOSTOR] = dsd;


	dsd.depth_enable = true;
	dsd.stencil_enable = false;
	dsd.depth_write_mask = DepthWriteMask::ZERO;
	dsd.depth_func = ComparisonFunc::GREATER_EQUAL;
	depthStencils[DSSTYPE_DEPTHREAD] = dsd;

	dsd.depth_enable = false;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_DEPTHDISABLED] = dsd;


	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ZERO;
	dsd.depth_func = ComparisonFunc::EQUAL;
	depthStencils[DSSTYPE_DEPTHREADEQUAL] = dsd;


	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::GREATER;
	depthStencils[DSSTYPE_ENVMAP] = dsd;

	dsd.depth_enable = true;
	dsd.depth_write_mask = DepthWriteMask::ALL;
	dsd.depth_func = ComparisonFunc::ALWAYS;
	dsd.stencil_enable = false;
	depthStencils[DSSTYPE_WRITEONLY] = dsd;

	dsd.depth_enable = false;
	dsd.depth_write_mask = DepthWriteMask::ZERO;
	dsd.stencil_enable = true;
	dsd.stencil_read_mask = 0;
	dsd.front_face.stencil_func = ComparisonFunc::ALWAYS;
	dsd.front_face.stencil_pass_op = StencilOp::REPLACE;
	dsd.back_face = dsd.front_face;
	for (int i = 0; i < 8; ++i)
	{
		dsd.stencil_write_mask = uint8_t(1 << i);
		depthStencils[DSSTYPE_COPY_STENCIL_BIT_0 + i] = dsd;
	}

	dsd.stencil_write_mask = 0;
	dsd.front_face.stencil_func = ComparisonFunc::EQUAL;
	dsd.front_face.stencil_pass_op = StencilOp::KEEP;
	dsd.back_face = dsd.front_face;
	for (int i = 0; i < 8; ++i)
	{
		dsd.stencil_read_mask = uint8_t(1 << i);
		depthStencils[DSSTYPE_EXTRACT_STENCIL_BIT_0 + i] = dsd;
	}


	BlendState bd;
	bd.render_target[0].blend_enable = false;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_OPAQUE] = bd;

	bd.render_target[0].src_blend = Blend::SRC_ALPHA;
	bd.render_target[0].dest_blend = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_TRANSPARENT] = bd;

	bd.render_target[0].blend_enable = true;
	bd.render_target[0].src_blend = Blend::ONE;
	bd.render_target[0].dest_blend = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::INV_SRC_ALPHA;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.independent_blend_enable = false;
	bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_PREMULTIPLIED] = bd;


	bd.render_target[0].blend_enable = true;
	bd.render_target[0].src_blend = Blend::SRC_ALPHA;
	bd.render_target[0].dest_blend = Blend::ONE;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ZERO;
	bd.render_target[0].dest_blend_alpha = Blend::ONE;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.independent_blend_enable = false,
		bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_ADDITIVE] = bd;


	bd.render_target[0].blend_enable = false;
	bd.render_target[0].render_target_write_mask = ColorWrite::DISABLE;
	bd.independent_blend_enable = false,
		bd.alpha_to_coverage_enable = false;
	blendStates[BSTYPE_COLORWRITEDISABLE] = bd;

	bd.render_target[0].src_blend = Blend::DEST_COLOR;
	bd.render_target[0].dest_blend = Blend::ZERO;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::DEST_ALPHA;
	bd.render_target[0].dest_blend_alpha = Blend::ZERO;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_MULTIPLY] = bd;

	bd.render_target[0].src_blend = Blend::INV_DEST_COLOR;
	bd.render_target[0].dest_blend = Blend::ZERO;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::DEST_ALPHA;
	bd.render_target[0].dest_blend_alpha = Blend::ZERO;
	bd.render_target[0].blend_op_alpha = BlendOp::ADD;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_INVERSE] = bd;

	bd.render_target[0].src_blend = Blend::ZERO;
	bd.render_target[0].dest_blend = Blend::SRC_COLOR;
	bd.render_target[0].blend_op = BlendOp::ADD;
	bd.render_target[0].src_blend_alpha = Blend::ONE;
	bd.render_target[0].dest_blend_alpha = Blend::ONE;
	bd.render_target[0].blend_op_alpha = BlendOp::MAX;
	bd.render_target[0].blend_enable = true;
	bd.render_target[0].render_target_write_mask = ColorWrite::ENABLE_ALL;
	bd.alpha_to_coverage_enable = false;
	bd.independent_blend_enable = false;
	blendStates[BSTYPE_TRANSPARENTSHADOW] = bd;





	SamplerDesc samplerDesc;
	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	samplerDesc.mip_lod_bias = 0.0f;
	samplerDesc.max_anisotropy = 0;
	samplerDesc.comparison_func = ComparisonFunc::NEVER;
	samplerDesc.border_color = SamplerBorderColor::TRANSPARENT_BLACK;
	samplerDesc.min_lod = 0;
	samplerDesc.max_lod = std::numeric_limits<float>::max();
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_MIRROR]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_CLAMP]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_LINEAR;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_LINEAR_WRAP]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_MIRROR]);

	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_WRAP]);


	samplerDesc.filter = Filter::MIN_MAG_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_POINT_CLAMP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_CLAMP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_WRAP]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::MIRROR;
	samplerDesc.address_v = TextureAddressMode::MIRROR;
	samplerDesc.address_w = TextureAddressMode::MIRROR;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_ANISO_MIRROR]);

	samplerDesc.filter = Filter::ANISOTROPIC;
	samplerDesc.address_u = TextureAddressMode::WRAP;
	samplerDesc.address_v = TextureAddressMode::WRAP;
	samplerDesc.address_w = TextureAddressMode::WRAP;
	samplerDesc.max_anisotropy = 16;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_OBJECTSHADER]);

	samplerDesc.filter = Filter::COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplerDesc.address_u = TextureAddressMode::CLAMP;
	samplerDesc.address_v = TextureAddressMode::CLAMP;
	samplerDesc.address_w = TextureAddressMode::CLAMP;
	samplerDesc.mip_lod_bias = 0.0f;
	samplerDesc.max_anisotropy = 0;
	samplerDesc.comparison_func = ComparisonFunc::GREATER_EQUAL;
	device->CreateSampler(&samplerDesc, &samplers[SAMPLER_CMP_DEPTH]);
}

const GPUBuffer& GetIndexBufferForQuads(uint32_t max_quad_count)
{
	const size_t required_max_index = max_quad_count * 4u;

	static std::mutex locker;
	std::scoped_lock lock(locker);

	if (required_max_index < 65536u)
	{
		// 16-bit request:
		max_quad_count = std::max(65535u / 4u, max_quad_count); // minimum a full 16 bit index buffer request, avoid allocating multiple 16-bit small requests
		const size_t required_index_count = max_quad_count * 6u;

		static GPUBuffer indexBufferForQuads16;
		if (!indexBufferForQuads16.IsValid() || indexBufferForQuads16.desc.size / indexBufferForQuads16.desc.stride < required_index_count)
		{
			GPUBufferDesc bd;
			bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::INDEX_BUFFER;
			if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
			{
				bd.misc_flags |= ResourceMiscFlag::RAY_TRACING;
			}
			bd.format = Format::R16_UINT;
			bd.stride = GetFormatStride(bd.format);
			bd.size = bd.stride * required_index_count;
			auto fill_ib = [&](void* dst)
			{
				uint16_t* primitiveData = (uint16_t*)dst;
				for (uint16_t particleID = 0; particleID < uint16_t(max_quad_count); ++particleID)
				{
					uint16_t v0 = particleID * 4;
					uint32_t i0 = particleID * 6;
					primitiveData[i0 + 0] = v0 + 0;
					primitiveData[i0 + 1] = v0 + 1;
					primitiveData[i0 + 2] = v0 + 2;
					primitiveData[i0 + 3] = v0 + 2;
					primitiveData[i0 + 4] = v0 + 1;
					primitiveData[i0 + 5] = v0 + 3;
				}
			};
			device->CreateBuffer2(&bd, fill_ib, &indexBufferForQuads16);
			device->SetName(&indexBufferForQuads16, "wi::renderer::indexBufferForQuads16bit");
		}
		return indexBufferForQuads16;
	}

	// 32-bit request below:
	max_quad_count = wi::math::GetNextPowerOfTwo(max_quad_count); // reduce allocations by making larger fitting allocations
	const size_t required_index_count = max_quad_count * 6u;

	static GPUBuffer indexBufferForQuads32;
	if (!indexBufferForQuads32.IsValid() || indexBufferForQuads32.desc.size / indexBufferForQuads32.desc.stride < required_index_count)
	{
		GPUBufferDesc bd;
		bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::INDEX_BUFFER;
		if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		{
			bd.misc_flags |= ResourceMiscFlag::RAY_TRACING;
		}
		bd.format = Format::R32_UINT;
		bd.stride = GetFormatStride(bd.format);
		bd.size = bd.stride * required_index_count;
		auto fill_ib = [&](void* dst)
		{
			uint32_t* primitiveData = (uint32_t*)dst;
			for (uint particleID = 0; particleID < max_quad_count; ++particleID)
			{
				uint32_t v0 = particleID * 4;
				uint32_t i0 = particleID * 6;
				primitiveData[i0 + 0] = v0 + 0;
				primitiveData[i0 + 1] = v0 + 1;
				primitiveData[i0 + 2] = v0 + 2;
				primitiveData[i0 + 3] = v0 + 2;
				primitiveData[i0 + 4] = v0 + 1;
				primitiveData[i0 + 5] = v0 + 3;
			}
		};
		device->CreateBuffer2(&bd, fill_ib, &indexBufferForQuads32);
		device->SetName(&indexBufferForQuads32, "wi::renderer::indexBufferForQuads32bit");
	}

	return indexBufferForQuads32;
}

// This is responsible to manage big chunks of GPUBuffer, each of which will be used for suballocations:
struct GPUSubAllocator
{
	static constexpr uint64_t blocksize = 256ull * 1024ull * 1024ull; // 256 MB
	struct Block
	{
		wi::allocator::PageAllocator allocator;
		GPUBuffer buffer;
	};
	wi::vector<Block> blocks;
	std::mutex locker;
} static suballocator;
BufferSuballocation SuballocateGPUBuffer(uint64_t size)
{
	if (size > GPUSubAllocator::blocksize / 2)
		return {}; // invalid, larger allocations than half block size will not be suballocated

	// scoped for locker
	{
		std::scoped_lock lock(suballocator.locker);

		// See if any of the large blocks can fulfill the allocation request:
		BufferSuballocation allocation;
		for (auto& block : suballocator.blocks)
		{
			allocation.allocation = block.allocator.allocate(size);
			if (allocation.allocation.IsValid())
			{
				allocation.alias = block.buffer;
				//wilog("SuballocateGPUBuffer allocated size: %s, pages: %d, free space remaining: %s", wi::helper::GetMemorySizeText(size).c_str(), block.allocator.page_count_from_bytes(size), wi::helper::GetMemorySizeText(allocation.allocation.allocator->allocator.storageReport().totalFreeSpace * block.allocator.page_size).c_str());
				return allocation;
			}
		}

		// Allocation couldn't be fulfilled, create new block:
		GPUBufferDesc desc;
		desc.size = GPUSubAllocator::blocksize;
		if (device->CheckCapability(GraphicsDeviceCapability::CACHE_COHERENT_UMA))
		{
			// In UMA mode, it is better to create UPLOAD buffer, this avoids one copy from UPLOAD to DEFAULT
			desc.usage = Usage::UPLOAD;
		}
		else
		{
			desc.usage = Usage::DEFAULT;
		}
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::VERTEX_BUFFER | BindFlag::INDEX_BUFFER;
		desc.misc_flags = ResourceMiscFlag::ALIASING_BUFFER | ResourceMiscFlag::NO_DEFAULT_DESCRIPTORS;
		desc.alignment = device->GetMinOffsetAlignment(&desc);
		auto& block = suballocator.blocks.emplace_back();
		bool success = device->CreateBuffer(&desc, nullptr, &block.buffer);
		assert(success);
		device->SetName(&block.buffer, "GPUSubAllocator");
		block.allocator.init(desc.size, (uint32_t)desc.alignment, true);
		wilog("SuballocateGPUBuffer created buffer block with size: %s, with page size: %s, page count: %d", wi::helper::GetMemorySizeText(block.allocator.total_size_in_bytes()).c_str(), wi::helper::GetMemorySizeText(block.allocator.page_size).c_str(), (int)block.allocator.page_count);
	}
	return SuballocateGPUBuffer(size); // retry
}
void UpdateGPUSuballocator()
{
	std::scoped_lock lock(suballocator.locker);
	for (auto& block : suballocator.blocks)
	{
		block.allocator.update_deferred_release(device->GetFrameCount(), device->GetBufferCount());
	}
	for (size_t i = 0; i < suballocator.blocks.size(); ++i)
	{
		if (suballocator.blocks[i].allocator.is_empty())
		{
			suballocator.blocks.erase(suballocator.blocks.begin() + i);
			break;
		}
	}
}

void ModifyObjectSampler(const SamplerDesc& desc)
{
	if (initialized.load())
	{
		device->CreateSampler(&desc, &samplers[SAMPLER_OBJECTSHADER]);
	}
}

const std::string& GetShaderPath()
{
	return SHADERPATH;
}
void SetShaderPath(const std::string& path)
{
	SHADERPATH = path;
}
const std::string& GetShaderSourcePath()
{
	return SHADERSOURCEPATH;
}
void SetShaderSourcePath(const std::string& path)
{
	SHADERSOURCEPATH = path;
}
void ReloadShaders()
{
	wi::jobsystem::Wait(raytracing_ctx);
	wi::jobsystem::Wait(objectps_ctx);
	wi::jobsystem::Wait(mesh_shader_ctx);
	wi::jobsystem::Wait(object_pso_job_ctx);

	device->ClearPipelineStateCache();
	SHADER_ERRORS.store(0);
	SHADER_MISSING.store(0);

	wi::eventhandler::FireEvent(wi::eventhandler::EVENT_RELOAD_SHADERS, 0);
}

void Initialize()
{
	wi::Timer timer;

	SetUpStates();
	LoadBuffers();

	static wi::eventhandler::Handle handle2 = wi::eventhandler::Subscribe(wi::eventhandler::EVENT_RELOAD_SHADERS, [](uint64_t userdata) { LoadShaders(); });
	LoadShaders();

	wilog("wi::renderer Initialized (%d ms)", (int)std::round(timer.elapsed()));
	initialized.store(true);
}
void ClearWorld(Scene& scene)
{
	scene.Clear();
}

// Don't store this structure on heap!
struct SHCAM
{
	XMMATRIX view_projection;
	Frustum frustum;					// This frustum can be used for intersection test with wiPrimitive primitives
	BoundingFrustum boundingfrustum;	// This boundingfrustum can be used for frustum vs frustum intersection test

	inline void init(const XMFLOAT3& eyePos, const XMFLOAT4& rotation, float nearPlane, float farPlane, float fov, XMVECTOR default_forward = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f), XMVECTOR default_up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), float aspect = 1)
	{
		const XMVECTOR E = XMLoadFloat3(&eyePos);
		const XMVECTOR Q = XMQuaternionNormalize(XMLoadFloat4(&rotation));
		const XMMATRIX rot = XMMatrixRotationQuaternion(Q);
		const XMVECTOR to = XMVector3TransformNormal(default_forward, rot);
		const XMVECTOR up = XMVector3TransformNormal(default_up, rot);
		const XMMATRIX V = XMMatrixLookToLH(E, to, up);
		const XMMATRIX P = XMMatrixPerspectiveFovLH(fov, aspect, farPlane, nearPlane);
		view_projection = XMMatrixMultiply(V, P);
		frustum.Create(view_projection);
		
		BoundingFrustum::CreateFromMatrix(boundingfrustum, P);
		std::swap(boundingfrustum.Near, boundingfrustum.Far);
		boundingfrustum.Transform(boundingfrustum, XMMatrixInverse(nullptr, V));
		XMStoreFloat4(&boundingfrustum.Orientation, XMQuaternionNormalize(XMLoadFloat4(&boundingfrustum.Orientation)));
	};
};
inline void CreateSpotLightShadowCam(const LightComponent& light, SHCAM& shcam)
{
	if (light.type == LightComponent::RECTANGLE)
	{
		XMVECTOR P = XMLoadFloat3(&light.position);
		static float backoffset = 1.0f;
		XMVECTOR P2 = P - XMLoadFloat3(&light.direction) * backoffset; // back offset hack to fixup the shadow projection limits
		float fov = XMVectorGetX(XMVector3AngleBetweenNormals(XMVector3Normalize(XMVectorSet(0, -light.height, backoffset, 0)), XMVector3Normalize(XMVectorSet(0, light.height, backoffset, 0))));
		fov = clamp(fov, XM_PIDIV4, XM_PI * 0.9f);
		float aspect = light.length / std::max(0.01f, light.height);
		XMFLOAT3 pos;
		XMStoreFloat3(&pos, P2);
		shcam.init(pos, light.rotation, backoffset, light.GetRange(), fov, XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), aspect); // rect light is pointing to -Z by default
	}
	else
	{
		shcam.init(light.position, light.rotation, 0.1f, light.GetRange(), light.outerConeAngle * 2);
	}
}
inline void CreateDirLightShadowCams(const LightComponent& light, CameraComponent camera, SHCAM* shcams, size_t shcam_count, const wi::rectpacker::Rect& shadow_rect)
{
	// remove camera jittering
	camera.jitter = XMFLOAT2(0, 0);
	camera.UpdateCamera();

	const XMMATRIX lightRotation = XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation));
	const XMVECTOR to = XMVector3TransformNormal(XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f), lightRotation);
	const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), lightRotation);
	const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), to, up); // important to not move (zero out eye vector) the light view matrix itself because texel snapping must be done on projection matrix!
	const float farPlane = camera.zFarP;

	// Unproject main frustum corners into world space (notice the reversed Z projection!):
	const XMMATRIX unproj = camera.GetInvViewProjection();
	const XMVECTOR frustum_corners[] =
	{
		XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), unproj),	// far
		XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), unproj),	// near
		XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), unproj),	// far
	};

	// Compute shadow cameras:
	for (int cascade = 0; cascade < shcam_count; ++cascade)
	{
		// Compute cascade bounds in light-view-space from the main frustum corners:
		const float split_near = cascade == 0 ? 0 : light.cascade_distances[cascade - 1] / farPlane;
		const float split_far = light.cascade_distances[cascade] / farPlane;
		const XMVECTOR corners[] =
		{
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far), lightView),
		};

		// Compute cascade bounding sphere center:
		XMVECTOR center = XMVectorZero();
		for (int j = 0; j < arraysize(corners); ++j)
		{
			center = XMVectorAdd(center, corners[j]);
		}
		center = center / float(arraysize(corners));

		// Compute cascade bounding sphere radius:
		float radius = 0;
		for (int j = 0; j < arraysize(corners); ++j)
		{
			radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[j], center))));
		}

		// Fit AABB onto bounding sphere:
		XMVECTOR vRadius = XMVectorReplicate(radius);
		XMVECTOR vMin = XMVectorSubtract(center, vRadius);
		XMVECTOR vMax = XMVectorAdd(center, vRadius);

		// Snap cascade to texel grid:
		const XMVECTOR extent = XMVectorSubtract(vMax, vMin);
		const XMVECTOR texelSize = extent / float(shadow_rect.w);
		vMin = XMVectorFloor(vMin / texelSize) * texelSize;
		vMax = XMVectorFloor(vMax / texelSize) * texelSize;
		center = (vMin + vMax) * 0.5f;

		XMFLOAT3 _center;
		XMFLOAT3 _min;
		XMFLOAT3 _max;
		XMStoreFloat3(&_center, center);
		XMStoreFloat3(&_min, vMin);
		XMStoreFloat3(&_max, vMax);

		// Extrude bounds to avoid early shadow clipping:
		float ext = abs(_center.z - _min.z);
		ext = std::max(ext, std::min(1500.0f, farPlane) * 0.5f);
		_min.z = _center.z - ext;
		_max.z = _center.z + ext;

		const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _max.z, _min.z); // notice reversed Z!

		shcams[cascade].view_projection = XMMatrixMultiply(lightView, lightProjection);
		shcams[cascade].frustum.Create(shcams[cascade].view_projection);
	}

}
inline void CreateCubemapCameras(const XMFLOAT3& position, float zNearP, float zFarP, SHCAM* shcams, size_t shcam_count)
{
	assert(shcam_count == 6);
	shcams[0].init(position, XMFLOAT4(0.5f, -0.5f, -0.5f, -0.5f), zNearP, zFarP, XM_PIDIV2); //+x
	shcams[1].init(position, XMFLOAT4(0.5f, 0.5f, 0.5f, -0.5f), zNearP, zFarP, XM_PIDIV2); //-x
	shcams[2].init(position, XMFLOAT4(1, 0, 0, -0), zNearP, zFarP, XM_PIDIV2); //+y
	shcams[3].init(position, XMFLOAT4(0, 0, 0, -1), zNearP, zFarP, XM_PIDIV2); //-y
	shcams[4].init(position, XMFLOAT4(0.707f, 0, 0, -0.707f), zNearP, zFarP, XM_PIDIV2); //+z
	shcams[5].init(position, XMFLOAT4(0, 0.707f, 0.707f, 0), zNearP, zFarP, XM_PIDIV2); //-z
}

ForwardEntityMaskCB ForwardEntityCullingCPU(const Visibility& vis, const AABB& batch_aabb, RENDERPASS renderPass)
{
	// Performs CPU light culling for a renderable batch:
	//	Similar to GPU-based tiled light culling, but this is only for simple forward passes (drawcall-granularity)

	ForwardEntityMaskCB cb;
	cb.xForwardLightMask.x = 0;
	cb.xForwardLightMask.y = 0;
	cb.xForwardDecalMask = 0;
	cb.xForwardEnvProbeMask = 0;

	uint32_t buckets[2] = { 0,0 };
	for (size_t i = 0; i < std::min(size_t(64), vis.visibleLights.size()); ++i) // only support indexing 64 lights at max for now
	{
		const uint32_t lightIndex = vis.visibleLights[i];
		const AABB& light_aabb = vis.scene->aabb_lights[lightIndex];
		if (light_aabb.intersects(batch_aabb))
		{
			const uint8_t bucket_index = uint8_t(i / 32);
			const uint8_t bucket_place = uint8_t(i % 32);
			buckets[bucket_index] |= 1 << bucket_place;
		}
	}
	cb.xForwardLightMask.x = buckets[0];
	cb.xForwardLightMask.y = buckets[1];

	for (size_t i = 0; i < std::min(size_t(32), vis.visibleDecals.size()); ++i)
	{
		const uint32_t decalIndex = vis.visibleDecals[vis.visibleDecals.size() - 1 - i]; // note: reverse order, for correct blending!
		const AABB& decal_aabb = vis.scene->aabb_decals[decalIndex];
		if (decal_aabb.intersects(batch_aabb))
		{
			const uint8_t bucket_place = uint8_t(i % 32);
			cb.xForwardDecalMask |= 1 << bucket_place;
		}
	}

	if (renderPass != RENDERPASS_ENVMAPCAPTURE)
	{
		for (size_t i = 0; i < std::min(size_t(32), vis.visibleEnvProbes.size()); ++i)
		{
			const uint32_t probeIndex = vis.visibleEnvProbes[vis.visibleEnvProbes.size() - 1 - i]; // note: reverse order, for correct blending!
			const AABB& probe_aabb = vis.scene->aabb_probes[probeIndex];
			if (probe_aabb.intersects(batch_aabb))
			{
				const uint8_t bucket_place = uint8_t(i % 32);
				cb.xForwardEnvProbeMask |= 1 << bucket_place;
			}
		}
	}

	return cb;
}

void Workaround(const int bug , CommandList cmd)
{
	if (bug == 1)
	{
		//PE: Strange DX12 bug, we must change the pso/pipeline state, just one time.
		//PE: After this there will be no "black dots" or culling/depth errors.
		//PE: This bug only happen on some nvidia cards ?
		//PE: https://github.com/turanszkij/WickedEngine/issues/450#issuecomment-1143647323

		//PE: We MUST use RENDERPASS_VOXELIZE (DSSTYPE_DEPTHDISABLED) or it will not work ?
		wi::jobsystem::Wait(object_pso_job_ctx);
		ObjectRenderingVariant variant = {};
		variant.bits.renderpass = RENDERPASS_VOXELIZE;
		variant.bits.blendmode = BLENDMODE_OPAQUE;
		variant.bits.sample_count = 1;
		const PipelineState* pso = GetObjectPSO(variant);

		device->EventBegin("Workaround 1", cmd);
		device->RenderPassBegin(nullptr, 0, cmd);
		device->BindPipelineState(pso, cmd);
		device->DrawIndexedInstanced(0, 0, 0, 0, 0, cmd); //PE: Just need predraw(cmd);
		device->RenderPassEnd(cmd);
		device->EventEnd(cmd);
	}
	return;
}

void RenderMeshes(
	const Visibility& vis,
	const RenderQueue& renderQueue,
	RENDERPASS renderPass,
	uint32_t filterMask,
	CommandList cmd,
	uint32_t flags = 0,
	uint32_t camera_count = 1
)
{
	if (renderQueue.empty())
		return;

	device->EventBegin("RenderMeshes", cmd);

	RenderPassInfo renderpass_info = device->GetRenderPassInfo(cmd);

	const bool tessellation =
		(flags & DRAWSCENE_TESSELLATION) &&
		GetTessellationEnabled() &&
		device->CheckCapability(GraphicsDeviceCapability::TESSELLATION)
		;
	const bool skip_planareflection_objects = flags & DRAWSCENE_SKIP_PLANAR_REFLECTION_OBJECTS;
	
	// Do we need to compute a light mask for this pass on the CPU?
	const bool forwardLightmaskRequest =
		renderPass == RENDERPASS_ENVMAPCAPTURE ||
		renderPass == RENDERPASS_VOXELIZE;

	const bool shadowRendering = renderPass == RENDERPASS_SHADOW;

	const bool mesh_shader = IsMeshShaderAllowed() &&
		(renderPass == RENDERPASS_PREPASS || renderPass == RENDERPASS_PREPASS_DEPTHONLY || renderPass == RENDERPASS_MAIN || renderPass == RENDERPASS_SHADOW || renderPass == RENDERPASS_RAINBLOCKER);

	// Pre-allocate space for all the instances in GPU-buffer:
	const size_t alloc_size = renderQueue.size() * camera_count * sizeof(ShaderMeshInstancePointer);
	const GraphicsDevice::GPUAllocation instances = device->AllocateGPU(alloc_size, cmd);
	const int instanceBufferDescriptorIndex = device->GetDescriptorIndex(&instances.buffer, SubresourceType::SRV);

	// This will correspond to a single draw call
	//	It's used to render multiple instances of a single mesh
	struct InstancedBatch
	{
		uint32_t meshIndex = ~0u;
		uint32_t instanceCount = 0;
		uint32_t dataOffset = 0;
		uint8_t userStencilRefOverride = 0;
		bool forceAlphatestForDithering = false;
		uint8_t lod = 0;
		AABB aabb;
	} instancedBatch = {};

	uint32_t prev_stencilref = STENCILREF_DEFAULT;
	device->BindStencilRef(prev_stencilref, cmd);

	IndexBufferFormat prev_ibformat = IndexBufferFormat::UINT16;
	const void* prev_ib_internal = nullptr;

	// This will be called every time we start a new draw call:
	auto batch_flush = [&]()
	{
		if (instancedBatch.instanceCount == 0)
			return;
		const MeshComponent& mesh = vis.scene->meshes[instancedBatch.meshIndex];
		if (!mesh.generalBuffer.IsValid())
			return;

		const bool forceAlphaTestForDithering = instancedBatch.forceAlphatestForDithering != 0;
		const uint8_t userStencilRefOverride = instancedBatch.userStencilRefOverride;

		const float tessF = mesh.GetTessellationFactor();
		const bool tessellatorRequested = tessF > 0 && tessellation;
		const bool meshShaderRequested = !tessellatorRequested && mesh_shader && mesh.vb_clu.IsValid();

		if (forwardLightmaskRequest)
		{
			ForwardEntityMaskCB cb = ForwardEntityCullingCPU(vis, instancedBatch.aabb, renderPass);
			device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(ForwardEntityMaskCB), cmd);
		}

		uint32_t first_subset = 0;
		uint32_t last_subset = 0;
		mesh.GetLODSubsetRange(instancedBatch.lod, first_subset, last_subset);
		for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
		{
			const MeshComponent::MeshSubset& subset = mesh.subsets[subsetIndex];
			if (subset.indexCount == 0 || subset.materialIndex >= vis.scene->materials.GetCount())
				continue;
			const MaterialComponent& material = vis.scene->materials[subset.materialIndex];

			if (skip_planareflection_objects && material.HasPlanarReflection())
				continue;

			bool subsetRenderable = filterMask & material.GetFilterMask();

			if (shadowRendering)
			{
				subsetRenderable = subsetRenderable && material.IsCastingShadow();
			}

			if (material.IsCoplanarBlending())
			{
				if (
					renderPass == RENDERPASS_PREPASS ||
					renderPass == RENDERPASS_SHADOW ||
					renderPass == RENDERPASS_RAINBLOCKER
					)
				{
					// coplanar blending can be skipped from these passes
					continue;
				}
				if (renderPass == RENDERPASS_MAIN)
				{
					// coplanar blending will be rendered in opaque main pass only
					subsetRenderable = filterMask & FILTER_OPAQUE;
				}
			}

			if (!subsetRenderable)
			{
				continue;
			}

			const PipelineState* pso = nullptr;
			const PipelineState* pso_backside = nullptr; // only when separate backside rendering is required (transparent doublesided)
			{
				if (IsWireRender() && renderPass != RENDERPASS_ENVMAPCAPTURE)
				{
					switch (renderPass)
					{
					case RENDERPASS_MAIN:
						if (meshShaderRequested)
						{
							pso = &PSO_object_wire_mesh_shader;
						}
						else
						{
							pso = tessellatorRequested ? &PSO_object_wire_tessellation : &PSO_object_wire;
						}
					default:
						break;
					}
				}
				else if (material.customShaderID >= 0 && material.customShaderID < (int)customShaders.size())
				{
					const CustomShader& customShader = customShaders[material.customShaderID];
					if (filterMask & customShader.filterMask)
					{
						pso = &customShader.pso[renderPass];
					}
				}
				else
				{
					ObjectRenderingVariant variant = {};
					variant.bits.renderpass = renderPass;
					variant.bits.shadertype = material.shaderType;
					variant.bits.blendmode = material.GetBlendMode();
					variant.bits.cullmode = (mesh.IsDoubleSided() || material.IsDoubleSided() || (shadowRendering && mesh.IsDoubleSidedShadow())) ? (uint32_t)CullMode::NONE : (uint32_t)CullMode::BACK;
					variant.bits.tessellation = tessellatorRequested;
					variant.bits.alphatest = material.IsAlphaTestEnabled() || forceAlphaTestForDithering;
					variant.bits.sample_count = renderpass_info.sample_count;
					variant.bits.mesh_shader = meshShaderRequested;

					pso = GetObjectPSO(variant);

					if ((filterMask & FILTER_TRANSPARENT) && variant.bits.cullmode == (uint32_t)CullMode::NONE)
					{
						variant.bits.cullmode = (uint32_t)CullMode::FRONT;
						pso_backside = GetObjectPSO(variant);
					}
				}
			}

			if (pso == nullptr || !pso->IsValid())
				continue; // This can happen if the pipeline compilation was not completed for this draw yet

			const bool meshShaderPSO = pso->desc.ms != nullptr;
			STENCILREF engineStencilRef = material.engineStencilRef;
			uint8_t userStencilRef = userStencilRefOverride > 0 ? userStencilRefOverride : material.userStencilRef;
			uint32_t stencilRef = CombineStencilrefs(engineStencilRef, userStencilRef);
			if (stencilRef != prev_stencilref)
			{
				prev_stencilref = stencilRef;
				device->BindStencilRef(stencilRef, cmd);
			}

			// Note: the mesh.generalBuffer can be either a standalone allocated buffer, or a suballocated one (to reduce index buffer switching)
			const GPUBuffer* ib = mesh.generalBufferOffsetAllocation.IsValid() ? &mesh.generalBufferOffsetAllocationAlias : &mesh.generalBuffer;
			const IndexBufferFormat ibformat = mesh.GetIndexFormat();
			const void* ibinternal = ib->internal_state.get();

			if (!meshShaderPSO && (prev_ib_internal != ibinternal || prev_ibformat != ibformat))
			{
				prev_ib_internal = ibinternal;
				prev_ibformat = ibformat;
				device->BindIndexBuffer(ib, ibformat, 0, cmd);
			}

			if (
				renderPass != RENDERPASS_PREPASS &&
				renderPass != RENDERPASS_PREPASS_DEPTHONLY &&
				renderPass != RENDERPASS_VOXELIZE
				) 
			{
				// depth only alpha test will be full res
				device->BindShadingRate(material.shadingRate, cmd);
			}

			ObjectPushConstants push;
			push.geometryIndex = mesh.geometryOffset + subsetIndex;
			push.materialIndex = subset.materialIndex;
			push.instances = instanceBufferDescriptorIndex;
			push.instance_offset = (uint)instancedBatch.dataOffset;

			uint32_t indexOffset = 0;
			if (mesh.generalBufferOffsetAllocation.IsValid())
			{
				// In case the mesh general buffer is suballocated, the indexOffset is calculated relative to the beginning of the aliased buffer block:
				indexOffset = uint32_t(((uint64_t)mesh.generalBufferOffsetAllocation.byte_offset + mesh.ib.offset) / mesh.GetIndexStride()) + subset.indexOffset;
			}
			else
			{
				// In case the mesh general buffer is not suballocated, it is a standalone buffer and index offset is relative to itself
				indexOffset = uint32_t(mesh.ib.offset / mesh.GetIndexStride()) + subset.indexOffset;
			}

			if (pso_backside != nullptr &&pso_backside->IsValid())
			{
				device->BindPipelineState(pso_backside, cmd);
				device->PushConstants(&push, sizeof(push), cmd);
				if (meshShaderPSO)
				{
					device->DispatchMesh((mesh.cluster_ranges[subsetIndex].clusterCount + 31) / 32, instancedBatch.instanceCount, 1, cmd);
				}
				else
				{
					device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, indexOffset, 0, 0, cmd);
				}
			}

			device->BindPipelineState(pso, cmd);
			device->PushConstants(&push, sizeof(push), cmd);
			if (meshShaderPSO)
			{
				device->DispatchMesh((mesh.cluster_ranges[subsetIndex].clusterCount + 31) / 32, instancedBatch.instanceCount, 1, cmd);
			}
			else
			{
				device->DrawIndexedInstanced(subset.indexCount, instancedBatch.instanceCount, indexOffset, 0, 0, cmd);
			}

		}
	};

	// The following loop is writing the instancing batches to a GPUBuffer:
	//	RenderQueue is sorted based on mesh index, so when a new mesh or stencil request is encountered, we need to flush the batch
	uint32_t instanceCount = 0;
	for (const RenderBatch& batch : renderQueue.batches) // Do not break out of this loop!
	{
		const uint32_t meshIndex = batch.GetMeshIndex();
		const uint32_t instanceIndex = batch.GetInstanceIndex();
		const ObjectComponent& instance = vis.scene->objects[instanceIndex];
		const AABB& instanceAABB = vis.scene->aabb_objects[instanceIndex];
		const uint8_t userStencilRefOverride = instance.userStencilRef;
		const uint8_t lod = batch.lod_override == 0xFF ? (uint8_t)instance.lod : batch.lod_override;

		// When we encounter a new mesh inside the global instance array, we begin a new RenderBatch:
		if (meshIndex != instancedBatch.meshIndex ||
			userStencilRefOverride != instancedBatch.userStencilRefOverride ||
			lod != instancedBatch.lod
			)
		{
			batch_flush();

			instancedBatch = {};
			instancedBatch.meshIndex = meshIndex;
			instancedBatch.instanceCount = 0;
			instancedBatch.dataOffset = (uint32_t)(instances.offset + instanceCount * sizeof(ShaderMeshInstancePointer));
			instancedBatch.userStencilRefOverride = userStencilRefOverride;
			instancedBatch.forceAlphatestForDithering = 0;
			instancedBatch.aabb = AABB();
			instancedBatch.lod = lod;
		}

		const float dither = std::max(instance.GetTransparency(), std::max(0.0f, batch.GetDistance() - instance.fadeDistance) / instance.radius);
		if (dither > 0.99f)
		{
			continue;
		}
		else if (dither > 0)
		{
			instancedBatch.forceAlphatestForDithering = 1;
		}
		if (instance.alphaRef < 1)
		{
			instancedBatch.forceAlphatestForDithering = 1;
		}

		if (forwardLightmaskRequest)
		{
			instancedBatch.aabb = AABB::Merge(instancedBatch.aabb, instanceAABB);
		}

		for (uint32_t camera_index = 0; camera_index < camera_count; ++camera_index)
		{
			const uint16_t camera_mask = 1 << camera_index;
			if ((batch.camera_mask & camera_mask) == 0)
				continue;

			ShaderMeshInstancePointer poi;
			poi.Create(instanceIndex, camera_index, dither);

			// Write into actual GPU-buffer:
			std::memcpy((ShaderMeshInstancePointer*)instances.data + instanceCount, &poi, sizeof(poi)); // memcpy whole structure into mapped pointer to avoid read from uncached memory

			instancedBatch.instanceCount++; // next instance in current InstancedBatch
			instanceCount++;
		}

	}

	batch_flush();

	device->EventEnd(cmd);
}

void RenderImpostors(
	const Visibility& vis,
	RENDERPASS renderPass, 
	CommandList cmd
)
{
	const PipelineState* pso = &PSO_impostor[renderPass];
	if (IsWireRender())
	{
		if (renderPass != RENDERPASS_PREPASS && renderPass != RENDERPASS_PREPASS_DEPTHONLY)
		{
			pso = &PSO_impostor_wire;
		}
		else
		{
			return;
		}
	}

	if (vis.scene->impostors.GetCount() > 0 && pso != nullptr && vis.scene->impostorBuffer.IsValid())
	{
		device->EventBegin("RenderImpostors", cmd);

		device->BindStencilRef(STENCILREF_DEFAULT, cmd);
		device->BindPipelineState(pso, cmd);

		device->BindIndexBuffer(
			&vis.scene->impostorBuffer,
			vis.scene->impostor_ib_format == Format::R32_UINT ? IndexBufferFormat::UINT32 : IndexBufferFormat::UINT16,
			vis.scene->impostor_ib_format == Format::R32_UINT ? vis.scene->impostor_ib32.offset : vis.scene->impostor_ib16.offset,
			cmd
		);
		device->BindResource(&vis.scene->impostorBuffer, 0, cmd, vis.scene->impostor_vb_pos.subresource_srv);
		device->BindResource(&vis.scene->impostorBuffer, 2, cmd, vis.scene->impostor_data.subresource_srv);
		device->BindResource(&vis.scene->impostorArray, 1, cmd);

		device->DrawIndexedInstancedIndirect(&vis.scene->impostorBuffer, vis.scene->impostor_indirect.offset, cmd);

		device->EventEnd(cmd);
	}
}

void ProcessDeferredTextureRequests(CommandList cmd)
{
	if(!painttextures.empty())
	{
		device->EventBegin("Paint Into Texture", cmd);

		// batch begin barriers:
		for (auto& params : painttextures)
		{
			PushBarrier(GPUBarrier::Image(&params.editTex, params.editTex.desc.layout, ResourceState::UNORDERED_ACCESS));
		}
		FlushBarriers(cmd);

		// render splats:
		device->BindComputeShader(&shaders[CSTYPE_PAINT_TEXTURE], cmd);
		for (auto& params : painttextures)
		{
			// overwrites some params!
			if (params.brushTex.IsValid())
			{
				params.push.texture_brush = device->GetDescriptorIndex(&params.brushTex, SubresourceType::SRV);
			}
			else
			{
				params.push.texture_brush = -1;
			}
			if (params.revealTex.IsValid())
			{
				params.push.texture_reveal = device->GetDescriptorIndex(&params.revealTex, SubresourceType::SRV);
			}
			else
			{
				params.push.texture_reveal = -1;
			}
			params.push.texture_output = device->GetDescriptorIndex(&params.editTex, SubresourceType::UAV);
			assert(params.push.texture_output >= 0);

			if (params.push.texture_output < 0)
				continue;

			device->PushConstants(&params.push, sizeof(params.push), cmd);

			const uint diameter = params.push.xPaintBrushRadius * 2;
			const uint dispatch_dim = (diameter + PAINT_TEXTURE_BLOCKSIZE - 1) / PAINT_TEXTURE_BLOCKSIZE;
			device->Dispatch(dispatch_dim, dispatch_dim, 1, cmd);

			device->Barrier(GPUBarrier::Memory(&params.editTex), cmd);
		}

		// ending barriers:
		for (auto& params : painttextures)
		{
			PushBarrier(GPUBarrier::Image(&params.editTex, ResourceState::UNORDERED_ACCESS, params.editTex.desc.layout));
		}
		FlushBarriers(cmd);

		// mipgen tasks after paint:
		for (auto& params : painttextures)
		{
			if (params.editTex.desc.mip_levels > 1)
			{
				GenerateMipChain(params.editTex, MIPGENFILTER::MIPGENFILTER_LINEAR, cmd);
			}
		}

		// clear all paint tasks for this frame:
		painttextures.clear();

		device->EventEnd(cmd);
	}


	deferredMIPGenLock.lock();
	for (auto& it : deferredMIPGens)
	{
		MIPGEN_OPTIONS mipopt;
		mipopt.preserve_coverage = it.second;
		GenerateMipChain(it.first, MIPGENFILTER_LINEAR, cmd, mipopt);
	}
	deferredMIPGens.clear();
	for (auto& it : deferredBCQueue)
	{
		BlockCompress(it.first, it.second, cmd);
	}
	deferredBCQueue.clear();
	deferredMIPGenLock.unlock();
}

void UpdateVisibility(Visibility& vis)
{
	// Perform parallel frustum culling and obtain closest reflector:
	wi::jobsystem::context ctx;
	auto range = wi::profiler::BeginRangeCPU("Frustum Culling");

	assert(vis.scene != nullptr); // User must provide a scene!
	assert(vis.camera != nullptr); // User must provide a camera!

	// The parallel frustum culling is first performed in shared memory, 
	//	then each group writes out it's local list to global memory
	//	The shared memory approach reduces atomics and helps the list to remain
	//	more coherent (less randomly organized compared to original order)
	static constexpr uint32_t groupSize = 63;
	static_assert(groupSize <= 256); // groupIndex must fit into uint8_t stream compaction element
	struct StreamCompaction
	{
		uint8_t list[groupSize];
		uint8_t count;
	};
	static constexpr size_t sharedmemory_size = sizeof(StreamCompaction);

	// Initialize visible indices:
	vis.Clear();

	if (!GetFreezeCullingCameraEnabled())
	{
		vis.frustum = vis.camera->frustum;
	}

	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled())
	{
		vis.flags &= ~Visibility::ALLOW_OCCLUSION_CULLING;
	}

	if (vis.flags & Visibility::ALLOW_LIGHTS)
	{
		// Cull lights:
		const uint32_t light_loop = (uint32_t)std::min(vis.scene->aabb_lights.size(), vis.scene->lights.GetCount());
		vis.visibleLights.resize(light_loop);
		vis.visibleLightShadowRects.clear();
		vis.visibleLightShadowRects.resize(light_loop);
		wi::jobsystem::Dispatch(ctx, light_loop, groupSize, [&](wi::jobsystem::JobArgs args) {

			// Setup stream compaction:
			StreamCompaction& stream_compaction = *(StreamCompaction*)args.sharedmemory;
			if (args.isFirstJobInGroup)
			{
				stream_compaction.count = 0; // first thread initializes local counter
			}

			const AABB& aabb = vis.scene->aabb_lights[args.jobIndex];

			if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
			{
				const LightComponent& light = vis.scene->lights[args.jobIndex];
				if (!light.IsInactive())
				{
					// Local stream compaction:
					stream_compaction.list[stream_compaction.count++] = args.groupIndex;
					if (light.IsVolumetricsEnabled())
					{
						vis.volumetriclight_request.store(true);
					}

					if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
					{
						if (!light.IsStatic() && light.GetType() != LightComponent::DIRECTIONAL && light.occlusionquery < 0)
						{
							if (!aabb.intersects(vis.camera->Eye))
							{
								light.occlusionquery = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
							}
						}
					}
				}
			}

			// Global stream compaction:
			if (args.isLastJobInGroup && stream_compaction.count > 0)
			{
				uint32_t prev_count = vis.light_counter.fetch_add(stream_compaction.count);
				uint32_t groupOffset = args.groupID * groupSize;
				for (uint32_t i = 0; i < stream_compaction.count; ++i)
				{
					vis.visibleLights[prev_count + i] = groupOffset + stream_compaction.list[i];
				}
			}

			}, sharedmemory_size);
	}

	if (vis.flags & Visibility::ALLOW_OBJECTS)
	{
		// Cull objects:
		const uint32_t object_loop = (uint32_t)std::min(vis.scene->aabb_objects.size(), vis.scene->objects.GetCount());
		vis.visibleObjects.resize(object_loop);
		wi::jobsystem::Dispatch(ctx, object_loop, groupSize, [&](wi::jobsystem::JobArgs args) {

			// Setup stream compaction:
			StreamCompaction& stream_compaction = *(StreamCompaction*)args.sharedmemory;
			if (args.isFirstJobInGroup)
			{
				stream_compaction.count = 0; // first thread initializes local counter
			}

			const AABB& aabb = vis.scene->aabb_objects[args.jobIndex];

			if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
			{
				// Local stream compaction:
				stream_compaction.list[stream_compaction.count++] = args.groupIndex;

				const ObjectComponent& object = vis.scene->objects[args.jobIndex];
				Scene::OcclusionResult& occlusion_result = vis.scene->occlusion_results_objects[args.jobIndex];
				bool occluded = false;
				if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
				{
					occluded = occlusion_result.IsOccluded();
				}

				if ((vis.flags & Visibility::ALLOW_REQUEST_REFLECTION) && object.IsRequestPlanarReflection() && !occluded)
				{
					// Planar reflection priority request:
					float dist = wi::math::DistanceEstimated(vis.camera->Eye, object.center);
					vis.locker.lock();
					if (dist < vis.closestRefPlane)
					{
						vis.closestRefPlane = dist;
						XMVECTOR P = XMLoadFloat3(&object.center);
						XMVECTOR N = XMVectorSet(0, 1, 0, 0);
						N = XMVector3TransformNormal(N, XMLoadFloat4x4(&vis.scene->matrix_objects[args.jobIndex]));
						N = XMVector3Normalize(N);
						XMVECTOR _refPlane = XMPlaneFromPointNormal(P, N);
						XMStoreFloat4(&vis.reflectionPlane, _refPlane);

						vis.planar_reflection_visible = true;
					}
					vis.locker.unlock();
				}

				if (object.GetFilterMask() & FILTER_TRANSPARENT)
				{
					vis.transparents_visible.store(true);
				}

				if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
				{
					if (object.IsRenderable() && occlusion_result.occlusionQueries[vis.scene->queryheap_idx] < 0)
					{
						if (aabb.intersects(vis.camera->Eye))
						{
							// camera is inside the instance, mark it as visible in this frame:
							occlusion_result.occlusionHistory |= 1;
						}
						else
						{
							occlusion_result.occlusionQueries[vis.scene->queryheap_idx] = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
						}
					}
				}
			}

			// Global stream compaction:
			if (args.isLastJobInGroup && stream_compaction.count > 0)
			{
				uint32_t prev_count = vis.object_counter.fetch_add(stream_compaction.count);
				uint32_t groupOffset = args.groupID * groupSize;
				for (uint32_t i = 0; i < stream_compaction.count; ++i)
				{
					vis.visibleObjects[prev_count + i] = groupOffset + stream_compaction.list[i];
				}
			}

			}, sharedmemory_size);
	}

	if (vis.flags & Visibility::ALLOW_DECALS)
	{
		// Note: decals must be appended in order for correct blending, must not use parallelization!
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			for (size_t i = 0; i < vis.scene->aabb_decals.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_decals[i];

				if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
				{
					vis.visibleDecals.push_back(uint32_t(i));
				}
			}
		});
	}

	if (vis.flags & Visibility::ALLOW_ENVPROBES)
	{
		// Note: probes must be appended in order for correct blending, must not use parallelization!
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			for (size_t i = 0; i < vis.scene->aabb_probes.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_probes[i];

				if ((aabb.layerMask & vis.layerMask) && vis.frustum.CheckBoxFast(aabb))
				{
					vis.visibleEnvProbes.push_back((uint32_t)i);
				}
			}
			});
	}

	if (vis.flags & Visibility::ALLOW_EMITTERS)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			// Cull emitters:
			for (size_t i = 0; i < vis.scene->emitters.GetCount(); ++i)
			{
				const wi::EmittedParticleSystem& emitter = vis.scene->emitters[i];
				if (!(emitter.layerMask & vis.layerMask))
				{
					continue;
				}
				vis.visibleEmitters.push_back((uint32_t)i);
			}
			});
	}

	if (vis.flags & Visibility::ALLOW_HAIRS)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			// Cull hairs:
			for (size_t i = 0; i < vis.scene->hairs.GetCount(); ++i)
			{
				const wi::HairParticleSystem& hair = vis.scene->hairs[i];
				if (!(hair.layerMask & vis.layerMask))
				{
					continue;
				}
				if (!hair.regenerate_frame)
				{
					const float dist = wi::math::Distance(vis.camera->Eye, hair.aabb.getCenter());
					const float radius = hair.aabb.getRadius();
					if (dist - radius > hair.viewDistance)
						continue;
				}
				if (hair.meshID == INVALID_ENTITY || !vis.frustum.CheckBoxFast(hair.aabb))
				{
					continue;
				}
				vis.visibleHairs.push_back((uint32_t)i);
			}
			});
	}

	if (vis.flags & Visibility::ALLOW_COLLIDERS)
	{
		wi::jobsystem::Execute(ctx, [&](wi::jobsystem::JobArgs args) {
			for (size_t i = 0; i < vis.scene->collider_count_gpu; ++i)
			{
				ColliderComponent collider = vis.scene->colliders_gpu[i];
				if (!(collider.layerMask & vis.layerMask))
				{
					continue;
				}
				if (!vis.frustum.CheckBoxFast(vis.scene->aabb_colliders_gpu[i]))
				{
					continue;
				}
				collider.dist = wi::math::DistanceSquared(vis.scene->aabb_colliders_gpu[i].getCenter(), vis.camera->Eye);
				vis.visibleColliders.push_back(collider);
			}
			// Ragdoll GPU colliders:
			for (size_t i = 0; i < vis.scene->humanoids.GetCount(); ++i)
			{
				uint32_t layerMask = ~0u;

				Entity entity = vis.scene->humanoids.GetEntity(i);
				const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
				if (layer != nullptr)
				{
					layerMask = layer->layerMask;
				}

				const HumanoidComponent& humanoid = vis.scene->humanoids[i];
				const bool capsule_shadow = !humanoid.IsCapsuleShadowDisabled();
				for (auto& bodypart : humanoid.ragdoll_bodyparts)
				{
					Sphere sphere = bodypart.capsule.getSphere();
					sphere.radius *= CAPSULE_SHADOW_BOLDEN;
					sphere.radius += CAPSULE_SHADOW_AFFECTION_RANGE;
					if (!vis.camera->frustum.CheckSphere(sphere.center, sphere.radius))
						continue;

					ColliderComponent collider;
					collider.layerMask = layerMask;
					collider.shape = ColliderComponent::Shape::Capsule;
					collider.capsule = bodypart.capsule;
					collider.SetCapsuleShadowEnabled(capsule_shadow);
					collider.dist = wi::math::DistanceSquared(sphere.center, vis.camera->Eye);
					vis.visibleColliders.push_back(collider);
				}
			}
			// GPU colliders sorting to camera priority:
			std::sort(vis.visibleColliders.begin(), vis.visibleColliders.end(), [](const ColliderComponent& a, const ColliderComponent& b) {
				return a.dist < b.dist;
			});
		});
	}

	wi::jobsystem::Wait(ctx);

	// finalize stream compaction:
	vis.visibleObjects.resize((size_t)vis.object_counter.load());
	vis.visibleLights.resize((size_t)vis.light_counter.load());

	if (vis.scene->weather.IsOceanEnabled())
	{
		bool occluded = false;
		if (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING)
		{
			vis.scene->ocean.occlusionQueries[vis.scene->queryheap_idx] = vis.scene->queryAllocator.fetch_add(1); // allocate new occlusion query from heap
			if (vis.scene->ocean.IsOccluded())
			{
				occluded = true;
			}
		}

		if ((vis.flags & Visibility::ALLOW_REQUEST_REFLECTION) && !occluded)
		{
			// Ocean will override any current reflectors
			vis.planar_reflection_visible = true;
			XMVECTOR _refPlane = XMPlaneFromPointNormal(XMVectorSet(0, vis.scene->weather.oceanParameters.waterHeight, 0, 0), XMVectorSet(0, 1, 0, 0));
			XMStoreFloat4(&vis.reflectionPlane, _refPlane);
		}
	}

	// Shadow atlas packing:
	if ((IsShadowsEnabled() && (vis.flags & Visibility::ALLOW_SHADOW_ATLAS_PACKING) && !vis.visibleLights.empty()) || vis.scene->weather.rain_amount > 0)
	{
		auto range = wi::profiler::BeginRangeCPU("Shadowmap packing");
		float iterative_scaling = 1;

		while (iterative_scaling > 0.03f)
		{
			vis.shadow_packer.clear();
			if (vis.scene->weather.rain_amount > 0)
			{
				// Rain blocker:
				wi::rectpacker::Rect rect = {};
				rect.id = -1;
				rect.w = rect.h = 128;
				vis.shadow_packer.add_rect(rect);
			}
			for (uint32_t lightIndex : vis.visibleLights)
			{
				const LightComponent& light = vis.scene->lights[lightIndex];
				if (light.IsInactive())
					continue;
				if (!light.IsCastingShadow() || light.IsStatic())
					continue;

				const float dist = wi::math::Distance(vis.camera->Eye, light.position);
				const float range = light.GetRange();
				const float amount = std::min(1.0f, range / std::max(0.001f, dist)) * iterative_scaling;

				wi::rectpacker::Rect rect = {};
				rect.id = int(lightIndex);
				switch (light.GetType())
				{
				case LightComponent::DIRECTIONAL:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = light.forced_shadow_resolution * int(light.cascade_distances.size());
						rect.h = light.forced_shadow_resolution;
					}
					else
					{
						rect.w = int(max_shadow_resolution_2D * iterative_scaling) * int(light.cascade_distances.size());
						rect.h = int(max_shadow_resolution_2D * iterative_scaling);
					}
					break;
				case LightComponent::SPOT:
				case LightComponent::RECTANGLE:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = int(light.forced_shadow_resolution);
						rect.h = int(light.forced_shadow_resolution);
					}
					else
					{
						rect.w = int(max_shadow_resolution_2D * amount);
						rect.h = int(max_shadow_resolution_2D * amount);
					}
					break;
				case LightComponent::POINT:
					if (light.forced_shadow_resolution >= 0)
					{
						rect.w = int(light.forced_shadow_resolution) * 6;
						rect.h = int(light.forced_shadow_resolution);
					}
					else
					{
						rect.w = int(max_shadow_resolution_cube * amount) * 6;
						rect.h = int(max_shadow_resolution_cube * amount);
					}
					break;
				default:
					break;
				}
				if (rect.w > 8 && rect.h > 8)
				{
					vis.shadow_packer.add_rect(rect);
				}
			}
			if (!vis.shadow_packer.rects.empty())
			{
				if (vis.shadow_packer.pack(8192))
				{
					for (auto& rect : vis.shadow_packer.rects)
					{
						if (rect.id == -1)
						{
							// Rain blocker:
							if (rect.was_packed)
							{
								vis.rain_blocker_shadow_rect = rect;
							}
							else
							{
								vis.rain_blocker_shadow_rect = {};
							}
							continue;
						}
						uint32_t lightIndex = uint32_t(rect.id);
						wi::rectpacker::Rect& lightrect = vis.visibleLightShadowRects[lightIndex];
						const LightComponent& light = vis.scene->lights[lightIndex];
						if (rect.was_packed)
						{
							lightrect = rect;

							// Remove slice multipliers from rect:
							switch (light.GetType())
							{
							case LightComponent::DIRECTIONAL:
								lightrect.w /= int(light.cascade_distances.size());
								break;
							case LightComponent::POINT:
								lightrect.w /= 6;
								break;
							default:
								break;
							}
						}
					}

					if ((int)shadowMapAtlas.desc.width < vis.shadow_packer.width || (int)shadowMapAtlas.desc.height < vis.shadow_packer.height)
					{
						TextureDesc desc;
						desc.width = uint32_t(vis.shadow_packer.width);
						desc.height = uint32_t(vis.shadow_packer.height);
						desc.format = format_depthbuffer_shadowmap;
						desc.bind_flags = BindFlag::DEPTH_STENCIL | BindFlag::SHADER_RESOURCE;
						desc.layout = ResourceState::SHADER_RESOURCE;
						desc.misc_flags = ResourceMiscFlag::TEXTURE_COMPATIBLE_COMPRESSION;
						device->CreateTexture(&desc, nullptr, &shadowMapAtlas);
						device->SetName(&shadowMapAtlas, "shadowMapAtlas");

						desc.format = format_rendertarget_shadowmap;
						desc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE;
						desc.layout = ResourceState::SHADER_RESOURCE;
						desc.clear.color[0] = 1;
						desc.clear.color[1] = 1;
						desc.clear.color[2] = 1;
						desc.clear.color[3] = 0;
						device->CreateTexture(&desc, nullptr, &shadowMapAtlas_Transparent);
						device->SetName(&shadowMapAtlas_Transparent, "shadowMapAtlas_Transparent");

					}

					break;
				}
				else
				{
					iterative_scaling *= 0.5f;
				}
			}
			else
			{
				iterative_scaling = 0.0; //PE: fix - endless loop if some lights do not have shadows.
			}
		}
		wi::profiler::EndRange(range);
	}

	wi::profiler::EndRange(range); // Frustum Culling
}
void UpdatePerFrameData(
	Scene& scene,
	const Visibility& vis,
	FrameCB& frameCB,
	float dt
)
{
	// Calculate volumetric cloud shadow data:
	if (vis.scene->weather.IsVolumetricClouds() && vis.scene->weather.IsVolumetricCloudsCastShadow())
	{
		if (!textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].IsValid())
		{
			TextureDesc desc;
			desc.type = TextureDesc::Type::TEXTURE_2D;
			desc.width = 512;
			desc.height = 512;
			desc.format = Format::R11G11B10_FLOAT;
			desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
			desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
			device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW]);
			device->SetName(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW], "textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW]");
			device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW_GAUSSIAN_TEMP]);
			device->SetName(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW_GAUSSIAN_TEMP], "textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW_GAUSSIAN_TEMP]");
		}

		const float cloudShadowSnapLength = 5000.0f;
		const float cloudShadowExtent = 10000.0f; // The cloud shadow bounding box size
		const float cloudShadowNearPlane = 0.0f;
		const float cloudShadowFarPlane = cloudShadowExtent * 2.0;

		const float metersToSkyUnit = 0.001f; // Engine units are in meters (must be same as globals.hlsli)
		const float skyUnitToMeters = 1.0f / metersToSkyUnit;

		XMVECTOR atmosphereCenter = XMLoadFloat3(&vis.scene->weather.atmosphereParameters.planetCenter);
		XMVECTOR sunDirection = XMLoadFloat3(&vis.scene->weather.sunDirection);
		const float planetRadius = vis.scene->weather.atmosphereParameters.bottomRadius;

		// Point on the surface of the planet relative to camera position and planet normal
		XMVECTOR lookAtPosition = XMVector3Normalize(vis.camera->GetEye() - (atmosphereCenter * skyUnitToMeters));
		lookAtPosition = (atmosphereCenter + lookAtPosition * planetRadius) * skyUnitToMeters;

		// Snap with user defined value
		lookAtPosition = XMVectorFloor(XMVectorAdd(lookAtPosition, XMVectorReplicate(0.5f * cloudShadowSnapLength)) / cloudShadowSnapLength) * cloudShadowSnapLength;

		XMVECTOR lightPosition = lookAtPosition + sunDirection * cloudShadowExtent; // far plane not needed here

		const XMMATRIX lightRotation = XMMatrixRotationQuaternion(XMLoadFloat4(&scene.weather.stars_rotation_quaternion)); // We only care about prioritized directional light anyway
		const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), lightRotation);

		XMMATRIX cloudShadowProjection = XMMatrixOrthographicOffCenterLH(-cloudShadowExtent, cloudShadowExtent, -cloudShadowExtent, cloudShadowExtent, cloudShadowNearPlane, cloudShadowFarPlane);
		XMMATRIX cloudShadowView = XMMatrixLookAtLH(lightPosition, lookAtPosition, up);

		XMMATRIX cloudShadowLightSpaceMatrix = XMMatrixMultiply(cloudShadowView, cloudShadowProjection);
		XMMATRIX cloudShadowLightSpaceMatrixInverse = XMMatrixInverse(nullptr, cloudShadowLightSpaceMatrix);

		XMStoreFloat4x4(&frameCB.cloudShadowLightSpaceMatrix, cloudShadowLightSpaceMatrix);
		XMStoreFloat4x4(&frameCB.cloudShadowLightSpaceMatrixInverse, cloudShadowLightSpaceMatrixInverse);
		frameCB.cloudShadowFarPlaneKm = cloudShadowFarPlane * metersToSkyUnit;
		frameCB.texture_volumetricclouds_shadow_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW], SubresourceType::SRV);
	}

	if (scene.weather.IsRealisticSky() && !textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].IsValid())
	{
		TextureDesc desc;
		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 256;
		desc.height = 64;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT]");

		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 32;
		desc.height = 32;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT]");

		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 192;
		desc.height = 104;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT]");

		desc.type = TextureDesc::Type::TEXTURE_2D;
		desc.width = 1;
		desc.height = 1;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT]);
		device->SetName(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], "textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT]");

		desc.type = TextureDesc::Type::TEXTURE_3D;
		desc.width = 32;
		desc.height = 32;
		desc.depth = 32;
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT]);
		device->SetName(&textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT], "textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT]");
	}

	// Update CPU-side frame constant buffer:
	frameCB.delta_time = dt * GetGameSpeed();
	frameCB.time_previous = frameCB.time;
	frameCB.time += frameCB.delta_time;
	frameCB.frame_count = (uint)device->GetFrameCount();
	frameCB.blue_noise_phase = (frameCB.frame_count & 0xFF) * 1.6180339887f;

	frameCB.vxgi.max_distance = scene.vxgi.maxDistance;
	frameCB.vxgi.resolution = GetVXGIEnabled() ? (uint)scene.vxgi.res : 0;
	frameCB.vxgi.resolution_rcp = GetVXGIEnabled() ? 1.0f / (float)frameCB.vxgi.resolution : 0; //PE: was inf.
	frameCB.vxgi.stepsize = scene.vxgi.rayStepSize;
	frameCB.vxgi.texture_radiance = device->GetDescriptorIndex(&scene.vxgi.radiance, SubresourceType::SRV);
	frameCB.vxgi.texture_sdf = device->GetDescriptorIndex(&scene.vxgi.sdf, SubresourceType::SRV);
	for (uint i = 0; i < VXGI_CLIPMAP_COUNT; ++i)
	{
		frameCB.vxgi.clipmaps[i].center = scene.vxgi.clipmaps[i].center;
		frameCB.vxgi.clipmaps[i].voxelSize = scene.vxgi.clipmaps[i].voxelsize;
	}

	frameCB.giboost_packed = wi::math::pack_half2(GetGIBoost(), 0);

	if (scene.weather.rain_amount > 0)
	{
		frameCB.rain_blocker_mad_prev = frameCB.rain_blocker_mad;
		frameCB.rain_blocker_matrix_prev = frameCB.rain_blocker_matrix;
		frameCB.rain_blocker_matrix_inverse_prev = frameCB.rain_blocker_matrix_inverse;
		frameCB.rain_blocker_mad = XMFLOAT4(
			float(vis.rain_blocker_shadow_rect.w) / float(shadowMapAtlas.desc.width),
			float(vis.rain_blocker_shadow_rect.h) / float(shadowMapAtlas.desc.height),
			float(vis.rain_blocker_shadow_rect.x) / float(shadowMapAtlas.desc.width),
			float(vis.rain_blocker_shadow_rect.y) / float(shadowMapAtlas.desc.height)
		);
		SHCAM shcam;
		CreateDirLightShadowCams(vis.scene->rain_blocker_dummy_light, *vis.camera, &shcam, 1, vis.rain_blocker_shadow_rect);
		XMStoreFloat4x4(&frameCB.rain_blocker_matrix, shcam.view_projection);
		XMStoreFloat4x4(&frameCB.rain_blocker_matrix_inverse, XMMatrixInverse(nullptr, shcam.view_projection));
	}

	frameCB.temporalaa_samplerotation = 0;
	if (GetTemporalAAEnabled())
	{
		uint x = frameCB.frame_count % 4;
		uint y = frameCB.frame_count / 4;
		frameCB.temporalaa_samplerotation = (x & 0x000000FF) | ((y & 0x000000FF) << 8);
	}

	frameCB.capsuleshadow_fade_angle = uint32_t(XMConvertFloatToHalf(CAPSULE_SHADOW_FADE)) | uint32_t(XMConvertFloatToHalf(std::max(0.001f, CAPSULE_SHADOW_ANGLE * 0.5f))) << 16u;

	frameCB.options = 0;
	if (GetTemporalAAEnabled())
	{
		frameCB.options |= OPTION_BIT_TEMPORALAA_ENABLED;
	}
	if (GetVXGIEnabled())
	{
		frameCB.options |= OPTION_BIT_VXGI_ENABLED;
	}
	if (GetVXGIReflectionsEnabled())
	{
		frameCB.options |= OPTION_BIT_VXGI_REFLECTIONS_ENABLED;
	}
	if (vis.scene->weather.IsRealisticSky())
	{
		frameCB.options |= OPTION_BIT_REALISTIC_SKY;
	}
	if (vis.scene->weather.IsOverrideFogColor())
	{
		frameCB.options |= OPTION_BIT_OVERRIDE_FOG_COLOR;
	}
	if (vis.scene->weather.IsHeightFog())
	{
		frameCB.options |= OPTION_BIT_HEIGHT_FOG;
	}
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING) && GetRaytracedShadowsEnabled())
	{
		frameCB.options |= OPTION_BIT_RAYTRACED_SHADOWS;
		frameCB.options |= OPTION_BIT_SHADOW_MASK;
	}
	if (GetScreenSpaceShadowsEnabled())
	{
		frameCB.options |= OPTION_BIT_SHADOW_MASK;
	}
	if (GetSurfelGIEnabled())
	{
		frameCB.options |= OPTION_BIT_SURFELGI_ENABLED;
	}
	if (IsDisableAlbedoMaps())
	{
		frameCB.options |= OPTION_BIT_DISABLE_ALBEDO_MAPS;
	}
	if (IsForceDiffuseLighting())
	{
		frameCB.options |= OPTION_BIT_FORCE_DIFFUSE_LIGHTING;
	}
	if (vis.scene->weather.IsVolumetricCloudsCastShadow() && vis.scene->weather.IsVolumetricClouds())
	{
		frameCB.options |= OPTION_BIT_VOLUMETRICCLOUDS_CAST_SHADOW;
	}
	if (vis.scene->weather.skyMap.IsValid() && !has_flag(vis.scene->weather.skyMap.GetTexture().desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
	{
		frameCB.options |= OPTION_BIT_STATIC_SKY_SPHEREMAP;
	}
	if (vis.scene->weather.IsRealisticSkyAerialPerspective() && vis.scene->weather.IsRealisticSky())
	{
		frameCB.options |= OPTION_BIT_REALISTIC_SKY_AERIAL_PERSPECTIVE;
	}
	if (vis.scene->weather.IsRealisticSkyHighQuality() && vis.scene->weather.IsRealisticSky())
	{
		frameCB.options |= OPTION_BIT_REALISTIC_SKY_HIGH_QUALITY;
	}
	if (vis.scene->weather.IsRealisticSkyReceiveShadow() && vis.scene->weather.IsRealisticSky())
	{
		frameCB.options |= OPTION_BIT_REALISTIC_SKY_RECEIVE_SHADOW;
	}
	if (vis.scene->weather.IsVolumetricCloudsReceiveShadow() && vis.scene->weather.IsVolumetricClouds())
	{
		frameCB.options |= OPTION_BIT_VOLUMETRICCLOUDS_RECEIVE_SHADOW;
	}
	if (IsCapsuleShadowEnabled())
	{
		frameCB.options |= OPTION_BIT_CAPSULE_SHADOW_ENABLED;
	}

	frameCB.scene = vis.scene->shaderscene;

	frameCB.texture_random64x64_index = device->GetDescriptorIndex(wi::texturehelper::getRandom64x64(), SubresourceType::SRV);
	frameCB.texture_bluenoise_index = device->GetDescriptorIndex(wi::texturehelper::getBlueNoise(), SubresourceType::SRV);
	frameCB.texture_sheenlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SHEENLUT], SubresourceType::SRV);
	frameCB.texture_skyviewlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], SubresourceType::SRV);
	frameCB.texture_transmittancelut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], SubresourceType::SRV);
	frameCB.texture_multiscatteringlut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], SubresourceType::SRV);
	frameCB.texture_skyluminancelut_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], SubresourceType::SRV);
	frameCB.texture_cameravolumelut_index = device->GetDescriptorIndex(&textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT], SubresourceType::SRV);
	frameCB.texture_wind_index = device->GetDescriptorIndex(&textures[TEXTYPE_3D_WIND], SubresourceType::SRV);
	frameCB.texture_wind_prev_index = device->GetDescriptorIndex(&textures[TEXTYPE_3D_WIND_PREV], SubresourceType::SRV);
	frameCB.texture_caustics_index = device->GetDescriptorIndex(&textures[TEXTYPE_2D_CAUSTICS], SubresourceType::SRV);

	// See if indirect debug buffer needs to be resized:
	if (indirectDebugStatsReadback_available[device->GetBufferIndex()] && indirectDebugStatsReadback[device->GetBufferIndex()].mapped_data != nullptr)
	{
		const IndirectDrawArgsInstanced* indirectDebugStats = (const IndirectDrawArgsInstanced*)indirectDebugStatsReadback[device->GetBufferIndex()].mapped_data;
		const uint64_t required_debug_buffer_size = sizeof(IndirectDrawArgsInstanced) + (sizeof(XMFLOAT4) + sizeof(XMFLOAT4)) * clamp(indirectDebugStats->VertexCountPerInstance, 0u, 4000000u);
		if (buffers[BUFFERTYPE_INDIRECT_DEBUG_0].desc.size < required_debug_buffer_size)
		{
			GPUBufferDesc bd;
			bd.size = required_debug_buffer_size;
			bd.bind_flags = BindFlag::VERTEX_BUFFER | BindFlag::UNORDERED_ACCESS;
			bd.misc_flags = ResourceMiscFlag::BUFFER_RAW | ResourceMiscFlag::INDIRECT_ARGS;
			device->CreateBufferZeroed(&bd, &buffers[BUFFERTYPE_INDIRECT_DEBUG_0]);
			device->SetName(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], "buffers[BUFFERTYPE_INDIRECT_DEBUG_0]");
			device->CreateBufferZeroed(&bd, &buffers[BUFFERTYPE_INDIRECT_DEBUG_1]);
			device->SetName(&buffers[BUFFERTYPE_INDIRECT_DEBUG_1], "buffers[BUFFERTYPE_INDIRECT_DEBUG_1]");
			std::memset(indirectDebugStatsReadback_available, 0, sizeof(indirectDebugStatsReadback_available));
		}
	}

	// Indirect debug buffers: 0 is always WRITE, 1 is always READ
	std::swap(buffers[BUFFERTYPE_INDIRECT_DEBUG_0], buffers[BUFFERTYPE_INDIRECT_DEBUG_1]);
	frameCB.indirect_debugbufferindex = device->GetDescriptorIndex(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], SubresourceType::UAV);

	// Note: shadow maps always assumed to be valid to avoid shader branching logic
	const Texture& shadowMap = shadowMapAtlas.IsValid() ? shadowMapAtlas : *wi::texturehelper::getBlack();
	const Texture& shadowMapTransparent = shadowMapAtlas_Transparent.IsValid() ? shadowMapAtlas_Transparent : *wi::texturehelper::getWhite();
	frameCB.texture_shadowatlas_index = device->GetDescriptorIndex(&shadowMap, SubresourceType::SRV);
	frameCB.texture_shadowatlas_transparent_index = device->GetDescriptorIndex(&shadowMapTransparent, SubresourceType::SRV);
	frameCB.shadow_atlas_resolution.x = shadowMap.desc.width;
	frameCB.shadow_atlas_resolution.y = shadowMap.desc.height;
	frameCB.shadow_atlas_resolution_rcp.x = 1.0f / frameCB.shadow_atlas_resolution.x;
	frameCB.shadow_atlas_resolution_rcp.y = 1.0f / frameCB.shadow_atlas_resolution.y;

	// Create volumetric cloud static resources if needed:
	if (scene.weather.IsVolumetricClouds() && !texture_shapeNoise.IsValid())
	{
		TextureDesc shape_desc;
		shape_desc.type = TextureDesc::Type::TEXTURE_3D;
		shape_desc.width = 64;
		shape_desc.height = 64;
		shape_desc.depth = 64;
		shape_desc.mip_levels = 6;
		shape_desc.format = Format::R8G8B8A8_UNORM;
		shape_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		shape_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&shape_desc, nullptr, &texture_shapeNoise);
		device->SetName(&texture_shapeNoise, "texture_shapeNoise");

		for (uint32_t i = 0; i < texture_shapeNoise.GetDesc().mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&texture_shapeNoise, SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&texture_shapeNoise, SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}

		TextureDesc detail_desc;
		detail_desc.type = TextureDesc::Type::TEXTURE_3D;
		detail_desc.width = 32;
		detail_desc.height = 32;
		detail_desc.depth = 32;
		detail_desc.mip_levels = 6;
		detail_desc.format = Format::R8G8B8A8_UNORM;
		detail_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		detail_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&detail_desc, nullptr, &texture_detailNoise);
		device->SetName(&texture_detailNoise, "texture_detailNoise");

		for (uint32_t i = 0; i < texture_detailNoise.GetDesc().mip_levels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&texture_detailNoise, SubresourceType::SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&texture_detailNoise, SubresourceType::UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}

		TextureDesc texture_desc;
		texture_desc.type = TextureDesc::Type::TEXTURE_2D;
		texture_desc.width = 128;
		texture_desc.height = 128;
		texture_desc.format = Format::R8G8B8A8_UNORM;
		texture_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		texture_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&texture_desc, nullptr, &texture_curlNoise);
		device->SetName(&texture_curlNoise, "texture_curlNoise");

		texture_desc.width = 1024;
		texture_desc.height = 1024;
		texture_desc.format = Format::R8G8B8A8_UNORM;
		device->CreateTexture(&texture_desc, nullptr, &texture_weatherMap);
		device->SetName(&texture_weatherMap, "texture_weatherMap");

	}

	// Fill Entity Array with decals + envprobes + lights in the frustum:
	uint envprobearray_offset = 0;
	uint envprobearray_count = 0;
	uint lightarray_offset_directional = 0;
	uint lightarray_count_directional = 0;
	uint lightarray_offset_spot = 0;
	uint lightarray_count_spot = 0;
	uint lightarray_offset_point = 0;
	uint lightarray_count_point = 0;
	uint lightarray_offset_rect = 0;
	uint lightarray_count_rect = 0;
	uint lightarray_offset = 0;
	uint lightarray_count = 0;
	uint decalarray_offset = 0;
	uint decalarray_count = 0;
	uint forcefieldarray_offset = 0;
	uint forcefieldarray_count = 0;
	frameCB.entity_culling_count = 0;
	{
		ShaderEntity* entityArray = frameCB.entityArray;
		float4x4* matrixArray = frameCB.matrixArray;

		uint32_t entityCounter = 0;
		uint32_t matrixCounter = 0;

		// Write decals into entity array:
		decalarray_offset = entityCounter;
		const size_t decal_iterations = std::min((size_t)MAX_SHADER_DECAL_COUNT, vis.visibleDecals.size());
		for (size_t i = 0; i < decal_iterations; ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				matrixCounter--;
				break;
			}
			ShaderEntity shaderentity = {};
			XMMATRIX shadermatrix;

			const uint32_t decalIndex = vis.visibleDecals[vis.visibleDecals.size() - 1 - i]; // note: reverse order, for correct blending!
			const DecalComponent& decal = vis.scene->decals[decalIndex];

			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->decals.GetEntity(decalIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_DECAL);
			if (decal.IsBaseColorOnlyAlpha())
			{
				shaderentity.SetFlags(ENTITY_FLAG_DECAL_BASECOLOR_ONLY_ALPHA);
			}
			shaderentity.position = decal.position;
			shaderentity.SetRange(decal.range);
			float emissive_mul = 1 + decal.emissive;
			shaderentity.SetColor(float4(decal.color.x * emissive_mul, decal.color.y * emissive_mul, decal.color.z * emissive_mul, decal.color.w));
			shaderentity.shadowAtlasMulAdd = decal.texMulAdd;
			shaderentity.SetConeAngleCos(decal.slopeBlendPower);
			shaderentity.SetDirection(decal.front);
			shaderentity.SetAngleScale(decal.normal_strength);
			shaderentity.SetLength(decal.displacement_strength);

			shaderentity.SetIndices(matrixCounter, 0);
			shadermatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&decal.world));

			int texture = -1;
			if (decal.texture.IsValid())
			{
				texture = device->GetDescriptorIndex(&decal.texture.GetTexture(), SubresourceType::SRV, decal.texture.GetTextureSRGBSubresource());
			}
			int normal = -1;
			if (decal.normal.IsValid())
			{
				normal = device->GetDescriptorIndex(&decal.normal.GetTexture(), SubresourceType::SRV);
			}
			int surfacemap = -1;
			if (decal.surfacemap.IsValid())
			{
				surfacemap = device->GetDescriptorIndex(&decal.surfacemap.GetTexture(), SubresourceType::SRV);
			}
			int displacementmap = -1;
			if (decal.displacementmap.IsValid())
			{
				displacementmap = device->GetDescriptorIndex(&decal.displacementmap.GetTexture(), SubresourceType::SRV);
			}

			shadermatrix.r[0] = XMVectorSetW(shadermatrix.r[0], *(float*)&texture);
			shadermatrix.r[1] = XMVectorSetW(shadermatrix.r[1], *(float*)&normal);
			shadermatrix.r[2] = XMVectorSetW(shadermatrix.r[2], *(float*)&surfacemap);
			shadermatrix.r[3] = XMVectorSetW(shadermatrix.r[3], *(float*)&displacementmap);

			XMStoreFloat4x4(matrixArray + matrixCounter, shadermatrix);
			matrixCounter++;

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			decalarray_count++;
		}

		// Write environment probes into entity array:
		envprobearray_offset = entityCounter;
		const size_t probe_iterations = std::min((size_t)MAX_SHADER_PROBE_COUNT, vis.visibleEnvProbes.size());
		for (size_t i = 0; i < probe_iterations; ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}
			if (matrixCounter >= MATRIXARRAY_COUNT)
			{
				matrixCounter--;
				break;
			}
			ShaderEntity shaderentity = {};
			XMMATRIX shadermatrix;

			const uint32_t probeIndex = vis.visibleEnvProbes[vis.visibleEnvProbes.size() - 1 - i]; // note: reverse order, for correct blending!
			const EnvironmentProbeComponent& probe = vis.scene->probes[probeIndex];

			shaderentity = {}; // zero out!
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->probes.GetEntity(probeIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_ENVMAP);
			shaderentity.position = probe.position;
			shaderentity.SetRange(probe.range);

			shaderentity.SetIndices(matrixCounter, 0);
			shadermatrix = XMLoadFloat4x4(&probe.inverseMatrix);

			int texture = -1;
			if (probe.texture.IsValid())
			{
				texture = device->GetDescriptorIndex(&probe.texture, SubresourceType::SRV);
			}

			shadermatrix.r[0] = XMVectorSetW(shadermatrix.r[0], *(float*)&texture);
			shadermatrix.r[1] = XMVectorSetW(shadermatrix.r[1], 0);
			shadermatrix.r[2] = XMVectorSetW(shadermatrix.r[2], 0);
			shadermatrix.r[3] = XMVectorSetW(shadermatrix.r[3], 0);

			XMStoreFloat4x4(matrixArray + matrixCounter, shadermatrix);
			matrixCounter++;

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			envprobearray_count++;
		}

		const XMFLOAT2 atlas_dim_rcp = XMFLOAT2(1.0f / float(shadowMapAtlas.desc.width), 1.0f / float(shadowMapAtlas.desc.height));

		// Write directional lights into entity array:
		lightarray_offset = entityCounter;
		lightarray_offset_directional = entityCounter;
		for (uint32_t lightIndex : vis.visibleLights)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}

			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.GetType() != LightComponent::DIRECTIONAL || light.IsInactive())
				continue;

			ShaderEntity shaderentity = {};
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->lights.GetEntity(lightIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_DIRECTIONALLIGHT);
			shaderentity.position = light.position;
			shaderentity.SetRange(light.GetRange());
			shaderentity.SetRadius(light.radius);
			shaderentity.SetLength(light.length);
			shaderentity.SetDirection(light.direction);
			shaderentity.SetColor(float4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity, 1));

			const bool shadowmap = IsShadowsEnabled() && light.IsCastingShadow() && !light.IsStatic();
			const wi::rectpacker::Rect& shadow_rect = vis.visibleLightShadowRects[lightIndex];

			if (shadowmap)
			{
				shaderentity.shadowAtlasMulAdd.x = shadow_rect.w * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.y = shadow_rect.h * atlas_dim_rcp.y;
				shaderentity.shadowAtlasMulAdd.z = shadow_rect.x * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.w = shadow_rect.y * atlas_dim_rcp.y;
			}

			shaderentity.SetIndices(matrixCounter, 0);

			const uint cascade_count = std::min((uint)light.cascade_distances.size(), MATRIXARRAY_COUNT - matrixCounter);
			shaderentity.SetShadowCascadeCount(cascade_count);

			if (shadowmap && !light.cascade_distances.empty())
			{
				SHCAM* shcams = (SHCAM*)alloca(sizeof(SHCAM) * cascade_count);
				CreateDirLightShadowCams(light, *vis.camera, shcams, cascade_count, shadow_rect);
				for (size_t cascade = 0; cascade < cascade_count; ++cascade)
				{
					XMStoreFloat4x4(&matrixArray[matrixCounter++], shcams[cascade].view_projection);
				}
			}

			if (light.IsCastingShadow())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_CASTING_SHADOW);
			}
			if (light.IsStatic())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_STATIC);
			}

			if (light.IsVolumetricCloudsEnabled())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_VOLUMETRICCLOUDS);
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			lightarray_count_directional++;
		}

		// Write spot lights into entity array:
		lightarray_offset_spot = entityCounter;
		for (uint32_t lightIndex : vis.visibleLights)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}

			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.GetType() != LightComponent::SPOT || light.IsInactive())
				continue;

			ShaderEntity shaderentity = {};
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->lights.GetEntity(lightIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_SPOTLIGHT);
			shaderentity.position = light.position;
			shaderentity.SetRange(light.GetRange());
			shaderentity.SetRadius(light.radius);
			shaderentity.SetLength(light.length);
			shaderentity.SetDirection(light.direction);
			shaderentity.SetColor(float4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity, 1));

			const bool shadowmap = IsShadowsEnabled() && light.IsCastingShadow() && !light.IsStatic();
			const wi::rectpacker::Rect& shadow_rect = vis.visibleLightShadowRects[lightIndex];

			const uint maskTex = light.maskTexDescriptor < 0 ? 0 : light.maskTexDescriptor;

			if (shadowmap)
			{
				shaderentity.shadowAtlasMulAdd.x = shadow_rect.w * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.y = shadow_rect.h * atlas_dim_rcp.y;
				shaderentity.shadowAtlasMulAdd.z = shadow_rect.x * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.w = shadow_rect.y * atlas_dim_rcp.y;
			}

			shaderentity.SetIndices(matrixCounter, maskTex);

			const float outerConeAngle = light.outerConeAngle;
			const float innerConeAngle = std::min(light.innerConeAngle, outerConeAngle);
			const float outerConeAngleCos = std::cos(outerConeAngle);
			const float innerConeAngleCos = std::cos(innerConeAngle);

			// https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_lights_punctual#inner-and-outer-cone-angles
			const float lightAngleScale = 1.0f / std::max(0.001f, innerConeAngleCos - outerConeAngleCos);
			const float lightAngleOffset = -outerConeAngleCos * lightAngleScale;

			shaderentity.SetConeAngleCos(outerConeAngleCos);
			shaderentity.SetAngleScale(lightAngleScale);
			shaderentity.SetAngleOffset(lightAngleOffset);

			if (shadowmap || (maskTex > 0))
			{
				SHCAM shcam;
				CreateSpotLightShadowCam(light, shcam);
				XMStoreFloat4x4(&matrixArray[matrixCounter++], shcam.view_projection);
			}

			if (light.IsCastingShadow())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_CASTING_SHADOW);
			}
			if (light.IsStatic())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_STATIC);
			}

			if (light.IsVolumetricCloudsEnabled())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_VOLUMETRICCLOUDS);
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			lightarray_count_spot++;
		}

		// Write point lights into entity array:
		lightarray_offset_point = entityCounter;
		for (uint32_t lightIndex : vis.visibleLights)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}

			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.GetType() != LightComponent::POINT || light.IsInactive())
				continue;

			ShaderEntity shaderentity = {};
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->lights.GetEntity(lightIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_POINTLIGHT);
			shaderentity.position = light.position;
			shaderentity.SetRange(light.GetRange());
			shaderentity.SetRadius(light.radius);
			shaderentity.SetLength(light.length);
			shaderentity.SetDirection(light.direction);
			shaderentity.SetColor(float4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity, 1));

			const bool shadowmap = IsShadowsEnabled() && light.IsCastingShadow() && !light.IsStatic();
			const wi::rectpacker::Rect& shadow_rect = vis.visibleLightShadowRects[lightIndex];

			const uint maskTex = light.maskTexDescriptor < 0 ? 0 : light.maskTexDescriptor;

			if (shadowmap)
			{
				shaderentity.shadowAtlasMulAdd.x = shadow_rect.w * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.y = shadow_rect.h * atlas_dim_rcp.y;
				shaderentity.shadowAtlasMulAdd.z = shadow_rect.x * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.w = shadow_rect.y * atlas_dim_rcp.y;
			}

			shaderentity.SetIndices(matrixCounter, maskTex);

			if (shadowmap)
			{
				const float FarZ = 0.1f;	// watch out: reversed depth buffer! Also, light near plane is constant for simplicity, this should match on cpu side!
				const float NearZ = std::max(1.0f, light.GetRange()); // watch out: reversed depth buffer!
				const float fRange = FarZ / (FarZ - NearZ);
				const float cubemapDepthRemapNear = fRange;
				const float cubemapDepthRemapFar = -fRange * NearZ;
				shaderentity.SetCubeRemapNear(cubemapDepthRemapNear);
				shaderentity.SetCubeRemapFar(cubemapDepthRemapFar);
			}

			if (light.IsCastingShadow())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_CASTING_SHADOW);
			}
			if (light.IsStatic())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_STATIC);
			}

			if (light.IsVolumetricCloudsEnabled())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_VOLUMETRICCLOUDS);
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			lightarray_count_point++;
		}

		// Write rect lights into entity array:
		lightarray_offset_rect = entityCounter;
		for (uint32_t lightIndex : vis.visibleLights)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}

			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.GetType() != LightComponent::RECTANGLE || light.IsInactive())
				continue;

			ShaderEntity shaderentity = {};
			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->lights.GetEntity(lightIndex);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			shaderentity.SetType(ENTITY_TYPE_RECTLIGHT);
			shaderentity.position = light.position;
			shaderentity.SetRange(light.GetRange());
			shaderentity.SetLength(light.length);
			shaderentity.SetHeight(light.height);
			shaderentity.SetQuaternion(light.rotation);
			shaderentity.SetColor(float4(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity, 1));

			const bool shadowmap = IsShadowsEnabled() && light.IsCastingShadow() && !light.IsStatic();
			const wi::rectpacker::Rect& shadow_rect = vis.visibleLightShadowRects[lightIndex];

			const uint maskTex = light.maskTexDescriptor < 0 ? 0 : light.maskTexDescriptor;

			if (shadowmap)
			{
				shaderentity.shadowAtlasMulAdd.x = shadow_rect.w * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.y = shadow_rect.h * atlas_dim_rcp.y;
				shaderentity.shadowAtlasMulAdd.z = shadow_rect.x * atlas_dim_rcp.x;
				shaderentity.shadowAtlasMulAdd.w = shadow_rect.y * atlas_dim_rcp.y;
			}

			shaderentity.SetIndices(matrixCounter, maskTex);

			if (shadowmap || (maskTex > 0))
			{
				SHCAM shcam;
				CreateSpotLightShadowCam(light, shcam);
				XMStoreFloat4x4(&matrixArray[matrixCounter++], shcam.view_projection);
			}

			if (light.IsCastingShadow())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_CASTING_SHADOW);
			}
			if (light.IsStatic())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_STATIC);
			}

			if (light.IsVolumetricCloudsEnabled())
			{
				shaderentity.SetFlags(ENTITY_FLAG_LIGHT_VOLUMETRICCLOUDS);
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			lightarray_count_rect++;
		}

		lightarray_count = lightarray_count_directional + lightarray_count_spot + lightarray_count_point + lightarray_count_rect;
		frameCB.entity_culling_count = lightarray_count + decalarray_count + envprobearray_count;

		// Write colliders into entity array:
		forcefieldarray_offset = entityCounter;
		for (size_t i = 0; i < vis.visibleColliders.size(); ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}
			ShaderEntity shaderentity = {};

			const ColliderComponent& collider = vis.visibleColliders[i];
			shaderentity.layerMask = collider.layerMask;

			if (collider.IsCapsuleShadowEnabled())
			{
				shaderentity.SetFlags(ENTITY_FLAG_CAPSULE_SHADOW_COLLIDER);
			}

			switch (collider.shape)
			{
			case ColliderComponent::Shape::Sphere:
				shaderentity.SetType(ENTITY_TYPE_COLLIDER_SPHERE);
				shaderentity.position = collider.sphere.center;
				shaderentity.SetRange(collider.sphere.radius);
				break;
			case ColliderComponent::Shape::Capsule:
				shaderentity.SetType(ENTITY_TYPE_COLLIDER_CAPSULE);
				shaderentity.position = collider.capsule.base;
				shaderentity.SetColliderTip(collider.capsule.tip);
				shaderentity.SetRange(collider.capsule.radius);
				break;
			case ColliderComponent::Shape::Plane:
				shaderentity.SetType(ENTITY_TYPE_COLLIDER_PLANE);
				shaderentity.position = collider.plane.origin;
				shaderentity.SetDirection(collider.plane.normal);
				shaderentity.SetIndices(matrixCounter, 0);
				matrixArray[matrixCounter++] = collider.plane.projection;
				break;
			default:
				assert(0);
				break;
			}

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			forcefieldarray_count++;
		}

		// Write force fields into entity array:
		for (size_t i = 0; i < vis.scene->forces.GetCount(); ++i)
		{
			if (entityCounter == SHADER_ENTITY_COUNT)
			{
				entityCounter--;
				break;
			}
			ShaderEntity shaderentity = {};

			const ForceFieldComponent& force = vis.scene->forces[i];

			shaderentity.layerMask = ~0u;

			Entity entity = vis.scene->forces.GetEntity(i);
			const LayerComponent* layer = vis.scene->layers.GetComponent(entity);
			if (layer != nullptr)
			{
				shaderentity.layerMask = layer->layerMask;
			}

			switch (force.type)
			{
			default:
			case ForceFieldComponent::Type::Point:
				shaderentity.SetType(ENTITY_TYPE_FORCEFIELD_POINT);
				break;
			case ForceFieldComponent::Type::Plane:
				shaderentity.SetType(ENTITY_TYPE_FORCEFIELD_PLANE);
				break;
			}
			shaderentity.position = force.position;
			shaderentity.SetGravity(force.gravity);
			shaderentity.SetRange(std::max(0.001f, force.GetRange()));
			// The default planar force field is facing upwards, and thus the pull direction is downwards:
			shaderentity.SetDirection(force.direction);

			std::memcpy(entityArray + entityCounter, &shaderentity, sizeof(ShaderEntity));
			entityCounter++;
			forcefieldarray_count++;
		}
	}

	frameCB.probes = ShaderEntityIterator(envprobearray_offset, envprobearray_count);
	frameCB.directional_lights = ShaderEntityIterator(lightarray_offset_directional, lightarray_count_directional);
	frameCB.spotlights = ShaderEntityIterator(lightarray_offset_spot, lightarray_count_spot);
	frameCB.pointlights = ShaderEntityIterator(lightarray_offset_point, lightarray_count_point);
	frameCB.rectlights = ShaderEntityIterator(lightarray_offset_rect, lightarray_count_rect);
	frameCB.lights = ShaderEntityIterator(lightarray_offset, lightarray_count);
	frameCB.decals = ShaderEntityIterator(decalarray_offset, decalarray_count);
	frameCB.forces = ShaderEntityIterator(forcefieldarray_offset, forcefieldarray_count);
}
void UpdateRenderData(
	const Visibility& vis,
	const FrameCB& frameCB,
	CommandList cmd
)
{
	device->EventBegin("UpdateRenderData", cmd);

	auto prof_updatebuffer_cpu = wi::profiler::BeginRangeCPU("Update Buffers (CPU)");
	auto prof_updatebuffer_gpu = wi::profiler::BeginRangeGPU("Update Buffers (GPU)", cmd);

	PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_3D_WIND], textures[TEXTYPE_3D_WIND].desc.layout, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_3D_WIND_PREV], textures[TEXTYPE_3D_WIND_PREV].desc.layout, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_2D_CAUSTICS], textures[TEXTYPE_2D_CAUSTICS].desc.layout, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], ResourceState::VERTEX_BUFFER | ResourceState::INDIRECT_ARGUMENT, ResourceState::COPY_SRC));
	FlushBarriers(cmd);

	device->ClearUAV(&textures[TEXTYPE_3D_WIND], 0, cmd);
	device->ClearUAV(&textures[TEXTYPE_3D_WIND_PREV], 0, cmd);
	device->ClearUAV(&textures[TEXTYPE_2D_CAUSTICS], 0, cmd);
	PushBarrier(GPUBarrier::Memory());

	device->CopyBuffer(&indirectDebugStatsReadback[device->GetBufferIndex()], 0, &buffers[BUFFERTYPE_INDIRECT_DEBUG_0], 0, sizeof(IndirectDrawArgsInstanced), cmd);
	indirectDebugStatsReadback_available[device->GetBufferIndex()] = true;
	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], ResourceState::COPY_SRC, ResourceState::COPY_DST));

	PushBarrier(GPUBarrier::Buffer(&vis.scene->meshletBuffer, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS));

	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_FRAMECB], ResourceState::CONSTANT_BUFFER, ResourceState::COPY_DST));
	if (vis.scene->instanceBuffer.IsValid())
	{
		PushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer, ResourceState::SHADER_RESOURCE, ResourceState::COPY_DST));
	}
	if (vis.scene->geometryBuffer.IsValid())
	{
		PushBarrier(GPUBarrier::Buffer(&vis.scene->geometryBuffer, ResourceState::SHADER_RESOURCE, ResourceState::COPY_DST));
	}
	if (vis.scene->materialBuffer.IsValid())
	{
		PushBarrier(GPUBarrier::Buffer(&vis.scene->materialBuffer, ResourceState::SHADER_RESOURCE, ResourceState::COPY_DST));
	}
	if (vis.scene->skinningBuffer.IsValid())
	{
		PushBarrier(GPUBarrier::Buffer(&vis.scene->skinningBuffer, ResourceState::SHADER_RESOURCE, ResourceState::COPY_DST));
	}
	FlushBarriers(cmd);

	device->UpdateBuffer(&buffers[BUFFERTYPE_FRAMECB], &frameCB, cmd);
	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_FRAMECB], ResourceState::COPY_DST, ResourceState::CONSTANT_BUFFER));

	if (vis.scene->instanceBuffer.IsValid() && vis.scene->instanceArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->instanceBuffer,
			0,
			&vis.scene->instanceUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->instanceArraySize * sizeof(ShaderMeshInstance),
			cmd
		);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->geometryBuffer.IsValid() && vis.scene->geometryArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->geometryBuffer,
			0,
			&vis.scene->geometryUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->geometryArraySize * sizeof(ShaderGeometry),
			cmd
		);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->geometryBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->materialBuffer.IsValid() && vis.scene->materialArraySize > 0)
	{
		device->CopyBuffer(
			&vis.scene->materialBuffer,
			0,
			&vis.scene->materialUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->materialArraySize * sizeof(ShaderMaterial),
			cmd
		);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->materialBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->skinningBuffer.IsValid() && vis.scene->skinningDataSize > 0)
	{
		device->CopyBuffer(
			&vis.scene->skinningBuffer,
			0,
			&vis.scene->skinningUploadBuffer[device->GetBufferIndex()],
			0,
			vis.scene->skinningDataSize,
			cmd
		);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->skinningBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	if (vis.scene->voxelgrid_gpu.IsValid() && vis.scene->voxel_grids.GetCount() > 0)
	{
		VoxelGrid& voxelgrid = vis.scene->voxel_grids[0];
		device->UpdateBuffer(&vis.scene->voxelgrid_gpu, voxelgrid.voxels.data(), cmd, voxelgrid.voxels.size() * sizeof(uint64_t));
		PushBarrier(GPUBarrier::Buffer(&vis.scene->voxelgrid_gpu, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));
	}

	// Indirect debug buffer - clear indirect args:
	IndirectDrawArgsInstanced debug_indirect = {};
	debug_indirect.VertexCountPerInstance = 0;
	debug_indirect.InstanceCount = 1;
	debug_indirect.StartVertexLocation = 0;
	debug_indirect.StartInstanceLocation = 0;
	device->UpdateBuffer(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], &debug_indirect, cmd, sizeof(debug_indirect));
	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_INDIRECT_DEBUG_0], ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Buffer(&buffers[BUFFERTYPE_INDIRECT_DEBUG_1], ResourceState::UNORDERED_ACCESS, ResourceState::VERTEX_BUFFER | ResourceState::INDIRECT_ARGUMENT));

	// Flush buffer updates:
	FlushBarriers(cmd);

	wi::profiler::EndRange(prof_updatebuffer_cpu);
	wi::profiler::EndRange(prof_updatebuffer_gpu);

	BindCommonResources(cmd);

	{
		auto range = wi::profiler::BeginRangeGPU("Wind", cmd);
		device->EventBegin("Wind", cmd);
		if (
			vis.scene->weather.windDirection.x != 0 ||
			vis.scene->weather.windDirection.y != 0 ||
			vis.scene->weather.windDirection.z != 0
			)
		{
			device->BindComputeShader(&shaders[CSTYPE_WIND], cmd);
			device->BindUAV(&textures[TEXTYPE_3D_WIND], 0, cmd);
			device->BindUAV(&textures[TEXTYPE_3D_WIND_PREV], 1, cmd);
			const TextureDesc& desc = textures[TEXTYPE_3D_WIND].GetDesc();
			device->Dispatch(desc.width / 8, desc.height / 8, desc.depth / 8, cmd);
		}
		PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_3D_WIND], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_3D_WIND].desc.layout));
		PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_3D_WIND_PREV], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_3D_WIND_PREV].desc.layout));
		device->EventEnd(cmd);
		wi::profiler::EndRange(range);
	}
	{
		auto range = wi::profiler::BeginRangeGPU("Caustics", cmd);
		device->EventBegin("Caustics", cmd);
		device->BindComputeShader(&shaders[CSTYPE_CAUSTICS], cmd);
		device->BindUAV(&textures[TEXTYPE_2D_CAUSTICS], 0, cmd);
		const TextureDesc& desc = textures[TEXTYPE_2D_CAUSTICS].GetDesc();
		device->Dispatch(desc.width / 8, desc.height / 8, 1, cmd);
		PushBarrier(GPUBarrier::Image(&textures[TEXTYPE_2D_CAUSTICS], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_CAUSTICS].desc.layout));
		device->EventEnd(cmd);
		wi::profiler::EndRange(range);
	}

	{
		device->EventBegin("Skinning and Morph", cmd);
		auto range = wi::profiler::BeginRangeGPU("Skinning and Morph", cmd);
		int descriptor_skinningbuffer = -1;
		if (vis.scene->skinningBuffer.IsValid())
		{
			descriptor_skinningbuffer = device->GetDescriptorIndex(&vis.scene->skinningBuffer, SubresourceType::SRV);
		}
		else if (vis.scene->skinningUploadBuffer[device->GetBufferIndex()].IsValid())
		{
			// In this case we use the upload buffer directly, this will be the case with UMA GPU:
			descriptor_skinningbuffer = device->GetDescriptorIndex(&vis.scene->skinningUploadBuffer[device->GetBufferIndex()], SubresourceType::SRV);
		}
		device->BindComputeShader(&shaders[CSTYPE_SKINNING], cmd);
		for (size_t i = 0; i < vis.scene->meshes.GetCount(); ++i)
		{
			Entity entity = vis.scene->meshes.GetEntity(i);
			const MeshComponent& mesh = vis.scene->meshes[i];

			if (
				(mesh.IsSkinned() || !mesh.morph_targets.empty() || mesh.vb_bon.IsValid()) && // Note: even if all morphs are inactive, the skinning must be done
				mesh.streamoutBuffer.IsValid()
				)
			{
				SkinningPushConstants push;
				push.vb_pos_wind = mesh.vb_pos_wind.descriptor_srv;
				push.vb_nor = mesh.vb_nor.descriptor_srv;
				push.vb_tan = mesh.vb_tan.descriptor_srv;
				push.so_pos = mesh.so_pos.descriptor_uav;
				push.so_nor = mesh.so_nor.descriptor_uav;
				push.so_tan = mesh.so_tan.descriptor_uav;
				push.skinningbuffer_index = descriptor_skinningbuffer;
				const ArmatureComponent* armature = vis.scene->armatures.GetComponent(mesh.armatureID);
				if (armature != nullptr)
				{
					push.bone_offset = armature->gpuBoneOffset;
				}
				else
				{
					const SoftBodyPhysicsComponent* softbody = vis.scene->softbodies.GetComponent(entity);
					if (softbody != nullptr)
					{
						push.bone_offset = softbody->gpuBoneOffset;
					}
					else
					{
						push.bone_offset = ~0u;
					}
				}
				push.vb_bon = mesh.vb_bon.descriptor_srv;
				if (mesh.active_morph_count > 0)
				{
					push.morph_count = mesh.active_morph_count;
					push.morph_offset = mesh.morphGPUOffset;
					push.morphvb_index = mesh.vb_mor.descriptor_srv;
				}
				else
				{
					push.morph_count = 0;
					push.morph_offset = ~0u;
					push.morphvb_index = -1;
				}
				if (IsFormatUnorm(mesh.position_format))
				{
					push.aabb_min = mesh.aabb._min;
					push.aabb_max = mesh.aabb._max;
				}
				else
				{
					push.aabb_min = {};
					push.aabb_max = {};
				}
				push.vertexCount = (uint)mesh.vertex_positions.size();
				push.influence_div4 = mesh.GetBoneInfluenceDiv4();
				device->PushConstants(&push, sizeof(push), cmd);

				device->Dispatch(((uint32_t)mesh.vertex_positions.size() + 63) / 64, 1, 1, cmd);

				PushBarrier(GPUBarrier::Buffer(&mesh.streamoutBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
			}
		}

		wi::profiler::EndRange(range); // Skinning and Morph
		device->EventEnd(cmd); // Skinning and Morph
	}

	FlushBarriers(cmd); // wind/skinning flush

	// Hair particle systems GPU simulation:
	//	(This must be non-async too, as prepass will render hairs!)
	static thread_local wi::vector<HairParticleSystem::UpdateGPUItem> hair_updates;
	if (!vis.visibleHairs.empty())
	{
		auto range = wi::profiler::BeginRangeGPU("HairParticles - Simulate", cmd);
		for (uint32_t hairIndex : vis.visibleHairs)
		{
			const wi::HairParticleSystem& hair = vis.scene->hairs[hairIndex];
			const MeshComponent* mesh = vis.scene->meshes.GetComponent(hair.meshID);

			if (mesh != nullptr)
			{
				Entity entity = vis.scene->hairs.GetEntity(hairIndex);
				size_t materialIndex = vis.scene->materials.GetIndex(entity);
				const MaterialComponent& material = vis.scene->materials[materialIndex];
				auto& hair_update = hair_updates.emplace_back();
				hair_update.hair = &hair;
				hair_update.instanceIndex = (uint32_t)vis.scene->objects.GetCount() + hairIndex;
				hair_update.mesh = mesh;
				hair_update.material = &material;
			}
		}
		HairParticleSystem::UpdateGPU(hair_updates.data(), (uint32_t)hair_updates.size(), cmd);
		hair_updates.clear();
		wi::profiler::EndRange(range);
	}

	// Impostor prepare:
	if (vis.scene->impostors.GetCount() > 0 && vis.scene->objects.GetCount() > 0 && vis.scene->impostorBuffer.IsValid())
	{
		device->EventBegin("Impostor prepare", cmd);
		auto range = wi::profiler::BeginRangeGPU("Impostor prepare", cmd);

		PushBarrier(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT, ResourceState::COPY_DST));
		FlushBarriers(cmd);
		IndirectDrawArgsIndexedInstanced clear_indirect = {};
		clear_indirect.IndexCountPerInstance = 0;
		clear_indirect.InstanceCount = 1;
		clear_indirect.StartIndexLocation = 0;
		clear_indirect.BaseVertexLocation = 0;
		clear_indirect.StartInstanceLocation = 0;
		device->UpdateBuffer(&vis.scene->impostorBuffer, &clear_indirect, cmd, sizeof(clear_indirect), vis.scene->impostor_indirect.offset);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS));
		FlushBarriers(cmd);

		device->BindComputeShader(&shaders[CSTYPE_IMPOSTOR_PREPARE], cmd);
		device->BindUAV(&vis.scene->impostorBuffer, 0, cmd, vis.scene->impostor_ib_format == Format::R32_UINT ? vis.scene->impostor_ib32.subresource_uav : vis.scene->impostor_ib16.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 1, cmd, vis.scene->impostor_vb_pos.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 2, cmd, vis.scene->impostor_vb_nor.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 3, cmd, vis.scene->impostor_data.subresource_uav);
		device->BindUAV(&vis.scene->impostorBuffer, 4, cmd, vis.scene->impostor_indirect.subresource_uav);

		uint object_count = (uint)vis.scene->objects.GetCount();
		device->PushConstants(&object_count, sizeof(object_count), cmd);

		device->Dispatch((object_count + 63u) / 64u, 1, 1, cmd);

		PushBarrier(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT));
		FlushBarriers(cmd);

		wi::profiler::EndRange(range);
		device->EventEnd(cmd);
	}
	else if(vis.scene->impostors.GetCount() > 0 && vis.scene->objects.GetCount() == 0 && vis.scene->impostorBuffer.IsValid())
	{
		device->ClearUAV(&vis.scene->impostorBuffer, 0, cmd);
		PushBarrier(GPUBarrier::Buffer(&vis.scene->impostorBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE | ResourceState::INDIRECT_ARGUMENT));
		FlushBarriers(cmd);
	}

	// Meshlets:
	if(vis.scene->instanceArraySize > 0 && vis.scene->meshletBuffer.IsValid())
	{
		device->EventBegin("Meshlet prepare", cmd);
		auto range = wi::profiler::BeginRangeGPU("Meshlet prepare", cmd);
		device->BindComputeShader(&shaders[CSTYPE_MESHLET_PREPARE], cmd);

		const GPUResource* uavs[] = {
			&vis.scene->meshletBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch((uint32_t)vis.scene->instanceArraySize, 1, 1, cmd);

		PushBarrier(GPUBarrier::Buffer(&vis.scene->meshletBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));

		wi::profiler::EndRange(range);
		device->EventEnd(cmd);
	}

	FlushBarriers(cmd);

	device->EventEnd(cmd);
}


void UpdateRenderDataAsync(
	const Visibility& vis,
	const FrameCB& frameCB,
	CommandList cmd
)
{
	device->EventBegin("UpdateRenderDataAsync", cmd);

	BindCommonResources(cmd);

	for (size_t i = 0; i < vis.scene->terrains.GetCount(); ++i)
	{
		vis.scene->terrains[i].UpdateVirtualTexturesGPU(cmd);
	}

	// Wetmaps will be initialized:
	for (uint32_t objectIndex = 0; objectIndex < vis.scene->objects.GetCount(); ++objectIndex)
	{
		const ObjectComponent& object = vis.scene->objects[objectIndex];
		if (!object.wetmap.IsValid() || object.wetmap_cleared)
			continue;
		device->ClearUAV(&object.wetmap, 0, cmd);
		object.wetmap_cleared = true;
		PushBarrier(GPUBarrier::Buffer(&object.wetmap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE));
	}
	FlushBarriers(cmd);

	// Precompute static volumetric cloud textures:
	if (!volumetric_clouds_precomputed && vis.scene->weather.IsVolumetricClouds())
	{
		// Shape Noise pass:
		{
			device->EventBegin("Shape Noise", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHAPENOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_shapeNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_shapeNoise, texture_shapeNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 8;
			const int noiseThreadXY = static_cast<uint32_t>(std::ceil(texture_shapeNoise.GetDesc().width / threadSize));
			const int noiseThreadZ = static_cast<uint32_t>(std::ceil(texture_shapeNoise.GetDesc().depth / threadSize));

			device->Dispatch(noiseThreadXY, noiseThreadXY, noiseThreadZ, cmd);

			device->EventEnd(cmd);
		}

		// Detail Noise pass:
		{
			device->EventBegin("Detail Noise", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_DETAILNOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_detailNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_detailNoise, texture_detailNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 8;
			const int noiseThreadXYZ = static_cast<uint32_t>(std::ceil(texture_detailNoise.GetDesc().width / threadSize));

			device->Dispatch(noiseThreadXYZ, noiseThreadXYZ, noiseThreadXYZ, cmd);

			device->EventEnd(cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&texture_shapeNoise, ResourceState::UNORDERED_ACCESS, texture_shapeNoise.desc.layout),
				GPUBarrier::Image(&texture_detailNoise, ResourceState::UNORDERED_ACCESS, texture_detailNoise.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		// Generate mip chains for 3D textures:
		GenerateMipChain(texture_shapeNoise, MIPGENFILTER_LINEAR, cmd);
		GenerateMipChain(texture_detailNoise, MIPGENFILTER_LINEAR, cmd);

		// Curl Noise pass:
		{
			device->EventBegin("Curl Map", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_CURLNOISE], cmd);

			const GPUResource* uavs[] = {
				&texture_curlNoise,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_curlNoise, texture_curlNoise.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 16;
			const int curlRes = texture_curlNoise.GetDesc().width;
			const int curlThread = static_cast<uint32_t>(std::ceil(curlRes / threadSize));

			device->Dispatch(curlThread, curlThread, 1, cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_curlNoise, ResourceState::UNORDERED_ACCESS, texture_curlNoise.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}

		// Weather Map pass:
		{
			device->EventBegin("Weather Map", cmd);
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_WEATHERMAP], cmd);

			const GPUResource* uavs[] = {
				&texture_weatherMap,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture_weatherMap, texture_weatherMap.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			const int threadSize = 16;
			const int weatherMapRes = texture_weatherMap.GetDesc().width;
			const int weatherThread = static_cast<uint32_t>(std::ceil(weatherMapRes / threadSize));

			device->Dispatch(weatherThread, weatherThread, 1, cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(),
					GPUBarrier::Image(&texture_weatherMap, ResourceState::UNORDERED_ACCESS, texture_weatherMap.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}
		volumetric_clouds_precomputed = true;
	}

	if (vis.scene->weather.IsVolumetricClouds() && vis.scene->weather.IsVolumetricCloudsCastShadow())
	{
		const Texture* weatherMapFirst = vis.scene->weather.volumetricCloudsWeatherMapFirst.IsValid() ? &vis.scene->weather.volumetricCloudsWeatherMapFirst.GetTexture() : nullptr;
		const Texture* weatherMapSecond = vis.scene->weather.volumetricCloudsWeatherMapSecond.IsValid() ? &vis.scene->weather.volumetricCloudsWeatherMapSecond.GetTexture() : nullptr;
		ComputeVolumetricCloudShadows(cmd, weatherMapFirst, weatherMapSecond);
	}

	if (vis.scene->weather.IsRealisticSky())
	{
		wi::renderer::ComputeSkyAtmosphereTextures(cmd);
		wi::renderer::ComputeSkyAtmosphereSkyViewLut(cmd);
		if (vis.scene->weather.IsRealisticSkyAerialPerspective())
		{
			wi::renderer::ComputeSkyAtmosphereCameraVolumeLut(cmd);
		}
	}

	// GPU Particle systems simulation/sorting/culling:
	if (!vis.visibleEmitters.empty() || vis.scene->weather.rain_amount > 0)
	{
		auto range = wi::profiler::BeginRangeGPU("EmittedParticles - Simulate", cmd);
		for (uint32_t emitterIndex : vis.visibleEmitters)
		{
			const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
			Entity entity = vis.scene->emitters.GetEntity(emitterIndex);
			const TransformComponent& transform = *vis.scene->transforms.GetComponent(entity);
			const MeshComponent* mesh = vis.scene->meshes.GetComponent(emitter.meshID);
			const uint32_t instanceIndex = uint32_t(vis.scene->objects.GetCount() + vis.scene->hairs.GetCount()) + emitterIndex;

			emitter.UpdateGPU(instanceIndex, mesh, cmd);
		}
		if (vis.scene->weather.rain_amount > 0)
		{
			vis.scene->rainEmitter.UpdateGPU(vis.scene->rainInstanceOffset, nullptr, cmd);
		}
		wi::profiler::EndRange(range);
	}

	if (vis.scene->textureStreamingFeedbackBuffer.IsValid())
	{
		device->ClearUAV(&vis.scene->textureStreamingFeedbackBuffer, 0, cmd);
	}

	device->EventEnd(cmd);
}

void TextureStreamingReadbackCopy(
	const wi::scene::Scene& scene,
	wi::graphics::CommandList cmd
)
{
	if (scene.textureStreamingFeedbackBuffer.IsValid())
	{
		device->Barrier(GPUBarrier::Buffer(&scene.textureStreamingFeedbackBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::COPY_SRC), cmd);
		device->CopyResource(
			&scene.textureStreamingFeedbackBuffer_readback[device->GetBufferIndex()],
			&scene.textureStreamingFeedbackBuffer,
			cmd
		);
	}
}

void UpdateOcean(
	const Visibility& vis,
	CommandList cmd
)
{
	if (!vis.scene->weather.IsOceanEnabled() || !vis.scene->ocean.IsValid())
		return;
	bool occluded = false;
	if (vis.flags & wi::renderer::Visibility::ALLOW_OCCLUSION_CULLING)
	{
		occluded = vis.scene->ocean.IsOccluded();
	}
	if (!occluded)
	{
		auto range = wi::profiler::BeginRangeGPU("Ocean - Simulate", cmd);
		wi::renderer::BindCommonResources(cmd);
		vis.scene->ocean.UpdateDisplacementMap(cmd);
		wi::profiler::EndRange(range);
	}
}
void ReadbackOcean(
	const Visibility& vis,
	CommandList cmd
)
{
	if (!vis.scene->weather.IsOceanEnabled() || !vis.scene->ocean.IsValid())
		return;
	bool occluded = false;
	if (vis.flags & wi::renderer::Visibility::ALLOW_OCCLUSION_CULLING)
	{
		occluded = vis.scene->ocean.IsOccluded();
	}
	if (!occluded)
	{
		vis.scene->ocean.CopyDisplacementMapReadback(cmd);
	}
}

void UpdateRaytracingAccelerationStructures(const Scene& scene, CommandList cmd)
{
	if (device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
	{
		if (!scene.TLAS.IsValid())
			return;

		device->CopyBuffer(
			&scene.TLAS.desc.top_level.instance_buffer,
			0,
			&scene.TLAS_instancesUpload[device->GetBufferIndex()],
			0,
			scene.TLAS.desc.top_level.instance_buffer.desc.size,
			cmd
		);

		// BLAS:
		{
			auto rangeCPU = wi::profiler::BeginRangeCPU("BLAS Update (CPU)");
			auto range = wi::profiler::BeginRangeGPU("BLAS Update (GPU)", cmd);
			device->EventBegin("BLAS Update", cmd);

			for (size_t i = 0; i < scene.meshes.GetCount(); ++i)
			{
				const MeshComponent& mesh = scene.meshes[i];
				for (auto& BLAS : mesh.BLASes)
				{
					if (BLAS.IsValid())
					{
						switch (mesh.BLAS_state)
						{
						default:
						case MeshComponent::BLAS_STATE_COMPLETE:
							break;
						case MeshComponent::BLAS_STATE_NEEDS_REBUILD:
							device->BuildRaytracingAccelerationStructure(&BLAS, cmd, nullptr);
							break;
						case MeshComponent::BLAS_STATE_NEEDS_REFIT:
							device->BuildRaytracingAccelerationStructure(&BLAS, cmd, &BLAS);
							break;
						}
					}
				}
				mesh.BLAS_state = MeshComponent::BLAS_STATE_COMPLETE;
			}

			for (size_t i = 0; i < scene.hairs.GetCount(); ++i)
			{
				const wi::HairParticleSystem& hair = scene.hairs[i];

				if (hair.meshID != INVALID_ENTITY && hair.BLAS.IsValid())
				{
					if (hair.must_rebuild_blas)
					{
						device->BuildRaytracingAccelerationStructure(&hair.BLAS, cmd, nullptr);
						hair.must_rebuild_blas = false;
					}
					else
					{
						device->BuildRaytracingAccelerationStructure(&hair.BLAS, cmd, &hair.BLAS);
					}
				}
			}

			for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
			{
				const wi::EmittedParticleSystem& emitter = scene.emitters[i];

				if (emitter.BLAS.IsValid())
				{
					device->BuildRaytracingAccelerationStructure(&emitter.BLAS, cmd, nullptr);
				}
			}

			if (scene.weather.rain_amount > 0 && scene.rainEmitter.BLAS.IsValid())
			{
				device->BuildRaytracingAccelerationStructure(&scene.rainEmitter.BLAS, cmd, nullptr);
			}

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Buffer(&scene.TLAS.desc.top_level.instance_buffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE_COMPUTE),
					GPUBarrier::Memory(), // sync BLAS
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
			wi::profiler::EndRange(range);
			wi::profiler::EndRange(rangeCPU);
		}

		// TLAS:
		{
			auto rangeCPU = wi::profiler::BeginRangeCPU("TLAS Update (CPU)");
			auto range = wi::profiler::BeginRangeGPU("TLAS Update (GPU)", cmd);
			device->EventBegin("TLAS Update", cmd);

			device->BuildRaytracingAccelerationStructure(&scene.TLAS, cmd, nullptr);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(&scene.TLAS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
			wi::profiler::EndRange(range);
			wi::profiler::EndRange(rangeCPU);
		}
	}
	else
	{
		BindCommonResources(cmd);
		scene.BVH.Build(scene, cmd);
	}

	scene.acceleration_structure_update_requested = false;
}


void OcclusionCulling_Reset(const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;

	device->QueryReset(
		&queryHeap,
		0,
		queryHeap.desc.query_count,
		cmd
	);
}
void OcclusionCulling_Render(const CameraComponent& camera, const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	auto range = wi::profiler::BeginRangeGPU("Occlusion Culling Render", cmd);

	device->BindPipelineState(&PSO_occlusionquery, cmd);

	XMMATRIX VP = camera.GetViewProjection();

	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;
	int query_write = vis.scene->queryheap_idx;

	if (!vis.visibleObjects.empty())
	{
		device->EventBegin("Occlusion Culling Objects", cmd);

		for (uint32_t instanceIndex : vis.visibleObjects)
		{
			const Scene::OcclusionResult& occlusion_result = vis.scene->occlusion_results_objects[instanceIndex];

			int queryIndex = occlusion_result.occlusionQueries[query_write];
			if (queryIndex >= 0)
			{
				AABB aabb = vis.scene->aabb_objects[instanceIndex];
				// extrude the bounding box a bit:
				aabb._min.x -= 0.001f;
				aabb._min.y -= 0.001f;
				aabb._min.z -= 0.001f;
				aabb._max.x += 0.001f;
				aabb._max.y += 0.001f;
				aabb._max.z += 0.001f;
				const XMMATRIX transform = aabb.getAsBoxMatrix() * VP;
				device->PushConstants(&transform, sizeof(transform), cmd);

				// render bounding box to later read the occlusion status
				device->QueryBegin(&queryHeap, queryIndex, cmd);
				device->Draw(14, 0, cmd);
				device->QueryEnd(&queryHeap, queryIndex, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (!vis.visibleLights.empty())
	{
		device->EventBegin("Occlusion Culling Lights", cmd);

		for (uint32_t lightIndex : vis.visibleLights)
		{
			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.occlusionquery >= 0)
			{
				uint32_t queryIndex = (uint32_t)light.occlusionquery;
				const AABB& aabb = vis.scene->aabb_lights[lightIndex];
				const XMMATRIX transform = aabb.getAsBoxMatrix() * VP;
				device->PushConstants(&transform, sizeof(transform), cmd);

				device->QueryBegin(&queryHeap, queryIndex, cmd);
				device->Draw(14, 0, cmd);
				device->QueryEnd(&queryHeap, queryIndex, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (vis.scene->weather.IsOceanEnabled())
	{
		int queryIndex = vis.scene->ocean.occlusionQueries[query_write];
		if (queryIndex >= 0)
		{
			device->EventBegin("Occlusion Culling Ocean", cmd);

			device->QueryBegin(&queryHeap, queryIndex, cmd);
			vis.scene->ocean.RenderForOcclusionTest(camera, cmd);
			device->QueryEnd(&queryHeap, queryIndex, cmd);

			device->EventEnd(cmd);
		}
	}

	wi::profiler::EndRange(range); // Occlusion Culling Render
}
void OcclusionCulling_Resolve(const Visibility& vis, CommandList cmd)
{
	if (!GetOcclusionCullingEnabled() || GetFreezeCullingCameraEnabled() || !vis.scene->queryHeap.IsValid())
	{
		return;
	}
	if (vis.visibleObjects.empty() && vis.visibleLights.empty() && !vis.scene->weather.IsOceanEnabled())
	{
		return;
	}

	int query_write = vis.scene->queryheap_idx;
	const GPUQueryHeap& queryHeap = vis.scene->queryHeap;
	uint32_t queryCount = vis.scene->queryAllocator.load();

	// Resolve into readback buffer:
	device->QueryResolve(
		&queryHeap,
		0,
		queryCount,
		&vis.scene->queryResultBuffer[query_write],
		0ull,
		cmd
	);

	if (device->CheckCapability(GraphicsDeviceCapability::PREDICATION))
	{
		// Resolve into predication buffer:
		device->QueryResolve(
			&queryHeap,
			0,
			queryCount,
			&vis.scene->queryPredicationBuffer,
			0ull,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&vis.scene->queryPredicationBuffer, ResourceState::COPY_DST, ResourceState::PREDICATION),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}
}

void DrawWaterRipples(const Visibility& vis, CommandList cmd)
{
	// remove camera jittering
	CameraComponent cam = *vis.camera;
	cam.jitter = XMFLOAT2(0, 0);
	cam.UpdateCamera();
	const XMMATRIX VP = cam.GetViewProjection();

	XMVECTOR vvv = abs(vis.reflectionPlane.x) > 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
	XMVECTOR dir = XMLoadFloat4(&vis.reflectionPlane);
	XMMATRIX R = XMMatrixLookToLH(XMVectorZero(), dir, XMVector3Cross(vvv, dir));

	device->EventBegin("Water Ripples", cmd);
	for(auto& x : vis.scene->waterRipples)
	{
		x.params.customRotation = &R;
		x.params.customProjection = &VP;
		x.Draw(cmd);
	}
	device->EventEnd(cmd);
}



void DrawSoftParticles(
	const Visibility& vis,
	bool distortion, 
	CommandList cmd
)
{
	auto range = distortion ?
		wi::profiler::BeginRangeGPU("EmittedParticles - Render (Distortion)", cmd) :
		wi::profiler::BeginRangeGPU("EmittedParticles - Render", cmd);

	BindCommonResources(cmd);

	size_t emitterCount = vis.visibleEmitters.size();
	if (emitterCount > 0)
	{
		// Sort emitters based on distance:
		assert(emitterCount < 0x0000FFFF); // watch out for sorting hash truncation!
		static thread_local wi::vector<uint32_t> emitterSortingHashes;
		emitterSortingHashes.resize(emitterCount);
		for (size_t i = 0; i < emitterCount; ++i)
		{
			const uint32_t emitterIndex = vis.visibleEmitters[i];
			const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
			float distance = wi::math::DistanceEstimated(emitter.center, vis.camera->Eye);
			emitterSortingHashes[i] = 0;
			emitterSortingHashes[i] |= (uint32_t)i & 0x0000FFFF;
			emitterSortingHashes[i] |= (uint32_t)XMConvertFloatToHalf(distance) << 16u;
		}
		std::sort(emitterSortingHashes.begin(), emitterSortingHashes.end(), std::greater<uint32_t>());

		for (size_t i = 0; i < emitterCount; ++i)
		{
			const uint32_t emitterIndex = vis.visibleEmitters[emitterSortingHashes[i] & 0x0000FFFF];
			const wi::EmittedParticleSystem& emitter = vis.scene->emitters[emitterIndex];
			const Entity entity = vis.scene->emitters.GetEntity(emitterIndex);
			const MaterialComponent& material = *vis.scene->materials.GetComponent(entity);

			if (distortion && emitter.shaderType == wi::EmittedParticleSystem::SOFT_DISTORTION)
			{
				emitter.Draw(material, cmd);
			}
			else if (!distortion && (emitter.shaderType == wi::EmittedParticleSystem::SOFT || emitter.shaderType == wi::EmittedParticleSystem::SOFT_LIGHTING || emitter.shaderType == wi::EmittedParticleSystem::SIMPLE || IsWireRender()))
			{
				emitter.Draw(material, cmd);
			}
		}
	}

	if (distortion && vis.scene->weather.rain_amount > 0)
	{
		// Draw with distortion shader override:
		EmittedParticleSystem::PARTICLESHADERTYPE shader = EmittedParticleSystem::PARTICLESHADERTYPE::SOFT_DISTORTION;
		vis.scene->rainEmitter.Draw(vis.scene->rainMaterial, cmd, &shader);
	}
	else if (!distortion && vis.scene->weather.rain_amount > 0)
	{
		// Draw normally
		vis.scene->rainEmitter.Draw(vis.scene->rainMaterial, cmd);
	}

	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);

	wi::profiler::EndRange(range);
}
void DrawSpritesAndFonts(
	const Scene& scene,
	const CameraComponent& camera,
	bool distortion,
	CommandList cmd
)
{
	if (scene.sprites.GetCount() == 0 && scene.fonts.GetCount() == 0)
		return;
	if (distortion)
	{
		device->EventBegin("Sprites and Fonts (distortion)", cmd);
	}
	else
	{
		device->EventBegin("Sprites and Fonts", cmd);
	}
	const XMMATRIX VP = camera.GetViewProjection();
	const XMMATRIX R = XMLoadFloat3x3(&camera.rotationMatrix);
	enum TYPE // ordering of the enums is important, it is designed to prioritize font on top of sprite rendering if distance is the same
	{
		FONT,
		SPRITE,
	};
	union DistanceSorter {
		struct
		{
			uint64_t id : 32;
			uint64_t type : 16;
			uint64_t distance : 16;
		} bits;
		uint64_t raw;
		static_assert(sizeof(bits) == sizeof(raw));
	};
	static thread_local wi::vector<uint64_t> distance_sorter;
	distance_sorter.clear();
	for (size_t i = 0; i < scene.sprites.GetCount(); ++i)
	{
		const wi::Sprite& sprite = scene.sprites[i];
		if (sprite.IsHidden())
			continue;
		if (sprite.params.isExtractNormalMapEnabled() != distortion)
			continue;
		DistanceSorter sorter = {};
		sorter.bits.id = uint32_t(i);
		sorter.bits.type = SPRITE;
		Entity entity = scene.sprites.GetEntity(i);
		const TransformComponent* transform = scene.transforms.GetComponent(entity);
		if (transform != nullptr)
		{
			sorter.bits.distance = XMConvertFloatToHalf(wi::math::DistanceEstimated(transform->GetPosition(), camera.Eye));
		}
		distance_sorter.push_back(sorter.raw);
	}
	if (!distortion)
	{
		for (size_t i = 0; i < scene.fonts.GetCount(); ++i)
		{
			const wi::SpriteFont& font = scene.fonts[i];
			if (font.IsHidden())
				continue;
			AABB aabb = scene.aabb_fonts[i];
			Entity entity = scene.fonts.GetEntity(i);
			const TransformComponent* transform = scene.transforms.GetComponent(entity);
			float scale = 1;
			if (font.IsCameraScaling())
			{
				if (transform != nullptr)
				{
					scale *= 0.05f * wi::math::Distance(transform->GetPosition(), camera.Eye);
				}
			}
			aabb = aabb * scale;
			XMMATRIX M = XMMatrixIdentity();
			if (transform != nullptr)
			{
				M = transform->GetWorldMatrix();
			}
			if (font.IsCameraFacing())
			{
				M = R * M;
			}
			aabb = aabb.transform(M);
			//DrawBox(aabb);
			if (!camera.frustum.CheckBoxFast(aabb))
				continue;
			DistanceSorter sorter = {};
			sorter.bits.id = uint32_t(i);
			sorter.bits.type = FONT;
			if (transform != nullptr)
			{
				sorter.bits.distance = XMConvertFloatToHalf(wi::math::DistanceEstimated(transform->GetPosition(), camera.Eye));
			}
			distance_sorter.push_back(sorter.raw);
		}
	}
	std::sort(distance_sorter.begin(), distance_sorter.end(), std::greater<uint64_t>());
	for (auto& x : distance_sorter)
	{
		DistanceSorter sorter = {};
		sorter.raw = x;
		XMMATRIX M = VP;
		Entity entity = INVALID_ENTITY;
		const TransformComponent* transform = nullptr;

		switch (sorter.bits.type)
		{
		default:
		case SPRITE:
		{
			const wi::Sprite& sprite = scene.sprites[sorter.bits.id];
			wi::image::Params params = sprite.params;
			entity = scene.sprites.GetEntity(sorter.bits.id);
			transform = scene.transforms.GetComponent(entity);
			if (transform != nullptr)
			{
				M = XMLoadFloat4x4(&transform->world) * M;
				if (sprite.IsCameraScaling())
				{
					float scale = 0.05f * wi::math::Distance(transform->GetPosition(), camera.Eye);
					M = XMMatrixScaling(scale, scale, scale) * M;
				}
			}
			if (sprite.IsCameraFacing())
			{
				M = R * M;
			}
			params.customProjection = &M;
			if (sprite.maskResource.IsValid())
			{
				params.setMaskMap(&sprite.maskResource.GetTexture());
			}
			else
			{
				params.setMaskMap(nullptr);
			}
			wi::image::Draw(sprite.GetTexture(), params, cmd);
		}
		break;
		case FONT:
		{
			const wi::SpriteFont& font = scene.fonts[sorter.bits.id];
			wi::font::Params params = font.params;
			entity = scene.fonts.GetEntity(sorter.bits.id);
			transform = scene.transforms.GetComponent(entity);
			if (transform != nullptr)
			{
				M = XMLoadFloat4x4(&transform->world) * M;
				if (font.IsCameraScaling())
				{
					float scale = 0.05f * wi::math::Distance(transform->GetPosition(), camera.Eye);
					M = XMMatrixScaling(scale, scale, scale) * M;
				}
			}
			if (font.IsCameraFacing())
			{
				M = R * M;
			}
			params.customProjection = &M;
			wi::font::Draw(font.GetText().c_str(), font.GetCurrentTextLength(), params, cmd);
		}
		break;
		}
	}
	device->EventEnd(cmd);
}
void DrawLightVisualizers(
	const Visibility& vis,
	CommandList cmd,
	uint32_t instance_replication
)
{
	if (!vis.visibleLights.empty())
	{
		device->EventBegin("Light Visualizer Render", cmd);

		BindCommonResources(cmd);

		XMMATRIX camrot = XMLoadFloat3x3(&vis.camera->rotationMatrix);

		for (int type = LightComponent::POINT; type < LightComponent::LIGHTTYPE_COUNT; ++type)
		{
			device->BindPipelineState(&PSO_lightvisualizer[type], cmd);

			for (uint32_t lightIndex : vis.visibleLights)
			{
				const LightComponent& light = vis.scene->lights[lightIndex];

				if (light.GetType() == type && light.IsVisualizerEnabled())
				{
					VolumeLightCB lcb;
					lcb.xLightColor = XMFLOAT4(light.color.x, light.color.y, light.color.z, 1);
					lcb.xLightEnerdis = XMFLOAT4(light.intensity, light.GetRange(), light.outerConeAngle, light.intensity);

					if (type == LightComponent::POINT)
					{
						const float sphere_volume = std::max(1.0f, wi::math::SphereVolume(light.radius));
						lcb.xLightColor.x *= light.intensity / sphere_volume;
						lcb.xLightColor.y *= light.intensity / sphere_volume;
						lcb.xLightColor.z *= light.intensity / sphere_volume;
						lcb.xLightEnerdis.w = light.GetRange() * 0.025f; // scale
						XMStoreFloat4x4(&lcb.xLightWorld,
							XMMatrixScaling(lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, lcb.xLightEnerdis.w)*
							camrot*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
						);
						device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

						uint32_t vertexCount = vertexCount_uvsphere;
						uint32_t segmentCount_cylinder = 32;
						if (light.length > 0)
						{
							vertexCount += segmentCount_cylinder * 2 * 3;
						}
						GraphicsDevice::GPUAllocation allocation = device->AllocateGPU(vertexCount * sizeof(float4), cmd);
						float4* dst = (float4*)allocation.data;
						float rad = std::max(0.025f, light.radius);
						if (light.length > 0)
						{
							// Capsule from two half spheres and an uncapped cylinder:
							XMMATRIX M =
								XMMatrixScaling(rad, rad, rad) *
								XMMatrixTranslation(-light.length * 0.5f, 0, 0) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslation(light.position.x, light.position.y, light.position.z)
								;
							for (uint32_t i = 0; i < vertexCount_uvsphere; i += 3)
							{
								if (UVSPHERE[i].x <= 0.01f && UVSPHERE[i + 1].x <= 0.01f && UVSPHERE[i + 2].x <= 0.01f)
								{
									XMVECTOR pos = XMLoadFloat4(&UVSPHERE[i]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
									pos = XMLoadFloat4(&UVSPHERE[i + 1]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
									pos = XMLoadFloat4(&UVSPHERE[i + 2]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
								}
							}
							M =
								XMMatrixScaling(rad, rad, rad) *
								XMMatrixTranslation(light.length * 0.5f, 0, 0) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslation(light.position.x, light.position.y, light.position.z)
								;
							for (uint32_t i = 0; i < vertexCount_uvsphere; i += 3)
							{
								if (UVSPHERE[i].x >= -0.01f && UVSPHERE[i + 1].x >= -0.01f && UVSPHERE[i + 2].x >= -0.01f)
								{
									XMVECTOR pos = XMLoadFloat4(&UVSPHERE[i]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
									pos = XMLoadFloat4(&UVSPHERE[i + 1]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
									pos = XMLoadFloat4(&UVSPHERE[i + 2]);
									pos = XMVector3Transform(pos, M);
									XMStoreFloat4(dst, pos);
									dst++;
								}
							}
							M =
								XMMatrixScaling(light.length * 0.5f, rad, rad) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslation(light.position.x, light.position.y, light.position.z)
								;
							for (uint32_t i = 0; i < segmentCount_cylinder; ++i)
							{
								float t1 = float(i) / segmentCount_cylinder * XM_2PI;
								float t2 = float(i + 1) / segmentCount_cylinder * XM_2PI;
								XMVECTOR pos = XMVectorSet(-1, std::sin(t1), std::cos(t1), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
								pos = XMVectorSet(1, std::sin(t2), std::cos(t2), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
								pos = XMVectorSet(1, std::sin(t1), std::cos(t1), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
								pos = XMVectorSet(-1, std::sin(t1), std::cos(t1), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
								pos = XMVectorSet(-1, std::sin(t2), std::cos(t2), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
								pos = XMVectorSet(1, std::sin(t2), std::cos(t2), 1);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
							}
						}
						else
						{
							// Sphere:
							XMMATRIX M =
								XMMatrixScaling(rad, rad, rad) *
								XMMatrixTranslation(-light.length * 0.5f, 0, 0) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslation(light.position.x, light.position.y, light.position.z)
								;
							for (uint32_t i = 0; i < vertexCount_uvsphere; ++i)
							{
								XMVECTOR pos = XMLoadFloat4(&UVSPHERE[i]);
								pos = XMVector3Transform(pos, M);
								XMStoreFloat4(dst, pos);
								dst++;
							}
						}
						const GPUBuffer* vbs[] = {
							&allocation.buffer,
						};
						const uint32_t strides[] = {
							sizeof(float4),
						};
						const uint64_t offsets[] = {
							allocation.offset,
						};
						device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

						device->DrawInstanced(vertexCount, instance_replication, 0, 0, cmd);
					}
					else if (type == LightComponent::SPOT)
					{
						if (light.innerConeAngle > 0)
						{
							float coneS = (float)(std::min(light.innerConeAngle, light.outerConeAngle) * 2 / XM_PIDIV4);
							lcb.xLightEnerdis.w = light.GetRange() * 0.1f; // scale
							XMStoreFloat4x4(&lcb.xLightWorld,
								XMMatrixScaling(coneS * lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, coneS * lcb.xLightEnerdis.w) *
								XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
								XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
							);

							device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

							device->Draw(vertexCount_cone, 0, cmd);
						}

						float coneS = (float)(light.outerConeAngle * 2 / XM_PIDIV4);
						lcb.xLightEnerdis.w = light.GetRange() * 0.1f; // scale
						XMStoreFloat4x4(&lcb.xLightWorld,
							XMMatrixScaling(coneS*lcb.xLightEnerdis.w, lcb.xLightEnerdis.w, coneS*lcb.xLightEnerdis.w)*
							XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))*
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
						);

						device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

						device->DrawInstanced(vertexCount_cone, instance_replication, 0, 0, cmd);
					}
					else if (type == LightComponent::RECTANGLE)
					{
						lcb.xLightEnerdis.y = (float)light.maskTexDescriptor;
						lcb.xLightEnerdis.z = light.length;
						lcb.xLightEnerdis.w = light.height;
						XMStoreFloat4x4(&lcb.xLightWorld,
							XMMatrixScaling(light.length * 0.5f, light.height * 0.5f, 1) *
							XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
							XMMatrixTranslationFromVector(XMLoadFloat3(&light.position))
						);

						device->BindDynamicConstantBuffer(lcb, CB_GETBINDSLOT(VolumeLightCB), cmd);

						device->DrawInstanced(4, instance_replication, 0, 0, cmd);
					}
				}
			}

		}

		device->EventEnd(cmd);

	}
}
void DrawVolumeLights(
	const Visibility& vis,
	CommandList cmd
)
{
	if (vis.visibleLights.empty())
		return;

	device->EventBegin("Volumetric Light Render", cmd);

	BindCommonResources(cmd);

	XMMATRIX VP = vis.camera->GetViewProjection();
	const XMVECTOR CamPos = vis.camera->GetEye();

	for (int type = 0; type < LightComponent::LIGHTTYPE_COUNT; ++type)
	{
		const PipelineState& pso = PSO_volumetriclight[type];

		if (!pso.IsValid())
			continue;

		device->BindPipelineState(&pso, cmd);

		int type_idx = -1;
		for (size_t i = 0; i < vis.visibleLights.size(); ++i)
		{
			const uint32_t lightIndex = vis.visibleLights[i];
			const LightComponent& light = vis.scene->lights[lightIndex];
			if (light.GetType() != type)
				continue;
			type_idx++;

			if (!light.IsVolumetricsEnabled())
				continue;

			switch (type)
			{
			case LightComponent::DIRECTIONAL:
			{
				MiscCB miscCb;
				miscCb.g_xColor.x = float(type_idx);
				miscCb.g_xColor.y = light.volumetric_boost;
				device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

				device->Draw(3, 0, cmd); // full screen triangle
			}
			break;
			case LightComponent::POINT:
			{
				MiscCB miscCb;
				miscCb.g_xColor.x = float(type_idx);
				miscCb.g_xColor.y = light.volumetric_boost;
				float sca = light.GetRange() + 1;
				XMStoreFloat4x4(&miscCb.g_xTransform, XMMatrixScaling(sca, sca, sca) * XMMatrixTranslationFromVector(XMLoadFloat3(&light.position)) * VP);
				device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

				device->Draw(240, 0, cmd); // icosphere
			}
			break;
			case LightComponent::SPOT:
			{
				MiscCB miscCb;
				miscCb.g_xColor.x = float(type_idx);
				miscCb.g_xColor.y = light.volumetric_boost;

				uint packed = wi::math::pack_half2(std::pow(std::sin(light.outerConeAngle), 2.0f), std::pow(std::cos(light.outerConeAngle), 2.0f));
				std::memcpy(&miscCb.g_xColor.z, &packed, sizeof(packed));

				const XMVECTOR LightPos = XMLoadFloat3(&light.position);
				const XMVECTOR LightDirection = XMLoadFloat3(&light.direction);
				const XMVECTOR L = XMVector3Normalize(LightPos - CamPos);
				const float spot_factor = XMVectorGetX(XMVector3Dot(L, LightDirection));
				const float spot_cutoff = std::cos(light.outerConeAngle);
				miscCb.g_xColor.w = (spot_factor < spot_cutoff) ? 1.0f : 0.0f;

				const float coneS = (const float)(light.outerConeAngle * 2 / XM_PIDIV4);
				XMStoreFloat4x4(&miscCb.g_xTransform,
					XMMatrixScaling(coneS * light.GetRange(), light.GetRange(), coneS * light.GetRange()) *
					XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
					XMMatrixTranslationFromVector(LightPos) *
					VP
				);
				device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

				device->Draw(192, 0, cmd); // cone
			}
			break;
			case LightComponent::RECTANGLE:
			{
				MiscCB miscCb;
				miscCb.g_xColor.x = float(type_idx);
				miscCb.g_xColor.y = light.volumetric_boost;

				const XMVECTOR LightPos = XMLoadFloat3(&light.position);

				XMStoreFloat4x4(&miscCb.g_xTransform,
					XMMatrixScaling(light.GetRange(), light.GetRange(), light.GetRange()) *
					XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation)) *
					XMMatrixTranslationFromVector(LightPos) *
					VP
				);
				device->BindDynamicConstantBuffer(miscCb, CB_GETBINDSLOT(MiscCB), cmd);

				device->Draw(14, 0, cmd); // cube
			}
			break;
			}
		}

	}

	device->EventEnd(cmd);
}
void DrawLensFlares(
	const Visibility& vis,
	CommandList cmd,
	const Texture* texture_directional_occlusion
)
{
	if (IsWireRender())
		return;

	device->EventBegin("Lens Flares", cmd);

	BindCommonResources(cmd);

	for (uint32_t lightIndex : vis.visibleLights)
	{
		const LightComponent& light = vis.scene->lights[lightIndex];

		if (!light.lensFlareRimTextures.empty())
		{
			XMVECTOR POS;

			if (light.GetType() == LightComponent::DIRECTIONAL)
			{
				// directional light flare will be placed at infinite position along direction vector:
				XMVECTOR D = XMVector3Normalize(-XMVector3Transform(XMVectorSet(0, 1, 0, 1), XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation))));
				if (XMVectorGetX(XMVector3Dot(D, XMVectorSet(0, -1, 0, 0))) < 0)
					continue; // sun below horizon, skip lensflare
				POS = vis.camera->GetEye() + D * -vis.camera->zFarP;

				// Directional light can use occlusion texture (eg. clouds):
				if (texture_directional_occlusion == nullptr)
				{
					device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
				}
				else
				{
					device->BindResource(texture_directional_occlusion, 0, cmd);
				}
			}
			else
			{
				// point and spotlight flare will be placed to the source position:
				POS = XMLoadFloat3(&light.position);

				// not using occlusion texture
				device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
			}

			if (XMVectorGetX(XMVector3Dot(XMVectorSubtract(POS, vis.camera->GetEye()), vis.camera->GetAt())) > 0) // check if the camera is facing towards the flare or not
			{
				device->BindPipelineState(&PSO_lensflare, cmd);

				// Get the screen position of the flare:
				XMVECTOR flarePos = XMVector3Project(POS, 0, 0, 1, 1, 1, 0, vis.camera->GetProjection(), vis.camera->GetView(), XMMatrixIdentity());

				LensFlarePush cb;
				XMStoreFloat3(&cb.xLensFlarePos, flarePos);

				uint32_t i = 0;
				for (auto& x : light.lensFlareRimTextures)
				{
					if (x.IsValid())
					{
						// pre-baked offsets
						// These values work well for me, but should be tweakable
						static const float mods[] = { 1.0f,0.55f,0.4f,0.1f,-0.1f,-0.3f,-0.5f };
						if (i >= arraysize(mods))
							break;

						cb.xLensFlareOffset = mods[i];
						cb.xLensFlareSize.x = (float)x.GetTexture().desc.width;
						cb.xLensFlareSize.y = (float)x.GetTexture().desc.height;

						device->PushConstants(&cb, sizeof(cb), cmd);

						device->BindResource(&x.GetTexture(), 1, cmd);
						device->Draw(4, 0, cmd);
						i++;
					}
				}
			}

		}

	}

	device->EventEnd(cmd);
}


void SetShadowProps2D(int resolution)
{
	max_shadow_resolution_2D = resolution;
}
void SetShadowPropsCube(int resolution)
{
	max_shadow_resolution_cube = resolution;
}

void DrawShadowmaps(
	const Visibility& vis,
	CommandList cmd
)
{
	if (IsWireRender() || !IsShadowsEnabled())
		return;
	if (!shadowMapAtlas.IsValid())
		return;
	if (vis.visibleLights.empty() && vis.scene->weather.rain_amount <= 0)
		return;

	device->EventBegin("DrawShadowmaps", cmd);
	auto range_cpu = wi::profiler::BeginRangeCPU("Shadowmap Rendering");
	auto range_gpu = wi::profiler::BeginRangeGPU("Shadowmap Rendering", cmd);

	const bool predicationRequest =
		device->CheckCapability(GraphicsDeviceCapability::PREDICATION) &&
		GetOcclusionCullingEnabled();

	const bool shadow_lod_override = IsShadowLODOverrideEnabled();

	BindCommonResources(cmd);

	BoundingFrustum cam_frustum;
	BoundingFrustum::CreateFromMatrix(cam_frustum, vis.camera->GetProjection());
	std::swap(cam_frustum.Near, cam_frustum.Far);
	cam_frustum.Transform(cam_frustum, vis.camera->GetInvView());
	XMStoreFloat4(&cam_frustum.Orientation, XMQuaternionNormalize(XMLoadFloat4(&cam_frustum.Orientation)));

	CameraCB cb;
	cb.init();

	const XMVECTOR EYE = vis.camera->GetEye();

	const uint32_t max_viewport_count = device->GetMaxViewportCount();

	const RenderPassImage rp[] = {
		RenderPassImage::DepthStencil(
			&shadowMapAtlas,
			RenderPassImage::LoadOp::CLEAR,
			RenderPassImage::StoreOp::STORE,
			ResourceState::SHADER_RESOURCE,
			ResourceState::DEPTHSTENCIL,
			ResourceState::SHADER_RESOURCE
		),
		RenderPassImage::RenderTarget(
			&shadowMapAtlas_Transparent,
			RenderPassImage::LoadOp::CLEAR,
			RenderPassImage::StoreOp::STORE,
			ResourceState::SHADER_RESOURCE,
			ResourceState::SHADER_RESOURCE
		),
	};
	device->RenderPassBegin(rp, arraysize(rp), cmd);

	for (uint32_t lightIndex : vis.visibleLights)
	{
		const LightComponent& light = vis.scene->lights[lightIndex];
		if (light.IsInactive())
			continue;

		const bool shadow = light.IsCastingShadow() && !light.IsStatic();
		if (!shadow)
			continue;
		const wi::rectpacker::Rect& shadow_rect = vis.visibleLightShadowRects[lightIndex];

		renderQueue.init();
		renderQueue_transparent.init();

		switch (light.GetType())
		{
		case LightComponent::DIRECTIONAL:
		{
			if (max_shadow_resolution_2D == 0 && light.forced_shadow_resolution < 0)
				break;
			if (light.cascade_distances.empty())
				break;

			const uint32_t cascade_count = std::min((uint32_t)light.cascade_distances.size(), max_viewport_count);
			Viewport* viewports = (Viewport*)alloca(sizeof(Viewport) * cascade_count);
			Rect* scissors = (Rect*)alloca(sizeof(Rect) * cascade_count);
			SHCAM* shcams = (SHCAM*)alloca(sizeof(SHCAM) * cascade_count);
			CreateDirLightShadowCams(light, *vis.camera, shcams, cascade_count, shadow_rect);

			for (size_t i = 0; i < vis.scene->aabb_objects.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_objects[i];
				if (aabb.layerMask & vis.layerMask)
				{
					const ObjectComponent& object = vis.scene->objects[i];
					if (object.IsRenderable() && object.IsCastingShadow())
					{
						const float distanceSq = wi::math::DistanceSquared(EYE, object.center);
						if (distanceSq > sqr(object.draw_distance + object.radius)) // Note: here I use draw_distance instead of fadeDeistance because this doesn't account for impostor switch fade
							continue;

						// Determine which cascades the object is contained in:
						uint8_t camera_mask = 0;
						uint8_t shadow_lod = 0xFF;
						for (uint32_t cascade = 0; cascade < cascade_count; ++cascade)
						{
							if ((cascade < (cascade_count - object.cascadeMask)) && shcams[cascade].frustum.CheckBoxFast(aabb))
							{
								camera_mask |= 1 << cascade;
								if (shadow_lod_override)
								{
									const uint8_t candidate_lod = (uint8_t)vis.scene->ComputeObjectLODForView(object, aabb, vis.scene->meshes[object.mesh_index], shcams[cascade].view_projection);
									shadow_lod = std::min(shadow_lod, candidate_lod);
								}
							}
						}
						if (camera_mask == 0)
							continue;

						RenderBatch batch;
						batch.Create(object.mesh_index, uint32_t(i), 0, object.sort_bits, camera_mask, shadow_lod);

						const uint32_t filterMask = object.GetFilterMask();
						if (filterMask & FILTER_OPAQUE)
						{
							renderQueue.add(batch);
						}
						if ((filterMask & FILTER_TRANSPARENT) || (filterMask & FILTER_WATER))
						{
							renderQueue_transparent.add(batch);
						}
					}
				}
			}

			if (!renderQueue.empty() || !renderQueue_transparent.empty())
			{
				for (uint32_t cascade = 0; cascade < cascade_count; ++cascade)
				{
					XMStoreFloat4x4(&cb.cameras[cascade].view_projection, shcams[cascade].view_projection);
					cb.cameras[cascade].output_index = cascade;
					for (int i = 0; i < arraysize(cb.cameras[cascade].frustum.planes); ++i)
					{
						cb.cameras[cascade].frustum.planes[i] = shcams[cascade].frustum.planes[i];
					}
					cb.cameras[cascade].options = SHADERCAMERA_OPTION_ORTHO;
					cb.cameras[cascade].forward.x = -light.direction.x;
					cb.cameras[cascade].forward.y = -light.direction.y;
					cb.cameras[cascade].forward.z = -light.direction.z;

					Viewport vp;
					vp.top_left_x = float(shadow_rect.x + cascade * shadow_rect.w);
					vp.top_left_y = float(shadow_rect.y);
					vp.width = float(shadow_rect.w);
					vp.height = float(shadow_rect.h);
					viewports[cascade] = vp; // instead of reference, copy it to be sure every member is initialized (alloca)
					scissors[cascade].from_viewport(vp);
				}

				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);
				device->BindViewports(cascade_count, viewports, cmd);
				device->BindScissorRects(cascade_count, scissors, cmd);

				renderQueue.sort_opaque();
				renderQueue_transparent.sort_transparent();
				RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, FILTER_OPAQUE, cmd, 0, cascade_count);
				RenderMeshes(vis, renderQueue_transparent, RENDERPASS_SHADOW, FILTER_TRANSPARENT | FILTER_WATER, cmd, 0, cascade_count);
			}

			if (!vis.visibleHairs.empty())
			{
				cb.cameras[0].position = vis.camera->Eye;
				for (uint32_t cascade = 0; cascade < std::min(2u, cascade_count); ++cascade)
				{
					XMStoreFloat4x4(&cb.cameras[0].view_projection, shcams[cascade].view_projection);
					device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

					Viewport vp;
					vp.top_left_x = float(shadow_rect.x + cascade * shadow_rect.w);
					vp.top_left_y = float(shadow_rect.y);
					vp.width = float(shadow_rect.w);
					vp.height = float(shadow_rect.h);
					device->BindViewports(1, &vp, cmd);

					Rect scissor;
					scissor.from_viewport(vp);
					device->BindScissorRects(1, &scissor, cmd);

					for (uint32_t hairIndex : vis.visibleHairs)
					{
						const HairParticleSystem& hair = vis.scene->hairs[hairIndex];
						if (!shcams[cascade].frustum.CheckBoxFast(hair.aabb))
							continue;
						Entity entity = vis.scene->hairs.GetEntity(hairIndex);
						const MaterialComponent* material = vis.scene->materials.GetComponent(entity);
						if (material != nullptr)
						{
							hair.Draw(*material, RENDERPASS_SHADOW, cmd);
						}
					}
				}
			}
		}
		break;
		case LightComponent::SPOT:
		case LightComponent::RECTANGLE:
		{
			if (max_shadow_resolution_2D == 0 && light.forced_shadow_resolution < 0)
				break;

			SHCAM shcam;
			CreateSpotLightShadowCam(light, shcam);
			if (!cam_frustum.Intersects(shcam.boundingfrustum))
				break;

			for (size_t i = 0; i < vis.scene->aabb_objects.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_objects[i];
				if ((aabb.layerMask & vis.layerMask) && shcam.frustum.CheckBoxFast(aabb))
				{
					const ObjectComponent& object = vis.scene->objects[i];
					if (object.IsRenderable() && object.IsCastingShadow())
					{
						const float distanceSq = wi::math::DistanceSquared(EYE, object.center);
						if (distanceSq > sqr(object.draw_distance + object.radius)) // Note: here I use draw_distance instead of fadeDeistance because this doesn't account for impostor switch fade
							continue;

						uint8_t shadow_lod = 0xFF;
						if (shadow_lod_override)
						{
							const uint8_t candidate_lod = (uint8_t)vis.scene->ComputeObjectLODForView(object, aabb, vis.scene->meshes[object.mesh_index], shcam.view_projection);
							shadow_lod = std::min(shadow_lod, candidate_lod);
						}

						RenderBatch batch;
						batch.Create(object.mesh_index, uint32_t(i), 0, object.sort_bits, 0xFF, shadow_lod);

						const uint32_t filterMask = object.GetFilterMask();
						if (filterMask & FILTER_OPAQUE)
						{
							renderQueue.add(batch);
						}
						if ((filterMask & FILTER_TRANSPARENT) || (filterMask & FILTER_WATER))
						{
							renderQueue_transparent.add(batch);
						}
					}
				}
			}

			if (predicationRequest && light.occlusionquery >= 0)
			{
				device->PredicationBegin(
					&vis.scene->queryPredicationBuffer,
					(uint64_t)light.occlusionquery * sizeof(uint64_t),
					PredicationOp::EQUAL_ZERO,
					cmd
				);
			}

			if (!renderQueue.empty() || !renderQueue_transparent.empty())
			{
				XMStoreFloat4x4(&cb.cameras[0].view_projection, shcam.view_projection);
				cb.cameras[0].output_index = 0;
				for (int i = 0; i < arraysize(cb.cameras[0].frustum.planes); ++i)
				{
					cb.cameras[0].frustum.planes[i] = shcam.frustum.planes[i];
				}
				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

				Viewport vp;
				vp.top_left_x = float(shadow_rect.x);
				vp.top_left_y = float(shadow_rect.y);
				vp.width = float(shadow_rect.w);
				vp.height = float(shadow_rect.h);
				device->BindViewports(1, &vp, cmd);

				Rect scissor;
				scissor.from_viewport(vp);
				device->BindScissorRects(1, &scissor, cmd);

				renderQueue.sort_opaque();
				renderQueue_transparent.sort_transparent();
				RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, FILTER_OPAQUE, cmd);
				RenderMeshes(vis, renderQueue_transparent, RENDERPASS_SHADOW, FILTER_TRANSPARENT | FILTER_WATER, cmd);
			}

			if (!vis.visibleHairs.empty())
			{
				cb.cameras[0].position = vis.camera->Eye;
				cb.cameras[0].options = SHADERCAMERA_OPTION_NONE;
				XMStoreFloat4x4(&cb.cameras[0].view_projection, shcam.view_projection);
				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

				Viewport vp;
				vp.top_left_x = float(shadow_rect.x);
				vp.top_left_y = float(shadow_rect.y);
				vp.width = float(shadow_rect.w);
				vp.height = float(shadow_rect.h);
				device->BindViewports(1, &vp, cmd);

				Rect scissor;
				scissor.from_viewport(vp);
				device->BindScissorRects(1, &scissor, cmd);

				for (uint32_t hairIndex : vis.visibleHairs)
				{
					const HairParticleSystem& hair = vis.scene->hairs[hairIndex];
					if (!shcam.frustum.CheckBoxFast(hair.aabb))
						continue;
					Entity entity = vis.scene->hairs.GetEntity(hairIndex);
					const MaterialComponent* material = vis.scene->materials.GetComponent(entity);
					if (material != nullptr)
					{
						hair.Draw(*material, RENDERPASS_SHADOW, cmd);
					}
				}
			}

			if (predicationRequest && light.occlusionquery >= 0)
			{
				device->PredicationEnd(cmd);
			}
		}
		break;
		case LightComponent::POINT:
		{
			if (max_shadow_resolution_cube == 0 && light.forced_shadow_resolution < 0)
				break;

			Sphere boundingsphere(light.position, light.GetRange());

			const float zNearP = 0.1f;
			const float zFarP = std::max(1.0f, light.GetRange());
			SHCAM cameras[6];
			CreateCubemapCameras(light.position, zNearP, zFarP, cameras, arraysize(cameras));
			Viewport vp[arraysize(cameras)];
			Rect scissors[arraysize(cameras)];
			Frustum frusta[arraysize(cameras)];
			uint32_t camera_count = 0;

			for (uint32_t shcam = 0; shcam < arraysize(cameras); ++shcam)
			{
				// always set up viewport and scissor just to be safe, even if this one is skipped:
				vp[shcam].top_left_x = float(shadow_rect.x + shcam * shadow_rect.w);
				vp[shcam].top_left_y = float(shadow_rect.y);
				vp[shcam].width = float(shadow_rect.w);
				vp[shcam].height = float(shadow_rect.h);
				scissors[shcam].from_viewport(vp[shcam]);

				// Check if cubemap face frustum is visible from main camera, otherwise, it will be skipped:
				if (cam_frustum.Intersects(cameras[shcam].boundingfrustum))
				{
					XMStoreFloat4x4(&cb.cameras[camera_count].view_projection, cameras[shcam].view_projection);
					// We no longer have a straight mapping from camera to viewport:
					//	- there will be always 6 viewports
					//	- there will be only as many cameras, as many cubemap face frustums are visible from main camera
					//	- output_index is mapping camera to viewport, used by shader to output to SV_ViewportArrayIndex
					cb.cameras[camera_count].output_index = shcam;
					cb.cameras[camera_count].options = SHADERCAMERA_OPTION_NONE;
					for (int i = 0; i < arraysize(cb.cameras[camera_count].frustum.planes); ++i)
					{
						cb.cameras[camera_count].frustum.planes[i] = cameras[shcam].frustum.planes[i];
					}
					frusta[camera_count] = cameras[shcam].frustum;
					camera_count++;
				}
			}

			for (size_t i = 0; i < vis.scene->aabb_objects.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_objects[i];
				if ((aabb.layerMask & vis.layerMask) && boundingsphere.intersects(aabb))
				{
					const ObjectComponent& object = vis.scene->objects[i];
					if (object.IsRenderable() && object.IsCastingShadow())
					{
						const float distanceSq = wi::math::DistanceSquared(EYE, object.center);
						if (distanceSq > sqr(object.draw_distance + object.radius)) // Note: here I use draw_distance instead of fadeDeistance because this doesn't account for impostor switch fade
							continue;

						// Check for each frustum, if object is visible from it:
						uint8_t camera_mask = 0;
						uint8_t shadow_lod = 0xFF;
						for (uint32_t camera_index = 0; camera_index < camera_count; ++camera_index)
						{
							if (frusta[camera_index].CheckBoxFast(aabb))
							{
								camera_mask |= 1 << camera_index;
								if (shadow_lod_override)
								{
									const uint8_t candidate_lod = (uint8_t)vis.scene->ComputeObjectLODForView(object, aabb, vis.scene->meshes[object.mesh_index], cameras[camera_index].view_projection);
									shadow_lod = std::min(shadow_lod, candidate_lod);
								}
							}
						}
						if (camera_mask == 0)
							continue;

						RenderBatch batch;
						batch.Create(object.mesh_index, uint32_t(i), 0, object.sort_bits, camera_mask, shadow_lod);

						const uint32_t filterMask = object.GetFilterMask();
						if (filterMask & FILTER_OPAQUE)
						{
							renderQueue.add(batch);
						}
						if ((filterMask & FILTER_TRANSPARENT) || (filterMask & FILTER_WATER))
						{
							renderQueue_transparent.add(batch);
						}
					}
				}
			}

			if (predicationRequest && light.occlusionquery >= 0)
			{
				device->PredicationBegin(
					&vis.scene->queryPredicationBuffer,
					(uint64_t)light.occlusionquery * sizeof(uint64_t),
					PredicationOp::EQUAL_ZERO,
					cmd
				);
			}

			if (!renderQueue.empty() || renderQueue_transparent.empty())
			{
				device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);
				device->BindViewports(arraysize(vp), vp, cmd);
				device->BindScissorRects(arraysize(scissors), scissors, cmd);

				renderQueue.sort_opaque();
				renderQueue_transparent.sort_transparent();
				RenderMeshes(vis, renderQueue, RENDERPASS_SHADOW, FILTER_OPAQUE, cmd, 0, camera_count);
				RenderMeshes(vis, renderQueue_transparent, RENDERPASS_SHADOW, FILTER_TRANSPARENT | FILTER_WATER, cmd, 0, camera_count);
			}

			if (!vis.visibleHairs.empty())
			{
				cb.cameras[0].position = vis.camera->Eye;
				for (uint32_t shcam = 0; shcam < arraysize(cameras); ++shcam)
				{
					XMStoreFloat4x4(&cb.cameras[0].view_projection, cameras[shcam].view_projection);
					device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

					Viewport vp;
					vp.top_left_x = float(shadow_rect.x + shcam * shadow_rect.w);
					vp.top_left_y = float(shadow_rect.y);
					vp.width = float(shadow_rect.w);
					vp.height = float(shadow_rect.h);
					device->BindViewports(1, &vp, cmd);

					Rect scissor;
					scissor.from_viewport(vp);
					device->BindScissorRects(1, &scissor, cmd);

					for (uint32_t hairIndex : vis.visibleHairs)
					{
						const HairParticleSystem& hair = vis.scene->hairs[hairIndex];
						if (!cameras[shcam].frustum.CheckBoxFast(hair.aabb))
							continue;
						Entity entity = vis.scene->hairs.GetEntity(hairIndex);
						const MaterialComponent* material = vis.scene->materials.GetComponent(entity);
						if (material != nullptr)
						{
							hair.Draw(*material, RENDERPASS_SHADOW, cmd);
						}
					}
				}
			}

			if (predicationRequest && light.occlusionquery >= 0)
			{
				device->PredicationEnd(cmd);
			}

		}
		break;
		default:
			break;
		} // terminate switch
	}

	// Rain blocker:
	if (vis.scene->weather.rain_amount > 0)
	{
		SHCAM shcam;
		CreateDirLightShadowCams(vis.scene->rain_blocker_dummy_light, *vis.camera, &shcam, 1, vis.rain_blocker_shadow_rect);

		renderQueue.init();
		for (size_t i = 0; i < vis.scene->aabb_objects.size(); ++i)
		{
			const AABB& aabb = vis.scene->aabb_objects[i];
			if (aabb.layerMask & vis.layerMask)
			{
				const ObjectComponent& object = vis.scene->objects[i];
				if (object.IsRenderable())
				{
					uint8_t camera_mask = 0;
					if (shcam.frustum.CheckBoxFast(aabb))
					{
						camera_mask |= 1 << 0;
					}
					if (camera_mask == 0)
						continue;

					renderQueue.add(object.mesh_index, uint32_t(i), 0, object.sort_bits, camera_mask);
				}
			}
		}

		if (!renderQueue.empty())
		{
			device->EventBegin("Rain Blocker", cmd);
			const uint cascade = 0;
			XMStoreFloat4x4(&cb.cameras[cascade].view_projection, shcam.view_projection);
			cb.cameras[cascade].output_index = cascade;
			for (int i = 0; i < arraysize(cb.cameras[cascade].frustum.planes); ++i)
			{
				cb.cameras[cascade].frustum.planes[i] = shcam.frustum.planes[i];
			}

			Viewport vp;
			vp.top_left_x = float(vis.rain_blocker_shadow_rect.x + cascade * vis.rain_blocker_shadow_rect.w);
			vp.top_left_y = float(vis.rain_blocker_shadow_rect.y);
			vp.width = float(vis.rain_blocker_shadow_rect.w);
			vp.height = float(vis.rain_blocker_shadow_rect.h);

			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);
			device->BindViewports(1, &vp, cmd);

			Rect scissor;
			scissor.from_viewport(vp);
			device->BindScissorRects(1, &scissor, cmd);

			renderQueue.sort_opaque();
			RenderMeshes(vis, renderQueue, RENDERPASS_RAINBLOCKER, FILTER_OBJECT_ALL, cmd, 0, 1);
			device->EventEnd(cmd);
		}
	}

	device->RenderPassEnd(cmd);

	wi::profiler::EndRange(range_gpu);
	wi::profiler::EndRange(range_cpu);
	device->EventEnd(cmd);
}

void DrawScene(
	const Visibility& vis,
	RENDERPASS renderPass,
	CommandList cmd,
	uint32_t flags
)
{
	const bool opaque = flags & DRAWSCENE_OPAQUE;
	const bool transparent = flags & DRAWSCENE_TRANSPARENT;
	const bool hairparticle = flags & DRAWSCENE_HAIRPARTICLE;
	const bool impostor = flags & DRAWSCENE_IMPOSTOR;
	const bool occlusion = (flags & DRAWSCENE_OCCLUSIONCULLING) && (vis.flags & Visibility::ALLOW_OCCLUSION_CULLING) && GetOcclusionCullingEnabled();
	const bool ocean = flags & DRAWSCENE_OCEAN;
	const bool skip_planar_reflection_objects = flags & DRAWSCENE_SKIP_PLANAR_REFLECTION_OBJECTS;
	const bool foreground = flags & DRAWSCENE_FOREGROUND_ONLY;
	const bool maincamera = flags & DRAWSCENE_MAINCAMERA;

	device->EventBegin("DrawScene", cmd);
	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);

	BindCommonResources(cmd);

	if (ocean && !skip_planar_reflection_objects && vis.scene->weather.IsOceanEnabled() && vis.scene->ocean.IsValid())
	{
		if (!occlusion || !vis.scene->ocean.IsOccluded())
		{
			vis.scene->ocean.Render(*vis.camera, cmd);
		}
	}

	uint32_t filterMask = 0;
	if (opaque)
	{
		filterMask |= FILTER_OPAQUE;
	}
	if (transparent)
	{
		filterMask |= FILTER_TRANSPARENT;
		filterMask |= FILTER_WATER;
	}

	if (IsWireRender())
	{
		filterMask = FILTER_ALL;
	}

	if (opaque || transparent)
	{
		renderQueue.init();
		for (uint32_t instanceIndex : vis.visibleObjects)
		{
			if (occlusion && vis.scene->occlusion_results_objects[instanceIndex].IsOccluded())
				continue;

			const ObjectComponent& object = vis.scene->objects[instanceIndex];
			if (!object.IsRenderable())
				continue;
			if (foreground != object.IsForeground())
				continue;
			if (maincamera && object.IsNotVisibleInMainCamera())
				continue;
			if (skip_planar_reflection_objects && object.IsNotVisibleInReflections())
				continue;
			if ((object.GetFilterMask() & filterMask) == 0)
				continue;

			const float distance = wi::math::Distance(vis.camera->Eye, object.center);
			if (distance > object.fadeDistance + object.radius)
				continue;

			renderQueue.add(object.mesh_index, instanceIndex, distance, object.sort_bits);
		}
		if (!renderQueue.empty())
		{
			if (transparent)
			{
				renderQueue.sort_transparent();
			}
			else
			{
				renderQueue.sort_opaque();
			}
			RenderMeshes(vis, renderQueue, renderPass, filterMask, cmd, flags);
		}
	}

	if (impostor)
	{
		RenderImpostors(vis, renderPass, cmd);
	}

	if (hairparticle)
	{
		if (IsWireRender() || !transparent)
		{
			for (uint32_t hairIndex : vis.visibleHairs)
			{
				const wi::HairParticleSystem& hair = vis.scene->hairs[hairIndex];
				Entity entity = vis.scene->hairs.GetEntity(hairIndex);
				const MaterialComponent& material = *vis.scene->materials.GetComponent(entity);

				hair.Draw(material, renderPass, cmd);
			}
		}
	}

	device->BindShadingRate(ShadingRate::RATE_1X1, cmd);
	device->EventEnd(cmd);

}

void DrawDebugWorld(
	const Scene& scene,
	const CameraComponent& camera,
	const wi::Canvas& canvas,
	CommandList cmd
)
{
	static GPUBuffer wirecubeVB;
	static GPUBuffer wirecubeIB;
	if (!wirecubeVB.IsValid())
	{
		XMFLOAT4 min = XMFLOAT4(-1, -1, -1, 1);
		XMFLOAT4 max = XMFLOAT4(1, 1, 1, 1);

		XMFLOAT4 verts[] = {
			min,							XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,max.y,min.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,max.y,max.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(min.x,min.y,max.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,min.y,min.z,1),	XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,max.y,min.z,1),	XMFLOAT4(1,1,1,1),
			max,							XMFLOAT4(1,1,1,1),
			XMFLOAT4(max.x,min.y,max.z,1),	XMFLOAT4(1,1,1,1),
		};

		GPUBufferDesc bd;
		bd.usage = Usage::DEFAULT;
		bd.size = sizeof(verts);
		bd.bind_flags = BindFlag::VERTEX_BUFFER;
		device->CreateBuffer(&bd, verts, &wirecubeVB);

		uint16_t indices[] = {
			0,1,1,2,0,3,0,4,1,5,4,5,
			5,6,4,7,2,6,3,7,2,3,6,7
		};

		bd.usage = Usage::DEFAULT;
		bd.size = sizeof(indices);
		bd.bind_flags = BindFlag::INDEX_BUFFER;
		device->CreateBuffer(&bd, indices, &wirecubeIB);
	}

	device->EventBegin("DrawDebugWorld", cmd);

	BindCommonResources(cmd);

	for (auto& x : renderableVoxelgrids)
	{
		x->debugdraw(camera.VP, cmd);
	}
	renderableVoxelgrids.clear();

	for (auto& x : renderablePathqueries)
	{
		x->debugdraw(camera.VP, cmd);
	}
	renderablePathqueries.clear();

	for (auto& x : renderableTrails)
	{
		x->Draw(camera, cmd);
	}
	renderableTrails.clear();

	if (debugCameras)
	{
		device->EventBegin("Debug Cameras", cmd);

		static GPUBuffer wirecamVB;
		static GPUBuffer wirecamIB;
		if (!wirecamVB.IsValid())
		{
			XMFLOAT4 verts[] = {
				XMFLOAT4(-0.1f,-0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0.1f,-0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0.1f,0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-0.1f,0.1f,-1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-1,-1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(1,-1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(1,1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(-1,1,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0,1.5f,1,1),	XMFLOAT4(1,1,1,1),
				XMFLOAT4(0,0,-1,1),	XMFLOAT4(1,1,1,1),
			};

			GPUBufferDesc bd;
			bd.usage = Usage::DEFAULT;
			bd.size = sizeof(verts);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, verts, &wirecamVB);

			uint16_t indices[] = {
				0,1,1,2,0,3,0,4,1,5,4,5,
				5,6,4,7,2,6,3,7,2,3,6,7,
				6,8,7,8,
				0,2,1,3
			};

			bd.usage = Usage::DEFAULT;
			bd.size = sizeof(indices);
			bd.bind_flags = BindFlag::INDEX_BUFFER;
			device->CreateBuffer(&bd, indices, &wirecamIB);
		}

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);

		const GPUBuffer* vbs[] = {
			&wirecamVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecamIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);

		for (size_t i = 0; i < scene.cameras.GetCount(); ++i)
		{
			const CameraComponent& cam = scene.cameras[i];

			const float aspect = cam.width / cam.height;
			XMStoreFloat4x4(&sb.g_xTransform, XMMatrixScaling(aspect * 0.5f, 0.5f, 0.5f) * cam.GetInvView() * camera.GetViewProjection());

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(32, 0, 0, cmd);

			if (cam.aperture_size > 0)
			{
				// focal length line:
				RenderableLine linne;
				linne.color_start = linne.color_end = XMFLOAT4(1, 1, 1, 1);
				linne.start = cam.Eye;
				XMVECTOR L = cam.GetEye() + cam.GetAt() * cam.focal_length;
				XMStoreFloat3(&linne.end, L);
				DrawLine(linne);

				// aperture size/shape circle:
				int segmentcount = 36;
				for (int j = 0; j < segmentcount; ++j)
				{
					const float angle0 = float(j) / float(segmentcount) * XM_2PI;
					const float angle1 = float(j + 1) / float(segmentcount) * XM_2PI;
					linne.start = XMFLOAT3(std::sin(angle0), std::cos(angle0), 0);
					linne.end = XMFLOAT3(std::sin(angle1), std::cos(angle1), 0);
					XMVECTOR S = XMLoadFloat3(&linne.start);
					XMVECTOR E = XMLoadFloat3(&linne.end);
					XMMATRIX R = XMLoadFloat3x3(&cam.rotationMatrix);
					XMMATRIX APERTURE = R * XMMatrixScaling(cam.aperture_size * cam.aperture_shape.x, cam.aperture_size * cam.aperture_shape.y, cam.aperture_size);
					S = XMVector3TransformNormal(S, APERTURE);
					E = XMVector3TransformNormal(E, APERTURE);
					S += L;
					E += L;
					XMStoreFloat3(&linne.start, S);
					XMStoreFloat3(&linne.end, E);
					DrawLine(linne);
				}
			}
		}

		device->EventEnd(cmd);
	}

	if (GetToDrawDebugColliders())
	{
		const XMFLOAT4 colliderColor = wi::Color(88, 218, 83, 255);
		for (size_t i = 0; i < scene.colliders.GetCount(); ++i)
		{
			ColliderComponent& collider = scene.colliders[i];
			switch (collider.shape)
			{
			default:
			case ColliderComponent::Shape::Sphere:
				DrawSphere(collider.sphere, colliderColor, false);
				break;
			case ColliderComponent::Shape::Capsule:
				DrawCapsule(collider.capsule, colliderColor, false);
				break;
			case ColliderComponent::Shape::Plane:
				{
					RenderableLine line;
					line.color_start = colliderColor;
					line.color_end = colliderColor;
					XMMATRIX planeMatrix = XMMatrixInverse(nullptr, XMLoadFloat4x4(&collider.plane.projection));
					XMVECTOR P0 = XMVector3Transform(XMVectorSet(-1, 0, -1, 1), planeMatrix);
					XMVECTOR P1 = XMVector3Transform(XMVectorSet(1, 0, -1, 1), planeMatrix);
					XMVECTOR P2 = XMVector3Transform(XMVectorSet(1, 0, 1, 1), planeMatrix);
					XMVECTOR P3 = XMVector3Transform(XMVectorSet(-1, 0, 1, 1), planeMatrix);
					XMStoreFloat3(&line.start, P0);
					XMStoreFloat3(&line.end, P1);
					DrawLine(line, false);
					XMStoreFloat3(&line.start, P1);
					XMStoreFloat3(&line.end, P2);
					DrawLine(line, false);
					XMStoreFloat3(&line.start, P2);
					XMStoreFloat3(&line.end, P3);
					DrawLine(line, false);
					XMStoreFloat3(&line.start, P3);
					XMStoreFloat3(&line.end, P0);
					DrawLine(line, false);
					XMVECTOR O = XMLoadFloat3(&collider.plane.origin);
					XMVECTOR N = XMLoadFloat3(&collider.plane.normal);
					XMStoreFloat3(&line.start, O);
					XMStoreFloat3(&line.end, O + N);
					DrawLine(line, false);
				}
				break;
			}
		}
	}

	if (GetToDrawDebugSprings())
	{
		const XMFLOAT4 springDebugColor = wi::Color(255, 70, 165, 255);
		for (size_t i = 0; i < scene.springs.GetCount(); ++i)
		{
			Entity entity = scene.springs.GetEntity(i);
			const SpringComponent& spring = scene.springs[i];
			const TransformComponent* transform = scene.transforms.GetComponent(entity);
			if (transform != nullptr)
			{
				wi::renderer::RenderableLine line;
				line.start = transform->GetPosition();
				line.end = spring.currentTail;
				line.color_end = line.color_start = springDebugColor;
				wi::renderer::DrawLine(line, false);
			}
			wi::primitive::Sphere sphere;
			sphere.center = spring.currentTail;
			sphere.radius = spring.hitRadius;
			wi::renderer::DrawSphere(sphere, springDebugColor, false);
		}
	}

	if (debugPartitionTree)
	{
		device->EventBegin("DebugPartitionTree", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		MiscCB sb;

		for (size_t i = 0; i < scene.aabb_objects.size(); ++i)
		{
			//const ObjectComponent& object = scene.objects[i];
			//const MeshComponent* mesh = scene.meshes.GetComponent(object.meshID);
			//if (mesh != nullptr && !mesh->bvh_leaf_aabbs.empty())
			//{
			//	const XMMATRIX objectMat = XMLoadFloat4x4(&scene.matrix_objects[i]);
			//	for (const AABB& aabb : mesh->bvh_leaf_aabbs)
			//	{
			//		XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix() * objectMat * camera.GetViewProjection());
			//		sb.g_xColor = XMFLOAT4(1, 1, 0, 1);

			//		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			//		device->DrawIndexed(24, 0, 0, cmd);
			//	}
			//}

			const AABB& aabb = scene.aabb_objects[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 0, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_lights.size(); ++i)
		{
			const AABB& aabb = scene.aabb_lights[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 1, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_decals.size(); ++i)
		{
			const AABB& aabb = scene.aabb_decals[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 0, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		for (size_t i = 0; i < scene.aabb_probes.size(); ++i)
		{
			const AABB& aabb = scene.aabb_probes[i];

			XMStoreFloat4x4(&sb.g_xTransform, aabb.getAsBoxMatrix()*camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(0, 1, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		device->EventEnd(cmd);
	}

	if (debugBoneLines)
	{
		device->EventBegin("DebugBoneLines", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		for (size_t i = 0; i < scene.armatures.GetCount(); ++i)
		{
			const ArmatureComponent& armature = scene.armatures[i];

			if (armature.boneCollection.empty())
			{
				continue;
			}

			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * armature.boneCollection.size(), cmd);

			int j = 0;
			for (Entity entity : armature.boneCollection)
			{
				if (!scene.transforms.Contains(entity))
					continue;
				const TransformComponent& transform = *scene.transforms.GetComponent(entity);
				XMVECTOR a = transform.GetPositionV();
				XMVECTOR b = a + XMVectorSet(0, 1, 0, 0);
				// Search for child to connect bone tip:
				bool child_found = false;
				for (Entity child : armature.boneCollection)
				{
					const HierarchyComponent* hierarchy = scene.hierarchy.GetComponent(child);
					if (hierarchy != nullptr && hierarchy->parentID == entity && scene.transforms.Contains(child))
					{
						const TransformComponent& child_transform = *scene.transforms.GetComponent(child);
						b = child_transform.GetPositionV();
						child_found = true;
						break;
					}
				}
				if (!child_found)
				{
					// No child, try to guess bone tip compared to parent (if it has parent):
					const HierarchyComponent* hierarchy = scene.hierarchy.GetComponent(entity);
					if (hierarchy != nullptr && scene.transforms.Contains(hierarchy->parentID))
					{
						const TransformComponent& parent_transform = *scene.transforms.GetComponent(hierarchy->parentID);
						XMVECTOR ab = a - parent_transform.GetPositionV();
						b = a + ab;
					}
				}

				LineSegment segment;
				XMStoreFloat4(&segment.a, a);
				XMStoreFloat4(&segment.b, b);
				segment.a.w = 1;
				segment.b.w = 1;
				segment.colorA = XMFLOAT4(1, 1, 1, 1);
				segment.colorB = XMFLOAT4(1, 0, 1, 1);

				memcpy((void*)((size_t)mem.data + j * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				j++;
			}

			const GPUBuffer* vbs[] = {
				&mem.buffer
			};
			const uint32_t strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(2 * j, 0, cmd);

		}

		device->EventEnd(cmd);
	}

	if(
		!renderableTriangles_solid.empty() ||
		!renderableTriangles_wireframe.empty() ||
		!renderableTriangles_solid_depth.empty() ||
		!renderableTriangles_wireframe_depth.empty()
		)
	{
		device->EventBegin("Debug Triangles", cmd);

		auto draw_triangle = [&](wi::vector<RenderableTriangle>& dbg) {
			if (dbg.empty())
				return;
			MiscCB sb;
			XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			struct Vertex
			{
				XMFLOAT4 position;
				XMFLOAT4 color;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(Vertex) * dbg.size() * 3, cmd);

			int i = 0;
			for (auto& triangle : dbg)
			{
				Vertex vertices[3];
				vertices[0].position = XMFLOAT4(triangle.positionA.x, triangle.positionA.y, triangle.positionA.z, 1);
				vertices[0].color = triangle.colorA;
				vertices[1].position = XMFLOAT4(triangle.positionB.x, triangle.positionB.y, triangle.positionB.z, 1);
				vertices[1].color = triangle.colorB;
				vertices[2].position = XMFLOAT4(triangle.positionC.x, triangle.positionC.y, triangle.positionC.z, 1);
				vertices[2].color = triangle.colorC;

				memcpy((void*)((size_t)mem.data + i * sizeof(vertices)), vertices, sizeof(vertices));
				i++;
			}

			const GPUBuffer* vbs[] = {
				&mem.buffer,
			};
			const uint32_t strides[] = {
				sizeof(Vertex),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(3 * i, 0, cmd);

			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_SOLID], cmd);
		draw_triangle(renderableTriangles_solid);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_WIREFRAME], cmd);
		draw_triangle(renderableTriangles_wireframe);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_SOLID_DEPTH], cmd);
		draw_triangle(renderableTriangles_solid_depth);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_TRIANGLE_WIREFRAME_DEPTH], cmd);
		draw_triangle(renderableTriangles_wireframe_depth);

		device->EventEnd(cmd);
	}

	if(!renderableLines.empty() || !renderableLines_depth.empty())
	{
		device->EventBegin("Debug Lines - 3D", cmd);

		auto draw_line = [&](wi::vector<RenderableLine>& dbg) {
			if (dbg.empty())
				return;
			MiscCB sb;
			XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * dbg.size(), cmd);

			int i = 0;
			for (auto& line : dbg)
			{
				LineSegment segment;
				segment.a = XMFLOAT4(line.start.x, line.start.y, line.start.z, 1);
				segment.b = XMFLOAT4(line.end.x, line.end.y, line.end.z, 1);
				segment.colorA = line.color_start;
				segment.colorB = line.color_end;

				memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				i++;
			}

			const GPUBuffer* vbs[] = {
				&mem.buffer,
			};
			const uint32_t strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(2 * i, 0, cmd);

			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);
		draw_line(renderableLines);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES_DEPTH], cmd);
		draw_line(renderableLines_depth);

		device->EventEnd(cmd);
	}

	// GPU-generated indirect debug lines:
	{
		device->EventBegin("Indirect Debug Lines - 3D", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		const GPUBuffer* vbs[] = {
			&buffers[BUFFERTYPE_INDIRECT_DEBUG_1],
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			sizeof(IndirectDrawArgsInstanced),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->DrawInstancedIndirect(&buffers[BUFFERTYPE_INDIRECT_DEBUG_1], 0, cmd);

		device->EventEnd(cmd);
	}

	if (!renderableLines2D.empty())
	{
		device->EventBegin("Debug Lines - 2D", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, canvas.GetProjection());
		sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		struct LineSegment
		{
			XMFLOAT4 a, colorA, b, colorB;
		};
		GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * renderableLines2D.size(), cmd);

		int i = 0;
		for (auto& line : renderableLines2D)
		{
			LineSegment segment;
			segment.a = XMFLOAT4(line.start.x, line.start.y, 0, 1);
			segment.b = XMFLOAT4(line.end.x, line.end.y, 0, 1);
			segment.colorA = line.color_start;
			segment.colorB = line.color_end;

			memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
			i++;
		}

		const GPUBuffer* vbs[] = {
			&mem.buffer,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		const uint64_t offsets[] = {
			mem.offset,
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

		device->Draw(2 * i, 0, cmd);

		renderableLines2D.clear();

		device->EventEnd(cmd);
	}

	if(!renderablePoints.empty() || !renderablePoints_depth.empty())
	{
		device->EventBegin("Debug Points", cmd);

		auto draw_point = [&](wi::vector<RenderablePoint>& dbg) {
			if (dbg.empty())
				return;
			MiscCB sb;
			XMStoreFloat4x4(&sb.g_xTransform, camera.GetProjection()); // only projection, we will expand in view space on CPU below to be camera facing!
			sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			// Will generate 2 line segments for each point forming a cross section:
			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * dbg.size() * 2, cmd);

			XMMATRIX V = camera.GetView();

			int i = 0;
			for (auto& point : dbg)
			{
				LineSegment segment;
				segment.colorA = segment.colorB = point.color;

				// the cross section will be transformed to view space and expanded here:
				XMVECTOR _c = XMLoadFloat3(&point.position);
				_c = XMVector3Transform(_c, V);

				XMVECTOR _a = _c + XMVectorSet(-1, -1, 0, 0) * point.size;
				XMVECTOR _b = _c + XMVectorSet(1, 1, 0, 0) * point.size;
				XMStoreFloat4(&segment.a, _a);
				XMStoreFloat4(&segment.b, _b);
				memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				i++;

				_a = _c + XMVectorSet(-1, 1, 0, 0) * point.size;
				_b = _c + XMVectorSet(1, -1, 0, 0) * point.size;
				XMStoreFloat4(&segment.a, _a);
				XMStoreFloat4(&segment.b, _b);
				memcpy((void*)((size_t)mem.data + i * sizeof(LineSegment)), &segment, sizeof(LineSegment));
				i++;
			}

			const GPUBuffer* vbs[] = {
				&mem.buffer,
			};
			const uint32_t strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(2 * i, 0, cmd);

			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES], cmd);
		draw_point(renderablePoints);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_LINES_DEPTH], cmd);
		draw_point(renderablePoints_depth);

		device->EventEnd(cmd);
	}

	if(!renderableBoxes.empty() || !renderableBoxes_depth.empty())
	{
		device->EventBegin("Debug Boxes", cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		auto draw_box = [&](wi::vector<std::pair<XMFLOAT4X4, XMFLOAT4>>& dbg) {
			if (dbg.empty())
				return;

			MiscCB sb;

			for (auto& x : dbg)
			{
				XMStoreFloat4x4(&sb.g_xTransform, XMLoadFloat4x4(&x.first) * camera.GetViewProjection());
				sb.g_xColor = x.second;

				device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

				device->DrawIndexed(24, 0, 0, cmd);
			}
			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);
		draw_box(renderableBoxes);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);
		draw_box(renderableBoxes_depth);

		device->EventEnd(cmd);
	}

	if (!renderableSpheres.empty() || !renderableSpheres_depth.empty())
	{
		device->EventBegin("Debug Spheres", cmd);

		static GPUBuffer wiresphereVB;
		static GPUBuffer wiresphereIB;
		if (!wiresphereVB.IsValid())
		{
			struct Vertex
			{
				XMFLOAT4 position;
				XMFLOAT4 color;
			};
			wi::vector<Vertex> vertices;

			const int segmentcount = 36;
			Vertex vert;
			vert.color = XMFLOAT4(1, 1, 1, 1);

			// XY
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(sinf(angle0), cosf(angle0), 0, 1);
				vertices.push_back(vert);
			}
			// XZ
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(sinf(angle0), 0, cosf(angle0), 1);
				vertices.push_back(vert);
			}
			// YZ
			for (int i = 0; i <= segmentcount; ++i)
			{
				const float angle0 = (float)i / (float)segmentcount * XM_2PI;
				vert.position = XMFLOAT4(0, sinf(angle0), cosf(angle0), 1);
				vertices.push_back(vert);
			}

			GPUBufferDesc bd;
			bd.usage = Usage::DEFAULT;
			bd.size = vertices.size() * sizeof(Vertex);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, vertices.data(), &wiresphereVB);

			wi::vector<uint16_t> indices;
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i));
				indices.push_back(uint16_t(i + 1));
			}
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i + segmentcount + 1));
				indices.push_back(uint16_t(i + segmentcount + 1 + 1));
			}
			for (int i = 0; i < segmentcount; ++i)
			{
				indices.push_back(uint16_t(i + (segmentcount + 1) * 2));
				indices.push_back(uint16_t(i + (segmentcount + 1) * 2 + 1));
			}

			bd.usage = Usage::DEFAULT;
			bd.size = indices.size() * sizeof(uint16_t);
			bd.bind_flags = BindFlag::INDEX_BUFFER;
			device->CreateBuffer(&bd, indices.data(), &wiresphereIB);
		}

		const GPUBuffer* vbs[] = {
			&wiresphereVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wiresphereIB, IndexBufferFormat::UINT16, 0, cmd);

		auto draw_sphere = [&](wi::vector<std::pair<Sphere, XMFLOAT4>>& dbg) {
			if (dbg.empty())
				return;

			MiscCB sb;
			for (auto& x : dbg)
			{
				const Sphere& sphere = x.first;
				XMStoreFloat4x4(&sb.g_xTransform,
					XMMatrixScaling(sphere.radius, sphere.radius, sphere.radius) *
					XMMatrixTranslation(sphere.center.x, sphere.center.y, sphere.center.z) *
					camera.GetViewProjection()
				);
				sb.g_xColor = x.second;

				device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

				device->DrawIndexed((uint32_t)(wiresphereIB.GetDesc().size / sizeof(uint16_t)), 0, 0, cmd);
			}
			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);
		draw_sphere(renderableSpheres);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);
		draw_sphere(renderableSpheres_depth);

		device->EventEnd(cmd);
	}

	if (!renderableCapsules.empty() || !renderableCapsules_depth.empty())
	{
		device->EventBegin("Debug Capsules", cmd);

		auto draw_capsule = [&](wi::vector<std::pair<Capsule, XMFLOAT4>>& dbg) {
			if (dbg.empty())
				return;
			MiscCB sb;
			XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(1, 1, 1, 1);
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			const int segmentcount = 18 + 1 + 18 + 1;
			const int linecount = (int)dbg.size() * segmentcount;

			struct LineSegment
			{
				XMFLOAT4 a, colorA, b, colorB;
			};
			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(LineSegment) * (size_t)linecount, cmd);

			XMVECTOR Eye = camera.GetEye();
			XMVECTOR Unit = XMVectorSet(0, 1, 0, 0);

			LineSegment* linearray = (LineSegment*)mem.data;
			int j = 0;
			for (auto& it : dbg)
			{
				const Capsule& capsule = it.first;
				const float radius = capsule.radius;

				XMVECTOR Base = XMLoadFloat3(&capsule.base);
				XMVECTOR Tip = XMLoadFloat3(&capsule.tip);
				XMVECTOR Radius = XMVectorReplicate(capsule.radius);
				XMVECTOR Normal = XMVector3Normalize(Tip - Base);
				XMVECTOR Tangent = XMVector3Normalize(XMVector3Cross(Normal, Base - Eye));
				XMVECTOR Binormal = XMVector3Normalize(XMVector3Cross(Tangent, Normal));
				XMVECTOR LineEndOffset = Normal * Radius;
				XMVECTOR A = Base + LineEndOffset;
				XMVECTOR B = Tip - LineEndOffset;
				XMVECTOR AB = Unit * XMVector3Length(B - A);
				XMMATRIX M = { Tangent,Normal,Binormal,XMVectorSetW(A, 1) };

				for (int i = 0; i < segmentcount; i += 1)
				{
					const float angle0 = XM_PIDIV2 + (float)i / (float)segmentcount * XM_2PI;
					const float angle1 = XM_PIDIV2 + (float)(i + 1) / (float)segmentcount * XM_2PI;
					XMVECTOR a, b;
					if (i < 18)
					{

						a = XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
						b = XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
					}
					else if (i == 18)
					{
						a = XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
						b = AB + XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
					}
					else if (i > 18 && i < 18 + 1 + 18)
					{
						a = AB + XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
						b = AB + XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
					}
					else
					{
						a = AB + XMVectorSet(sinf(angle0) * radius, cosf(angle0) * radius, 0, 1);
						b = XMVectorSet(sinf(angle1) * radius, cosf(angle1) * radius, 0, 1);
					}
					a = XMVector3Transform(a, M);
					b = XMVector3Transform(b, M);
					LineSegment line;
					XMStoreFloat4(&line.a, a);
					XMStoreFloat4(&line.b, b);
					line.colorA = line.colorB = it.second;
					std::memcpy(linearray + j, &line, sizeof(line));
					j++;
				}

			}

			const GPUBuffer* vbs[] = {
				&mem.buffer,
			};
			const uint32_t strides[] = {
				sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
			};
			const uint64_t offsets[] = {
				mem.offset,
			};
			device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, offsets, cmd);

			device->Draw(linecount * 2, 0, cmd);

			dbg.clear();
		};

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE], cmd);
		draw_capsule(renderableCapsules);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);
		draw_capsule(renderableCapsules_depth);

		device->EventEnd(cmd);
	}

	if (debugEnvProbes)
	{
		device->EventBegin("Debug EnvProbes", cmd);
		// Envmap spheres:

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_ENVPROBE], cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = scene.probes[i];

			XMStoreFloat4x4(&sb.g_xTransform, XMMatrixTranslationFromVector(XMLoadFloat3(&probe.position)));
			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->BindResource(&probe.texture, 0, cmd);

			device->Draw(vertexCount_uvsphere, 0, cmd);
		}


		// Local proxy boxes:

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);

		const GPUBuffer* vbs[] = {
			&wirecubeVB,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);

		for (size_t i = 0; i < scene.probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = scene.probes[i];
			Entity entity = scene.probes.GetEntity(i);

			if (!scene.transforms.Contains(entity))
			{
				continue;
			}

			const TransformComponent& transform = *scene.transforms.GetComponent(entity);

			XMStoreFloat4x4(&sb.g_xTransform, XMLoadFloat4x4(&transform.world)*camera.GetViewProjection());
			sb.g_xColor = float4(0, 1, 1, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			device->DrawIndexed(24, 0, 0, cmd);
		}

		device->EventEnd(cmd);
	}


	if (GetDDGIDebugEnabled() && scene.ddgi.probe_buffer.IsValid())
	{
		device->EventBegin("Debug DDGI", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_DDGI], cmd);
		device->DrawInstanced(2880, scene.shaderscene.ddgi.probe_count, 0, 0, cmd); // uv-sphere

		device->EventEnd(cmd);
	}


	if (gridHelper)
	{
		device->EventBegin("GridHelper", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_GRID], cmd);

		static float alpha = 0.75f;
		const float channel_min = 0.2f;
		static uint32_t gridVertexCount = 0;
		static GPUBuffer grid;
		if (!grid.IsValid())
		{
			const float h = 0.01f; // avoid z-fight with zero plane
			const int a = 20;
			XMFLOAT4 verts[((a + 1) * 2 + (a + 1) * 2) * 2];

			int count = 0;
			for (int i = 0; i <= a; ++i)
			{
				verts[count++] = XMFLOAT4(i - a * 0.5f, h, -a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(channel_min, channel_min, 1, alpha) : XMFLOAT4(1, 1, 1, alpha));

				verts[count++] = XMFLOAT4(i - a * 0.5f, h, +a * 0.5f, 1);
				verts[count++] = (i == a / 2 ? XMFLOAT4(channel_min, channel_min, 1, alpha) : XMFLOAT4(1, 1, 1, alpha));
			}
			for (int j = 0; j <= a; ++j)
			{
				verts[count++] = XMFLOAT4(-a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, channel_min, channel_min, alpha) : XMFLOAT4(1, 1, 1, alpha));

				verts[count++] = XMFLOAT4(+a * 0.5f, h, j - a * 0.5f, 1);
				verts[count++] = (j == a / 2 ? XMFLOAT4(1, channel_min, channel_min, alpha) : XMFLOAT4(1, 1, 1, alpha));
			}

			gridVertexCount = arraysize(verts) / 2;

			GPUBufferDesc bd;
			bd.size = sizeof(verts);
			bd.bind_flags = BindFlag::VERTEX_BUFFER;
			device->CreateBuffer(&bd, verts, &grid);
		}

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = float4(1, 1, 1, 1);

		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		const GPUBuffer* vbs[] = {
			&grid,
		};
		const uint32_t strides[] = {
			sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
		};
		device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
		device->Draw(gridVertexCount, 0, cmd);

		device->EventEnd(cmd);
	}

	if (VXGI_DEBUG && scene.vxgi.radiance.IsValid())
	{
		device->EventBegin("Debug Voxels", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_VOXEL], cmd);

		MiscCB sb;
		XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
		sb.g_xColor = float4((float)VXGI_DEBUG_CLIPMAP, 1, 1, 1);

		device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

		uint32_t vertexCount = scene.vxgi.res * scene.vxgi.res * scene.vxgi.res;
		if (VXGI_DEBUG_CLIPMAP == VXGI_CLIPMAP_COUNT)
		{
			vertexCount *= VXGI_CLIPMAP_COUNT;
		}
		device->Draw(vertexCount, 0, cmd);

		device->EventEnd(cmd);
	}

	if (debugEmitters)
	{
		device->EventBegin("DebugEmitters", cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.emitters.GetCount(); ++i)
		{
			const wi::EmittedParticleSystem& emitter = scene.emitters[i];
			Entity entity = scene.emitters.GetEntity(i);
			const MeshComponent* mesh = scene.meshes.GetComponent(emitter.meshID);

			XMMATRIX W = XMLoadFloat4x4(&emitter.worldMatrix) * camera.GetViewProjection();
			if (mesh != nullptr && IsFormatUnorm(mesh->position_format) && !mesh->so_pos.IsValid())
			{
				XMMATRIX R = mesh->aabb.getUnormRemapMatrix();
				W = R * W;
			}

			XMStoreFloat4x4(&sb.g_xTransform, W);
			sb.g_xColor = float4(0, 1, 0, 1);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			if (mesh == nullptr)
			{
				// No mesh, just draw a box:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_CUBE_DEPTH], cmd);
				const GPUBuffer* vbs[] = {
					&wirecubeVB,
				};
				const uint32_t strides[] = {
					sizeof(XMFLOAT4) + sizeof(XMFLOAT4),
				};
				device->BindVertexBuffers(vbs, 0, arraysize(vbs), strides, nullptr, cmd);
				device->BindIndexBuffer(&wirecubeIB, IndexBufferFormat::UINT16, 0, cmd);
				device->DrawIndexed(24, 0, 0, cmd);
			}
			else
			{
				// Draw mesh wireframe:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_EMITTER], cmd);
				DebugObjectPushConstants push;
				push.vb_pos_wind = mesh->so_pos.IsValid() ? mesh->so_pos.descriptor_srv : mesh->vb_pos_wind.descriptor_srv;
				device->PushConstants(&push, sizeof(push), cmd);
				device->BindIndexBuffer(&mesh->generalBuffer, mesh->GetIndexFormat(), mesh->ib.offset, cmd);

				device->DrawIndexed((uint32_t)mesh->indices.size(), 0, 0, cmd);
			}
		}

		device->EventEnd(cmd);
	}

	if (!paintrads.empty())
	{
		device->EventBegin("Paint Radiuses", cmd);

		device->BindPipelineState(&PSO_debug[DEBUGRENDERING_PAINTRADIUS], cmd);

		for (auto& x : paintrads)
		{
			if (!scene.transforms.Contains(x.objectEntity) ||
				!scene.objects.Contains(x.objectEntity)
				)
			{
				continue;
			}
			const ObjectComponent& object = *scene.objects.GetComponent(x.objectEntity);
			if (scene.meshes.GetCount() < object.mesh_index)
			{
				continue;
			}
			const MeshComponent& mesh = scene.meshes[object.mesh_index];
			const MeshComponent::MeshSubset& subset = mesh.subsets[x.subset];
			if (!scene.materials.Contains(subset.materialID))
			{
				continue;
			}

			GraphicsDevice::GPUAllocation mem = device->AllocateGPU(sizeof(ShaderMeshInstancePointer), cmd);
			ShaderMeshInstancePointer poi;
			poi.Create((uint)scene.objects.GetIndex(x.objectEntity));
			std::memcpy(mem.data, &poi, sizeof(poi));

			device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

			PaintRadiusCB cb;
			cb.xPaintRadResolution = x.dimensions;
			cb.xPaintRadCenter = x.center;
			cb.xPaintRadRadius = x.radius;
			cb.xPaintRadUVSET = x.uvset;
			cb.xPaintRadBrushRotation = x.rotation;
			cb.xPaintRadBrushShape = x.shape;
			device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(PaintRadiusCB), cmd);

			ObjectPushConstants push;
			push.geometryIndex = mesh.geometryOffset + x.subset;
			push.materialIndex = subset.materialIndex;
			push.instances = device->GetDescriptorIndex(&mem.buffer, SubresourceType::SRV);
			push.instance_offset = (uint)mem.offset;
			device->PushConstants(&push, sizeof(push), cmd);

			device->DrawIndexedInstanced(subset.indexCount, 1, subset.indexOffset, 0, 0, cmd);
		}

		paintrads.clear();

		device->EventEnd(cmd);
	}


	if (debugForceFields)
	{
		device->EventBegin("DebugForceFields", cmd);

		MiscCB sb;
		for (size_t i = 0; i < scene.forces.GetCount(); ++i)
		{
			const ForceFieldComponent& force = scene.forces[i];

			XMStoreFloat4x4(&sb.g_xTransform, camera.GetViewProjection());
			sb.g_xColor = XMFLOAT4(camera.Eye.x, camera.Eye.y, camera.Eye.z, (float)i);

			device->BindDynamicConstantBuffer(sb, CB_GETBINDSLOT(MiscCB), cmd);

			switch (force.type)
			{
			case ForceFieldComponent::Type::Point:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_FORCEFIELD_POINT], cmd);
				device->Draw(2880, 0, cmd); // uv-sphere
				break;
			case ForceFieldComponent::Type::Plane:
				device->BindPipelineState(&PSO_debug[DEBUGRENDERING_FORCEFIELD_PLANE], cmd);
				device->Draw(14, 0, cmd); // box
				break;
			}
		}

		device->EventEnd(cmd);
	}

	if (GetRaytraceDebugBVHVisualizerEnabled())
	{
		RayTraceSceneBVH(scene, cmd);
	}

	if (!debugTextStorage.empty())
	{
		device->EventBegin("DebugTexts", cmd);
		const XMMATRIX VP = camera.GetViewProjection();
		const XMMATRIX R = XMLoadFloat3x3(&camera.rotationMatrix);
		struct DebugTextSorter
		{
			const char* text;
			size_t text_len;
			DebugTextParams params;
			float distance;
		};
		static thread_local wi::vector<DebugTextSorter> sorted_texts;
		sorted_texts.clear();
		size_t offset = 0;
		while(offset < debugTextStorage.size())
		{
			auto& x = sorted_texts.emplace_back();
			x.params = *(const DebugTextParams*)(debugTextStorage.data() + offset);
			offset += sizeof(DebugTextParams);
			x.text = (const char*)(debugTextStorage.data() + offset);
			x.text_len = strlen(x.text);
			offset += x.text_len + 1;
			x.distance = wi::math::Distance(x.params.position, camera.Eye);

		}
		std::sort(sorted_texts.begin(), sorted_texts.end(), [](const DebugTextSorter& a, const DebugTextSorter& b) {
			return a.distance > b.distance;
			});
		for (auto& x : sorted_texts)
		{
			wi::font::Params params;
			params.position = x.params.position;
			params.size = x.params.pixel_height;
			params.scaling = 1.0f / params.size * x.params.scaling;
			params.color = wi::Color::fromFloat4(x.params.color);
			params.intensity = x.params.color.w > 1 ? x.params.color.w : 1;
			params.h_align = wi::font::WIFALIGN_CENTER;
			params.v_align = wi::font::WIFALIGN_CENTER;
			params.softness = 0.0f;
			params.shadowColor = wi::Color::Black();
			params.shadow_softness = 0.8f;
			params.customProjection = &VP;
			if (x.params.flags & DebugTextParams::DEPTH_TEST)
			{
				params.enableDepthTest();
			}
			if (x.params.flags & DebugTextParams::CAMERA_FACING)
			{
				params.customRotation = &R;
			}
			if (x.params.flags & DebugTextParams::CAMERA_SCALING)
			{
				params.scaling *= x.distance * 0.05f;
			}
			wi::font::Draw(x.text, x.text_len, params, cmd);
		}
		debugTextStorage.clear();
		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}


void ComputeVolumetricCloudShadows(
	CommandList cmd,
	const Texture* weatherMapFirst,
	const Texture* weatherMapSecond)
{
	device->EventBegin("RenderVolumetricCloudShadows", cmd);
	auto range = wi::profiler::BeginRangeGPU("Volumetric Clouds Shadow", cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].GetDesc();
	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Cloud shadow render pass:
	{
		device->EventBegin("Volumetric Cloud Rendering Shadow", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_SHADOW_RENDER], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&texture_shapeNoise, 0, cmd);
		device->BindResource(&texture_detailNoise, 1, cmd);
		device->BindResource(&texture_curlNoise, 2, cmd);

		if (weatherMapFirst != nullptr)
		{
			device->BindResource(weatherMapFirst, 3, cmd);
		}
		else
		{
			device->BindResource(&texture_weatherMap, 3, cmd);
		}

		if (weatherMapSecond != nullptr)
		{
			device->BindResource(weatherMapSecond, 4, cmd);
		}
		else
		{
			device->BindResource(&texture_weatherMap, 4, cmd);
		}

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW], textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	Postprocess_Blur_Gaussian(
		textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW],
		textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW_GAUSSIAN_TEMP],
		textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW],
		cmd,
		-1, -1,
		false // wide
	);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void ComputeSkyAtmosphereTextures(CommandList cmd)
{
	device->EventBegin("ComputeSkyAtmosphereTextures", cmd);
	auto range = wi::profiler::BeginRangeGPU("SkyAtmosphere Textures", cmd);

	BindCommonResources(cmd);

	// Transmittance Lut pass:
	{
		device->EventBegin("TransmittanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_TRANSMITTANCELUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd); // empty
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd); // empty
		device->BindResource(&textures[TEXTYPE_2D_VOLUMETRICCLOUDS_SHADOW], 2, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int threadSize = 8;
		const int transmittanceLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].GetDesc().width;
		const int transmittanceLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].GetDesc().height;
		const int transmittanceLutThreadX = static_cast<uint32_t>(std::ceil(transmittanceLutWidth / threadSize));
		const int transmittanceLutThreadY = static_cast<uint32_t>(std::ceil(transmittanceLutHeight / threadSize));

		device->Dispatch(transmittanceLutThreadX, transmittanceLutThreadY, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// MultiScattered Luminance Lut pass:
	{
		device->EventBegin("MultiScatteredLuminanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], cmd);

		// Use transmittance from previous pass
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd); // empty

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int multiScatteredLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].GetDesc().width;
		const int multiScatteredLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].GetDesc().height;

		device->Dispatch(multiScatteredLutWidth, multiScatteredLutHeight, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Environment Luminance Lut pass:
	{
		device->EventBegin("EnvironmentLuminanceLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_SKYLUMINANCELUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int environmentLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].GetDesc().width;
		const int environmentLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].GetDesc().height;

		device->Dispatch(environmentLutWidth, environmentLutHeight, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_SKYLUMINANCELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);

	wi::profiler::EndRange(range);
}
void ComputeSkyAtmosphereSkyViewLut(CommandList cmd)
{
	const int threadSize = 8;
	const int skyViewLutWidth = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].GetDesc().width;
	const int skyViewLutHeight = textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].GetDesc().height;
	const int skyViewLutThreadX = static_cast<uint32_t>(std::ceil(skyViewLutWidth / threadSize));
	const int skyViewLutThreadY = static_cast<uint32_t>(std::ceil(skyViewLutHeight / threadSize));
	if (skyViewLutThreadX * skyViewLutThreadY < 1)
		return;

	device->EventBegin("ComputeSkyAtmosphereSkyViewLut", cmd);

	BindCommonResources(cmd);

	// Sky View Lut pass:
	{
		device->EventBegin("SkyViewLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_SKYVIEWLUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(skyViewLutThreadX, skyViewLutThreadY, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_2D_SKYATMOSPHERE_SKYVIEWLUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}
void ComputeSkyAtmosphereCameraVolumeLut(CommandList cmd)
{
	device->EventBegin("ComputeSkyAtmosphereCameraVolumeLut", cmd);

	BindCommonResources(cmd);
	
	// Camera Volume Lut pass:
	{
		device->EventBegin("CameraVolumeLut", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SKYATMOSPHERE_CAMERAVOLUMELUT], cmd);

		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_TRANSMITTANCELUT], 0, cmd);
		device->BindResource(&textures[TEXTYPE_2D_SKYATMOSPHERE_MULTISCATTEREDLUMINANCELUT], 1, cmd);

		const GPUResource* uavs[] = {
			&textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT], textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT].desc.layout, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const int threadSize = 8;
		const int cameraVolumeLutWidth = textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT].GetDesc().width;
		const int cameraVolumeLutHeight = textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT].GetDesc().height;
		const int cameraVolumeLutDepth = textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT].GetDesc().depth;
		const int cameraVolumeLutThreadX = static_cast<uint32_t>(std::ceil(cameraVolumeLutWidth / threadSize));
		const int cameraVolumeLutThreadY = static_cast<uint32_t>(std::ceil(cameraVolumeLutHeight / threadSize));
		const int cameraVolumeLutThreadZ = static_cast<uint32_t>(std::ceil(cameraVolumeLutDepth / threadSize));

		device->Dispatch(cameraVolumeLutThreadX, cameraVolumeLutThreadY, cameraVolumeLutThreadZ, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT], ResourceState::UNORDERED_ACCESS, textures[TEXTYPE_3D_SKYATMOSPHERE_CAMERAVOLUMELUT].desc.layout)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	device->EventEnd(cmd);
}

void DrawSky(const Scene& scene, CommandList cmd)
{
	device->EventBegin("DrawSky", cmd);
	
	if (scene.weather.skyMap.IsValid())
	{
		device->BindPipelineState(&PSO_sky[SKYRENDERING_STATIC], cmd);
	}
	else
	{
		device->BindPipelineState(&PSO_sky[SKYRENDERING_DYNAMIC], cmd);
	}

	BindCommonResources(cmd);

	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}
void DrawSun(CommandList cmd)
{
	device->EventBegin("DrawSun", cmd);

	device->BindPipelineState(&PSO_sky[SKYRENDERING_SUN], cmd);

	BindCommonResources(cmd);

	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}


void RefreshEnvProbes(const Visibility& vis, CommandList cmd)
{
	device->EventBegin("EnvironmentProbe Refresh", cmd);
	auto range = wi::profiler::BeginRangeGPU("Environment Probe Refresh", cmd);

	BindCommonResources(cmd);

	const float zNearP = vis.camera->zNearP;
	const float zFarP = vis.camera->zFarP;
	const float zNearPRcp = 1.0f / zNearP;
	const float zFarPRcp = 1.0f / zFarP;

	auto render_probe = [&](const EnvironmentProbeComponent& probe, const AABB& probe_aabb) {

		Viewport vp;
		vp.height = vp.width = (float)probe.texture.desc.width;
		device->BindViewports(1, &vp, cmd);

		SHCAM cameras[6];
		CreateCubemapCameras(probe.position, zNearP, zFarP, cameras, arraysize(cameras));

		CameraCB cb;
		cb.init();
		for (uint32_t i = 0; i < arraysize(cameras); ++i)
		{
			XMStoreFloat4x4(&cb.cameras[i].view_projection, cameras[i].view_projection);
			XMMATRIX invVP = XMMatrixInverse(nullptr, cameras[i].view_projection);
			XMStoreFloat4x4(&cb.cameras[i].inverse_view_projection, invVP);
			cb.cameras[i].position = probe.position;
			cb.cameras[i].output_index = i;
			cb.cameras[i].z_near = zNearP;
			cb.cameras[i].z_near_rcp = zNearPRcp;
			cb.cameras[i].z_far = zFarP;
			cb.cameras[i].z_far_rcp = zFarPRcp;
			cb.cameras[i].z_range = abs(zFarP - zNearP);
			cb.cameras[i].z_range_rcp = 1.0f / std::max(0.0001f, cb.cameras[i].z_range);
			cb.cameras[i].internal_resolution = uint2(probe.texture.desc.width, probe.texture.desc.height);
			cb.cameras[i].internal_resolution_rcp.x = 1.0f / cb.cameras[i].internal_resolution.x;
			cb.cameras[i].internal_resolution_rcp.y = 1.0f / cb.cameras[i].internal_resolution.y;
			cb.cameras[i].sample_count = probe.GetSampleCount();

			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersNEAR[0], XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersNEAR[1], XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersNEAR[2], XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersNEAR[3], XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersFAR[0], XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersFAR[1], XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersFAR[2], XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), invVP));
			XMStoreFloat4(&cb.cameras[i].frustum_corners.cornersFAR[3], XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), invVP));

		}
		device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

		if (vis.scene->weather.IsRealisticSky())
		{
			// Update skyatmosphere textures, since each probe has different positions
			ComputeSkyAtmosphereSkyViewLut(cmd);
		}

		Texture envrenderingDepthBuffer;
		Texture envrenderingColorBuffer_MSAA;
		Texture envrenderingColorBuffer;
		Texture envrenderingColorBuffer_Filtered;

		// Find temporary render textures to fit request, or create new ones if they don't exist:
		union RenderTextureID
		{
			struct
			{
				uint32_t width : 16;
				uint32_t sample_count : 3;
				uint32_t is_depth : 1;
				uint32_t is_filtered : 1;
			} bits;
			uint32_t raw;
		};
		static wi::unordered_map<uint32_t, Texture> render_textures;
		static std::mutex locker;
		{
			const uint32_t required_sample_count = probe.GetSampleCount();

			std::scoped_lock lck(locker);
			RenderTextureID id_depth = {};
			id_depth.bits.width = probe.resolution;
			id_depth.bits.sample_count = required_sample_count;
			id_depth.bits.is_depth = 1;
			id_depth.bits.is_filtered = 0;
			envrenderingDepthBuffer = render_textures[id_depth.raw];

			RenderTextureID id_color = {};
			id_color.bits.width = probe.resolution;
			id_color.bits.sample_count = 1;
			id_color.bits.is_depth = 0;
			id_color.bits.is_filtered = 0;
			envrenderingColorBuffer = render_textures[id_color.raw];

			RenderTextureID id_color_filtered = {};
			id_color_filtered.bits.width = probe.resolution;
			id_color_filtered.bits.sample_count = 1;
			id_color_filtered.bits.is_depth = 0;
			id_color_filtered.bits.is_filtered = 1;
			envrenderingColorBuffer_Filtered = render_textures[id_color_filtered.raw];

			TextureDesc desc;
			desc.array_size = 6;
			desc.height = probe.resolution;
			desc.width = probe.resolution;
			desc.usage = Usage::DEFAULT;

			if (!envrenderingDepthBuffer.IsValid())
			{
				desc.mip_levels = 1;
				desc.bind_flags = BindFlag::DEPTH_STENCIL | BindFlag::SHADER_RESOURCE;
				desc.format = format_depthbuffer_envprobe;
				desc.layout = ResourceState::SHADER_RESOURCE;
				desc.sample_count = required_sample_count;
				if (required_sample_count == 1)
				{
					desc.misc_flags = ResourceMiscFlag::TEXTURECUBE;
				}
				device->CreateTexture(&desc, nullptr, &envrenderingDepthBuffer);
				device->SetName(&envrenderingDepthBuffer, "envrenderingDepthBuffer");
				render_textures[id_depth.raw] = envrenderingDepthBuffer;

				std::string info;
				info += "Created envprobe depth buffer for request";
				info += "\n\tResolution = " + std::to_string(desc.width) + " * " + std::to_string(desc.height) + " * 6";
				info += "\n\tSample Count = " + std::to_string(desc.sample_count);
				info += "\n\tMip Levels = " + std::to_string(desc.mip_levels);
				info += "\n\tFormat = ";
				info += GetFormatString(desc.format);
				info += "\n\tMemory = " + wi::helper::GetMemorySizeText(ComputeTextureMemorySizeInBytes(desc)) + "\n";
				wi::backlog::post(info);
			}

			if (!envrenderingColorBuffer.IsValid())
			{
				desc.mip_levels = probe.texture.desc.mip_levels;
				desc.bind_flags = BindFlag::RENDER_TARGET | BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
				desc.format = format_rendertarget_envprobe;
				desc.layout = ResourceState::SHADER_RESOURCE;
				desc.misc_flags = ResourceMiscFlag::TEXTURECUBE;
				desc.sample_count = 1;
				device->CreateTexture(&desc, nullptr, &envrenderingColorBuffer);
				device->SetName(&envrenderingColorBuffer, "envrenderingColorBuffer");
				render_textures[id_color.raw] = envrenderingColorBuffer;

				// Cubes per mip level:
				for (uint32_t i = 0; i < envrenderingColorBuffer.desc.mip_levels; ++i)
				{
					int subresource_index;
					subresource_index = device->CreateSubresource(&envrenderingColorBuffer, SubresourceType::SRV, 0, envrenderingColorBuffer.desc.array_size, i, 1);
					assert(subresource_index == i);
					subresource_index = device->CreateSubresource(&envrenderingColorBuffer, SubresourceType::UAV, 0, envrenderingColorBuffer.desc.array_size, i, 1);
					assert(subresource_index == i);
				}

				std::string info;
				info += "Created envprobe render target for request";
				info += "\n\tResolution = " + std::to_string(desc.width) + " * " + std::to_string(desc.height) + " * 6";
				info += "\n\tSample Count = " + std::to_string(desc.sample_count);
				info += "\n\tMip Levels = " + std::to_string(desc.mip_levels);
				info += "\n\tFormat = ";
				info += GetFormatString(desc.format);
				info += "\n\tMemory = " + wi::helper::GetMemorySizeText(ComputeTextureMemorySizeInBytes(desc)) + "\n";
				wi::backlog::post(info);
			}

			if (!envrenderingColorBuffer_Filtered.IsValid())
			{
				desc.mip_levels = probe.texture.desc.mip_levels;
				desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
				desc.format = format_rendertarget_envprobe;
				desc.layout = ResourceState::SHADER_RESOURCE;
				desc.misc_flags = ResourceMiscFlag::TEXTURECUBE;
				desc.sample_count = 1;
				device->CreateTexture(&desc, nullptr, &envrenderingColorBuffer_Filtered);
				device->SetName(&envrenderingColorBuffer_Filtered, "envrenderingColorBuffer_Filtered");
				render_textures[id_color_filtered.raw] = envrenderingColorBuffer_Filtered;

				// Cubes per mip level:
				for (uint32_t i = 0; i < envrenderingColorBuffer_Filtered.desc.mip_levels; ++i)
				{
					int subresource_index;
					subresource_index = device->CreateSubresource(&envrenderingColorBuffer_Filtered, SubresourceType::SRV, 0, envrenderingColorBuffer_Filtered.desc.array_size, i, 1);
					assert(subresource_index == i);
					subresource_index = device->CreateSubresource(&envrenderingColorBuffer_Filtered, SubresourceType::UAV, 0, envrenderingColorBuffer_Filtered.desc.array_size, i, 1);
					assert(subresource_index == i);
				}

				std::string info;
				info += "Created envprobe filtering target for request";
				info += "\n\tResolution = " + std::to_string(desc.width) + " * " + std::to_string(desc.height) + " * 6";
				info += "\n\tSample Count = " + std::to_string(desc.sample_count);
				info += "\n\tMip Levels = " + std::to_string(desc.mip_levels);
				info += "\n\tFormat = ";
				info += GetFormatString(desc.format);
				info += "\n\tMemory = " + wi::helper::GetMemorySizeText(ComputeTextureMemorySizeInBytes(desc)) + "\n";
				wi::backlog::post(info);
			}

			if (required_sample_count > 1)
			{
				RenderTextureID id_color_msaa = {};
				id_color_msaa.bits.width = probe.resolution;
				id_color_msaa.bits.sample_count = required_sample_count;
				id_color_msaa.bits.is_depth = 0;
				id_color_msaa.bits.is_filtered = 0;
				envrenderingColorBuffer_MSAA = render_textures[id_color_msaa.raw];

				if (!envrenderingColorBuffer_MSAA.IsValid())
				{
					desc.sample_count = required_sample_count;
					desc.mip_levels = 1;
					desc.bind_flags = BindFlag::RENDER_TARGET;
					desc.misc_flags = ResourceMiscFlag::TRANSIENT_ATTACHMENT;
					desc.layout = ResourceState::RENDERTARGET;
					desc.format = format_rendertarget_envprobe;
					device->CreateTexture(&desc, nullptr, &envrenderingColorBuffer_MSAA);
					device->SetName(&envrenderingColorBuffer_MSAA, "envrenderingColorBuffer_MSAA");
					render_textures[id_color_msaa.raw] = envrenderingColorBuffer_MSAA;

					std::string info;
					info += "Created envprobe render target for request";
					info += "\n\tResolution = " + std::to_string(desc.width) + " * " + std::to_string(desc.height) + " * 6";
					info += "\n\tSample Count = " + std::to_string(desc.sample_count);
					info += "\n\tMip Levels = " + std::to_string(desc.mip_levels);
					info += "\n\tFormat = ";
					info += GetFormatString(desc.format);
					info += "\n\tMemory = " + wi::helper::GetMemorySizeText(ComputeTextureMemorySizeInBytes(desc)) + "\n";
					wi::backlog::post(info);
				}
			}
		}

		if (probe.IsMSAA())
		{
			const RenderPassImage rp[] = {
				RenderPassImage::RenderTarget(
					&envrenderingColorBuffer_MSAA,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::DONTCARE,
					ResourceState::RENDERTARGET,
					ResourceState::RENDERTARGET
				),
				RenderPassImage::Resolve(
					&envrenderingColorBuffer,
					ResourceState::SHADER_RESOURCE,
					ResourceState::SHADER_RESOURCE,
					0
				),
				RenderPassImage::DepthStencil(
					&envrenderingDepthBuffer,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::SHADER_RESOURCE,
					ResourceState::DEPTHSTENCIL,
					ResourceState::SHADER_RESOURCE
				),
			};
			device->RenderPassBegin(rp, arraysize(rp), cmd);
		}
		else
		{
			const RenderPassImage rp[] = {
				RenderPassImage::DepthStencil(
					&envrenderingDepthBuffer,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::SHADER_RESOURCE,
					ResourceState::DEPTHSTENCIL,
					ResourceState::SHADER_RESOURCE
				),
				RenderPassImage::RenderTarget(
					&envrenderingColorBuffer,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::SHADER_RESOURCE,
					ResourceState::SHADER_RESOURCE
				)
			};
			device->RenderPassBegin(rp, arraysize(rp), cmd);
		}

		// Scene will only be rendered if this is a real probe entity:
		if (probe_aabb.layerMask & vis.layerMask)
		{
			Sphere culler(probe.position, zFarP);

			renderQueue.init();
			for (size_t i = 0; i < vis.scene->aabb_objects.size(); ++i)
			{
				const AABB& aabb = vis.scene->aabb_objects[i];
				if ((aabb.layerMask & vis.layerMask) && (aabb.layerMask & probe_aabb.layerMask) && culler.intersects(aabb))
				{
					const ObjectComponent& object = vis.scene->objects[i];
					if (object.IsRenderable() && !object.IsNotVisibleInReflections())
					{
						uint8_t camera_mask = 0;
						for (uint32_t camera_index = 0; camera_index < arraysize(cameras); ++camera_index)
						{
							if (cameras[camera_index].frustum.CheckBoxFast(aabb))
							{
								camera_mask |= 1 << camera_index;
							}
						}
						if (camera_mask == 0)
							continue;

						renderQueue.add(object.mesh_index, uint32_t(i), 0, object.sort_bits, camera_mask);
					}
				}
			}

			if (!renderQueue.empty())
			{
				RenderMeshes(vis, renderQueue, RENDERPASS_ENVMAPCAPTURE, FILTER_ALL, cmd, 0, arraysize(cameras));
			}
		}

		// sky
		{
			if (vis.scene->weather.skyMap.IsValid())
			{
				device->BindPipelineState(&PSO_sky[SKYRENDERING_ENVMAPCAPTURE_STATIC], cmd);
			}
			else
			{
				device->BindPipelineState(&PSO_sky[SKYRENDERING_ENVMAPCAPTURE_DYNAMIC], cmd);
			}

			device->DrawInstanced(240, 6, 0, 0, cmd); // 6 instances so it will be replicated for every cubemap face
		}

		if (probe_aabb.layerMask & vis.layerMask) // only draw light visualizers if this is a hand placed probe
		{
			DrawLightVisualizers(vis, cmd, 6); // 6 instances so it will be replicated for every cubemap face
		}

		device->RenderPassEnd(cmd);

		// Compute Aerial Perspective for environment map
		if (vis.scene->weather.IsRealisticSky() && vis.scene->weather.IsRealisticSkyAerialPerspective() && (probe_aabb.layerMask & vis.layerMask))
		{
			if (probe.IsMSAA())
			{
				device->EventBegin("Aerial Perspective Capture [MSAA]", cmd);
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE_CAPTURE_MSAA], cmd);
			}
			else
			{
				device->EventBegin("Aerial Perspective Capture", cmd);
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE_CAPTURE], cmd);
			}

			device->BindResource(&envrenderingDepthBuffer, 0, cmd);

			TextureDesc desc = envrenderingColorBuffer.GetDesc();

			AerialPerspectiveCapturePushConstants push;
			push.resolution.x = desc.width;
			push.resolution.y = desc.height;
			push.resolution_rcp.x = 1.0f / push.resolution.x;
			push.resolution_rcp.y = 1.0f / push.resolution.y;
			push.texture_input = device->GetDescriptorIndex(&envrenderingColorBuffer, SubresourceType::SRV);
			push.texture_output = device->GetDescriptorIndex(&envrenderingColorBuffer, SubresourceType::UAV);

			device->PushConstants(&push, sizeof(push), cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				6,
				cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}

		// Compute Volumetric Clouds for environment map
		if (vis.scene->weather.IsVolumetricClouds() && (probe_aabb.layerMask & vis.layerMask))
		{
			if (probe.IsMSAA())
			{
				device->EventBegin("Volumetric Cloud Rendering Capture [MSAA]", cmd);
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER_CAPTURE_MSAA], cmd);
			}
			else
			{
				device->EventBegin("Volumetric Cloud Rendering Capture", cmd);
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER_CAPTURE], cmd);
			}

			device->BindResource(&envrenderingDepthBuffer, 5, cmd);

			device->BindResource(&texture_shapeNoise, 0, cmd);
			device->BindResource(&texture_detailNoise, 1, cmd);
			device->BindResource(&texture_curlNoise, 2, cmd);

			if (vis.scene->weather.volumetricCloudsWeatherMapFirst.IsValid())
			{
				device->BindResource(&vis.scene->weather.volumetricCloudsWeatherMapFirst.GetTexture(), 3, cmd);
			}
			else
			{
				device->BindResource(&texture_weatherMap, 3, cmd);
			}

			if (vis.scene->weather.volumetricCloudsWeatherMapSecond.IsValid())
			{
				device->BindResource(&vis.scene->weather.volumetricCloudsWeatherMapSecond.GetTexture(), 4, cmd);
			}
			else
			{
				device->BindResource(&texture_weatherMap, 4, cmd);
			}

			TextureDesc desc = envrenderingColorBuffer.GetDesc();

			VolumetricCloudCapturePushConstants push;
			push.resolution.x = desc.width;
			push.resolution.y = desc.height;
			push.resolution_rcp.x = 1.0f / push.resolution.x;
			push.resolution_rcp.y = 1.0f / push.resolution.y;
			push.texture_input = device->GetDescriptorIndex(&envrenderingColorBuffer, SubresourceType::SRV);
			push.texture_output = device->GetDescriptorIndex(&envrenderingColorBuffer, SubresourceType::UAV);

			if (probe.IsRealTime())
			{
				push.maxStepCount = 32;
				push.LODMin = 3;
				push.shadowSampleCount = 3;
				push.groundContributionSampleCount = 2;
			}
			else
			{
				// Use same parameters as current view
				push.maxStepCount = vis.scene->weather.volumetricCloudParameters.maxStepCount;
				push.LODMin = vis.scene->weather.volumetricCloudParameters.LODMin;
				push.shadowSampleCount = vis.scene->weather.volumetricCloudParameters.shadowSampleCount;
				push.groundContributionSampleCount = vis.scene->weather.volumetricCloudParameters.groundContributionSampleCount;
			}

			device->PushConstants(&push, sizeof(push), cmd);
			
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				6,
				cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}

		GenerateMipChain(envrenderingColorBuffer, MIPGENFILTER_LINEAR, cmd);

		// Filter the enviroment map mip chain according to BRDF:
		//	A bit similar to MIP chain generation, but its input is the MIP-mapped texture,
		//	and we generatethe filtered MIPs from bottom to top.
		device->EventBegin("FilterEnvMap", cmd);
		{
			// Copy over whole:
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, envrenderingColorBuffer.desc.layout, ResourceState::COPY_SRC),
					GPUBarrier::Image(&envrenderingColorBuffer_Filtered, envrenderingColorBuffer_Filtered.desc.layout, ResourceState::COPY_DST),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
			device->CopyResource(&envrenderingColorBuffer_Filtered, &envrenderingColorBuffer, cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer, ResourceState::COPY_SRC, envrenderingColorBuffer.desc.layout),
					GPUBarrier::Image(&envrenderingColorBuffer_Filtered, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			TextureDesc desc = envrenderingColorBuffer_Filtered.GetDesc();

			device->BindComputeShader(&shaders[CSTYPE_FILTERENVMAP], cmd);

			int mip_start = desc.mip_levels - 1;
			desc.width = std::max(1u, desc.width >> mip_start);
			desc.height = std::max(1u, desc.height >> mip_start);
			for (int i = mip_start; i > 0; --i)
			{
				FilterEnvmapPushConstants push;
				push.filterResolution.x = desc.width;
				push.filterResolution.y = desc.height;
				push.filterResolution_rcp.x = 1.0f / push.filterResolution.x;
				push.filterResolution_rcp.y = 1.0f / push.filterResolution.y;
				push.filterRoughness = (float)i / (float)(desc.mip_levels - 1);
				if (probe_aabb.layerMask & vis.layerMask)
				{
					// real probe:
					if (probe.IsRealTime())
					{
						push.filterRayCount = 1024;
					}
					else
					{
						push.filterRayCount = 8192;
					}
				}
				else
				{
					// dummy probe, reduced ray count:
					push.filterRayCount = 64;
				}
				push.texture_input = device->GetDescriptorIndex(&envrenderingColorBuffer, SubresourceType::SRV);
				push.texture_output = device->GetDescriptorIndex(&envrenderingColorBuffer_Filtered, SubresourceType::UAV, i);
				device->PushConstants(&push, sizeof(push), cmd);

				device->Dispatch(
					(desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE,
					(desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE,
					6,
					cmd
				);

				desc.width *= 2;
				desc.height *= 2;
			}

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&envrenderingColorBuffer_Filtered, ResourceState::UNORDERED_ACCESS, envrenderingColorBuffer_Filtered.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}
		device->EventEnd(cmd);

		// Finally, the complete envmap is block compressed into the probe's texture:
		BlockCompress(envrenderingColorBuffer_Filtered, probe.texture, cmd);
	};

	if (vis.scene->probes.GetCount() == 0)
	{
		// In this case, there are no probes, so the sky will be rendered to first envmap:
		const EnvironmentProbeComponent& probe = vis.scene->global_dynamic_probe;

		AABB probe_aabb;
		probe_aabb.layerMask = 0;
		if (probe.texture.IsValid())
		{
			render_probe(probe, probe_aabb);
		}
	}
	else
	{
		bool rendered_anything = false;
		for (size_t i = 0; i < vis.scene->probes.GetCount(); ++i)
		{
			const EnvironmentProbeComponent& probe = vis.scene->probes[i];
			const AABB& probe_aabb = vis.scene->aabb_probes[i];

			if ((probe_aabb.layerMask & vis.layerMask) && probe.render_dirty && probe.texture.IsValid())
			{
				probe.render_dirty = false;
				render_probe(probe, probe_aabb);
				rendered_anything = true;
			}
		}

		// Reset SkyAtmosphere SkyViewLut after usage:
		if (rendered_anything)
		{
			CameraCB cb;
			cb.init();
			cb.cameras[0].position = vis.camera->Eye;
			device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);

			ComputeSkyAtmosphereSkyViewLut(cmd);
		}
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd); // EnvironmentProbe Refresh
}

void RefreshImpostors(const Scene& scene, CommandList cmd)
{
	if (!scene.impostorArray.IsValid())
		return;

	device->EventBegin("Impostor Refresh", cmd);

	Viewport viewport;
	viewport.width = viewport.height = (float)scene.impostorTextureDim;
	device->BindViewports(1, &viewport, cmd);

	BindCommonResources(cmd);

	for (uint32_t impostorIndex = 0; impostorIndex < scene.impostors.GetCount(); ++impostorIndex)
	{
		const ImpostorComponent& impostor = scene.impostors[impostorIndex];
		if (!impostor.render_dirty || impostor.textureIndex < 0)
			continue;
		impostor.render_dirty = false;

		Entity entity = scene.impostors.GetEntity(impostorIndex);
		const MeshComponent& mesh = *scene.meshes.GetComponent(entity);

		// impostor camera will fit around mesh bounding sphere:
		const Sphere boundingsphere = mesh.GetBoundingSphere();

		device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

		for (size_t i = 0; i < impostorCaptureAngles; ++i)
		{
			CameraComponent impostorcamera;
			impostorcamera.SetCustomProjectionEnabled(true);
			TransformComponent camera_transform;

			camera_transform.ClearTransform();
			camera_transform.Translate(boundingsphere.center);

			XMMATRIX P = XMMatrixOrthographicOffCenterLH(-boundingsphere.radius, boundingsphere.radius, -boundingsphere.radius, boundingsphere.radius, -boundingsphere.radius, boundingsphere.radius);
			XMStoreFloat4x4(&impostorcamera.Projection, P);
			XMVECTOR Q = XMQuaternionNormalize(XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), XM_2PI * (float)i / (float)impostorCaptureAngles));
			XMStoreFloat4(&camera_transform.rotation_local, Q);

			camera_transform.UpdateTransform();
			impostorcamera.TransformCamera(camera_transform);
			impostorcamera.UpdateCamera();

			if (IsFormatUnorm(mesh.position_format))
			{
				XMMATRIX VP = impostorcamera.GetViewProjection();
				VP = mesh.aabb.getUnormRemapMatrix() * VP;
				XMStoreFloat4x4(&impostorcamera.VP, VP);
			}

			BindCameraCB(impostorcamera, impostorcamera, impostorcamera, cmd);

			int slice = int(impostor.textureIndex * impostorCaptureAngles * 3 + i * 3);

			const RenderPassImage rp[] = {
				RenderPassImage::RenderTarget(
					&scene.impostorRenderTarget_Albedo_MSAA,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::RENDERTARGET,
					ResourceState::RENDERTARGET
				),
				RenderPassImage::RenderTarget(
					&scene.impostorRenderTarget_Normal_MSAA,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::RENDERTARGET,
					ResourceState::RENDERTARGET
				),
				RenderPassImage::RenderTarget(
					&scene.impostorRenderTarget_Surface_MSAA,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::STORE,
					ResourceState::RENDERTARGET,
					ResourceState::RENDERTARGET
				),
				RenderPassImage::Resolve(&scene.impostorRenderTarget_Albedo),
				RenderPassImage::Resolve(&scene.impostorRenderTarget_Normal),
				RenderPassImage::Resolve(&scene.impostorRenderTarget_Surface),
				RenderPassImage::DepthStencil(
					&scene.impostorDepthStencil,
					RenderPassImage::LoadOp::CLEAR,
					RenderPassImage::StoreOp::DONTCARE
				)
			};
			device->RenderPassBegin(rp, arraysize(rp), cmd);

			device->BindPipelineState(&PSO_captureimpostor, cmd);

			uint32_t first_subset = 0;
			uint32_t last_subset = 0;
			mesh.GetLODSubsetRange(0, first_subset, last_subset);
			for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
			{
				const MeshComponent::MeshSubset& subset = mesh.subsets[subsetIndex];
				if (subset.indexCount == 0)
				{
					continue;
				}

				ObjectPushConstants push;
				push.geometryIndex = mesh.geometryOffset + subsetIndex;
				push.materialIndex = subset.materialIndex;
				push.instances = -1;
				push.instance_offset = 0;
				device->PushConstants(&push, sizeof(push), cmd);

				device->DrawIndexedInstanced(subset.indexCount, 1, subset.indexOffset, 0, 0, cmd);
			}

			device->RenderPassEnd(cmd);

			BlockCompress(scene.impostorRenderTarget_Albedo, scene.impostorArray, cmd, slice + 0);
			BlockCompress(scene.impostorRenderTarget_Normal, scene.impostorArray, cmd, slice + 1);
			BlockCompress(scene.impostorRenderTarget_Surface, scene.impostorArray, cmd, slice + 2);
		}

	}

	device->EventEnd(cmd);
}

void PaintDecals(const Scene& scene, CommandList cmd)
{
	if (paintdecals.empty())
		return;

	device->EventBegin("PaintDecals", cmd);

	BindCommonResources(cmd);

	for (auto& paint : paintdecals)
	{
		if (!scene.objects.Contains(paint.objectEntity))
			continue;
		const ObjectComponent& object = *scene.objects.GetComponent(paint.objectEntity);
		if (!scene.meshes.Contains(object.meshID))
			continue;
		const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);

		PaintDecalCB cb = {};
		XMStoreFloat4x4(&cb.g_xPaintDecalMatrix, XMMatrixInverse(nullptr, XMLoadFloat4x4(&paint.decalMatrix)));
		cb.g_xPaintDecalTexture = device->GetDescriptorIndex(&paint.in_texture, SubresourceType::SRV);
		cb.g_xPaintDecalInstanceID = (uint)scene.objects.GetIndex(paint.objectEntity);
		cb.g_xPaintDecalSlopeBlendPower = paint.slopeBlendPower;

		device->BindPipelineState(&PSO_paintdecal, cmd);

		device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(PaintDecalCB), cmd);

		device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

		Viewport vp;
		vp.width = (float)paint.out_texture.desc.width;
		vp.height = (float)paint.out_texture.desc.height;
		device->BindViewports(1, &vp, cmd);

		uint32_t indexOffset = ~0u;
		uint32_t indexCount = 0;

		uint32_t first_subset = 0;
		uint32_t last_subset = 0;
		mesh.GetLODSubsetRange(0, first_subset, last_subset);
		for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
		{
			indexOffset = std::min(indexOffset, mesh.subsets[subsetIndex].indexOffset);
			indexCount = std::max(indexCount, mesh.subsets[subsetIndex].indexCount);
		}

		RenderPassImage rp[] = {
			RenderPassImage::RenderTarget(&paint.out_texture)
		};
		device->RenderPassBegin(rp, arraysize(rp), cmd);

		device->DrawIndexed(indexCount, indexOffset, 0, cmd);

		device->RenderPassEnd(cmd);

		MIPGEN_OPTIONS mipopt;
		mipopt.preserve_coverage = true;
		GenerateMipChain(paint.out_texture, MIPGENFILTER_LINEAR, cmd, mipopt);
	}

	paintdecals.clear();

	device->EventEnd(cmd);
}

void CreateVXGIResources(VXGIResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.diffuse);
	device->SetName(&res.diffuse, "vxgi.diffuse");

	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.specular);
	device->SetName(&res.specular, "vxgi.specular");

	res.pre_clear = true;
}
void VXGI_Voxelize(
	const Visibility& vis,
	CommandList cmd
)
{
	if (!GetVXGIEnabled())
	{
		return;
	}

	device->EventBegin("VXGI - Voxelize", cmd);
	auto range = wi::profiler::BeginRangeGPU("VXGI - Voxelize", cmd);

	const Scene& scene = *vis.scene;
	const Scene::VXGI::ClipMap& clipmap = scene.vxgi.clipmaps[scene.vxgi.clipmap_to_update];

	AABB bbox;
	bbox.createFromHalfWidth(clipmap.center, clipmap.extents);

	renderQueue.init();
	for (size_t i = 0; i < scene.aabb_objects.size(); ++i)
	{
		const AABB& aabb = scene.aabb_objects[i];
		if (bbox.intersects(aabb))
		{
			const ObjectComponent& object = scene.objects[i];
			if (object.IsRenderable() && (scene.vxgi.clipmap_to_update < (VXGI_CLIPMAP_COUNT - object.cascadeMask)))
			{
				renderQueue.add(object.mesh_index, uint32_t(i), 0, object.sort_bits);
			}
		}
	}

	if (!renderQueue.empty())
	{
		BindCommonResources(cmd);

		VoxelizerCB cb;
		cb.offsetfromPrevFrame = clipmap.offsetfromPrevFrame;
		cb.clipmap_index = scene.vxgi.clipmap_to_update;
		device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_VOXELIZER, cmd);

		if (scene.vxgi.pre_clear)
		{
			device->EventBegin("Pre Clear", cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&scene.vxgi.render_atomic, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.prev_radiance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.radiance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.sdf, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.sdf_temp, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
			device->ClearUAV(&scene.vxgi.prev_radiance, 0, cmd);
			device->ClearUAV(&scene.vxgi.radiance, 0, cmd);
			device->ClearUAV(&scene.vxgi.sdf, 0, cmd);
			device->ClearUAV(&scene.vxgi.sdf_temp, 0, cmd);
			device->ClearUAV(&scene.vxgi.render_atomic, 0, cmd);
			scene.vxgi.pre_clear = false;

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(&scene.vxgi.render_atomic),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
			device->EventEnd(cmd);
		}
		else
		{
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&scene.vxgi.render_atomic, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.prev_radiance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.sdf, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&scene.vxgi.sdf_temp, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
			device->EventBegin("Atomic Clear", cmd);
			device->ClearUAV(&scene.vxgi.render_atomic, 0, cmd);
			device->EventEnd(cmd);

			device->EventBegin("Offset Previous Voxels", cmd);
			device->BindComputeShader(&shaders[CSTYPE_VXGI_OFFSETPREV], cmd);
			device->BindResource(&scene.vxgi.radiance, 0, cmd);
			device->BindUAV(&scene.vxgi.prev_radiance, 0, cmd);

			device->Dispatch(scene.vxgi.res / 8, scene.vxgi.res / 8, scene.vxgi.res / 8, cmd);

			device->EventEnd(cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Memory(&scene.vxgi.render_atomic),
					GPUBarrier::Image(&scene.vxgi.radiance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		{
			device->EventBegin("Voxelize", cmd);

			Viewport vp;
			vp.width = (float)scene.vxgi.res;
			vp.height = (float)scene.vxgi.res;
			device->BindViewports(1, &vp, cmd);

			device->BindResource(&scene.vxgi.prev_radiance, 0, cmd);
			device->BindUAV(&scene.vxgi.render_atomic, 0, cmd);

			device->RenderPassBegin(nullptr, 0, cmd, RenderPassFlags::ALLOW_UAV_WRITES);
#ifdef VOXELIZATION_GEOMETRY_SHADER_ENABLED
			const uint32_t frustum_count = 1; // axis will be selected by geometry shader
#else
			const uint32_t frustum_count = 3; // just used to replicate 3 times for main axes, but not with real frustums
#endif // VOXELIZATION_GEOMETRY_SHADER_ENABLED
			RenderMeshes(vis, renderQueue, RENDERPASS_VOXELIZE, FILTER_OPAQUE, cmd, 0, frustum_count);
			device->RenderPassEnd(cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&scene.vxgi.render_atomic, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
					GPUBarrier::Image(&scene.vxgi.prev_radiance, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}

		{
			device->EventBegin("Temporal Blend Voxels", cmd);
			device->BindComputeShader(&shaders[CSTYPE_VXGI_TEMPORAL], cmd);
			device->BindResource(&scene.vxgi.prev_radiance, 0, cmd);
			device->BindResource(&scene.vxgi.render_atomic, 1, cmd);
			device->BindUAV(&scene.vxgi.radiance, 0, cmd);
			device->BindUAV(&scene.vxgi.sdf, 1, cmd);

			device->Dispatch(scene.vxgi.res / 8, scene.vxgi.res / 8, scene.vxgi.res / 8, cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&scene.vxgi.sdf, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
					GPUBarrier::Image(&scene.vxgi.radiance, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
			device->EventEnd(cmd);
		}

		{
			device->EventBegin("SDF Jump Flood", cmd);
			device->BindComputeShader(&shaders[CSTYPE_VXGI_SDF_JUMPFLOOD], cmd);

			const Texture* _write = &scene.vxgi.sdf_temp;
			const Texture* _read = &scene.vxgi.sdf;

			int passcount = (int)std::ceil(std::log2((float)scene.vxgi.res));
			for (int i = 0; i < passcount; ++i)
			{
				float jump_size = std::pow(2.0f, float(passcount - i - 1));
				device->PushConstants(&jump_size, sizeof(jump_size), cmd);

				device->BindUAV(_write, 0, cmd);
				device->BindResource(_read, 0, cmd);

				device->Dispatch(scene.vxgi.res / 8, scene.vxgi.res / 8, scene.vxgi.res / 8, cmd);

				if (i < (passcount - 1))
				{
					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(_write, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
							GPUBarrier::Image(_read, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}
					std::swap(_read, _write);
				}
			}

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(_write, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			device->EventEnd(cmd);
		}
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void VXGI_Resolve(
	const VXGIResources& res,
	const Scene& scene,
	Texture texture_lineardepth,
	CommandList cmd
)
{
	if (!GetVXGIEnabled() || !scene.vxgi.radiance.IsValid())
	{
		return;
	}

	device->EventBegin("VXGI - Resolve", cmd);
	auto range = wi::profiler::BeginRangeGPU("VXGI - Resolve", cmd);

	BindCommonResources(cmd);

	if (res.pre_clear)
	{
		res.pre_clear = false;
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.diffuse, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.specular, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
		device->ClearUAV(&res.diffuse, 0, cmd);
		device->ClearUAV(&res.specular, 0, cmd);
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.diffuse, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.specular, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.diffuse, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.specular, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	{
		device->EventBegin("Diffuse", cmd);
		device->BindComputeShader(&shaders[CSTYPE_VXGI_RESOLVE_DIFFUSE], cmd);

		PostProcess postprocess;
		device->BindUAV(&res.diffuse, 0, cmd);
		postprocess.resolution.x = res.diffuse.desc.width;
		postprocess.resolution.y = res.diffuse.desc.height;
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		uint2 dispatch_dim;
		dispatch_dim.x = postprocess.resolution.x;
		dispatch_dim.y = postprocess.resolution.y;

		device->Dispatch((dispatch_dim.x + 7u) / 8u, (dispatch_dim.y + 7u) / 8u, 1, cmd);

		device->EventEnd(cmd);
	}

	if(VXGI_REFLECTIONS_ENABLED)
	{
		device->EventBegin("Specular", cmd);
		device->BindComputeShader(&shaders[CSTYPE_VXGI_RESOLVE_SPECULAR], cmd);

		PostProcess postprocess;
		device->BindUAV(&res.specular, 0, cmd);
		postprocess.resolution.x = res.specular.desc.width;
		postprocess.resolution.y = res.specular.desc.height;
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		uint2 dispatch_dim;
		dispatch_dim.x = postprocess.resolution.x;
		dispatch_dim.y = postprocess.resolution.y;

		device->Dispatch((dispatch_dim.x + 7u) / 8u, (dispatch_dim.y + 7u) / 8u, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.diffuse, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
			GPUBarrier::Image(&res.specular, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateTiledLightResources(TiledLightResources& res, XMUINT2 resolution)
{
	res.tileCount = GetEntityCullingTileCount(resolution);

	GPUBufferDesc bd;
	bd.stride = sizeof(uint);
	bd.size = res.tileCount.x * res.tileCount.y * bd.stride * SHADER_ENTITY_TILE_BUCKET_COUNT * 2; // *2: opaque and transparent arrays
	bd.usage = Usage::DEFAULT;
	bd.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
	bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	device->CreateBuffer(&bd, nullptr, &res.entityTiles);
	device->SetName(&res.entityTiles, "entityTiles");
}
void ComputeTiledLightCulling(
	const TiledLightResources& res,
	const Visibility& vis,
	const Texture& debugUAV,
	CommandList cmd
)
{
	auto range = wi::profiler::BeginRangeGPU("Entity Culling", cmd);

	device->Barrier(GPUBarrier::Buffer(&res.entityTiles, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS), cmd);

	if (
		vis.visibleLights.empty() &&
		vis.visibleDecals.empty() &&
		vis.visibleEnvProbes.empty()
		)
	{
		device->EventBegin("Tiled Entity Clear Only", cmd);
		device->ClearUAV(&res.entityTiles, 0, cmd);
		device->Barrier(GPUBarrier::Buffer(&res.entityTiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE), cmd);
		device->EventEnd(cmd);
		wi::profiler::EndRange(range);
		return;
	}

	BindCommonResources(cmd);

	device->EventBegin("Entity Culling", cmd);

	if (GetDebugLightCulling() && debugUAV.IsValid())
	{
		device->BindComputeShader(&shaders[GetAdvancedLightCulling() ? CSTYPE_LIGHTCULLING_ADVANCED_DEBUG : CSTYPE_LIGHTCULLING_DEBUG], cmd);
		device->BindUAV(&debugUAV, 3, cmd);
	}
	else
	{
		device->BindComputeShader(&shaders[GetAdvancedLightCulling() ? CSTYPE_LIGHTCULLING_ADVANCED : CSTYPE_LIGHTCULLING], cmd);
	}

	device->BindUAV(&res.entityTiles, 0, cmd);

	device->Dispatch(res.tileCount.x, res.tileCount.y, 1, cmd);

	device->Barrier(GPUBarrier::Buffer(&res.entityTiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE), cmd);

	device->EventEnd(cmd);

	// Unbind from UAV slots:
	GPUResource empty;
	const GPUResource* uavs[] = {
		&empty,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	wi::profiler::EndRange(range);
}


void ResolveMSAADepthBuffer(const Texture& dst, const Texture& src, CommandList cmd)
{
	device->EventBegin("ResolveMSAADepthBuffer", cmd);

	device->BindResource(&src, 0, cmd);
	device->BindUAV(&dst, 0, cmd);

	device->Barrier(GPUBarrier::Image(&dst, dst.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&dst, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&dst), cmd);

	const TextureDesc& desc = src.GetDesc();

	device->BindComputeShader(&shaders[CSTYPE_RESOLVEMSAADEPTHSTENCIL], cmd);
	device->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, 1, cmd);

	device->Barrier(GPUBarrier::Image(&dst, ResourceState::UNORDERED_ACCESS, dst.desc.layout), cmd);

	device->EventEnd(cmd);
}
void DownsampleDepthBuffer(const Texture& src, CommandList cmd)
{
	device->EventBegin("DownsampleDepthBuffer", cmd);

	device->BindPipelineState(&PSO_downsampledepthbuffer, cmd);

	device->BindResource(&src, 0, cmd);

	device->Draw(3, 0, cmd);


	device->EventEnd(cmd);
}

void GenerateMipChain(const Texture& texture, MIPGENFILTER filter, CommandList cmd, const MIPGEN_OPTIONS& options)
{
	if (!texture.IsValid())
	{
		assert(0);
		return;
	}

	TextureDesc desc = texture.GetDesc();

	if (desc.mip_levels < 2)
	{
		assert(0);
		return;
	}

	MipgenPushConstants mipgen = {};

	if (options.preserve_coverage)
	{
		mipgen.mipgen_options |= MIPGEN_OPTION_BIT_PRESERVE_COVERAGE;
	}
	if (IsFormatSRGB(desc.format))
	{
		mipgen.mipgen_options |= MIPGEN_OPTION_BIT_SRGB;
	}

	if (desc.type == TextureDesc::Type::TEXTURE_1D)
	{
		assert(0); // not implemented
	}
	else if (desc.type == TextureDesc::Type::TEXTURE_2D)
	{

		if (has_flag(desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
		{

			if (desc.array_size > 6)
			{
				// Cubearray
				assert(options.arrayIndex >= 0 && "You should only filter a specific cube in the array for now, so provide its index!");

				switch (filter)
				{
				case MIPGENFILTER_POINT:
					device->EventBegin("GenerateMipChain CubeArray - PointFilter", cmd);
					device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
					break;
				case MIPGENFILTER_LINEAR:
					device->EventBegin("GenerateMipChain CubeArray - LinearFilter", cmd);
					device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAINCUBEARRAY_FLOAT4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
					break;
				default:
					assert(0);
					break;
				}

				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 0),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 1),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 2),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 3),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 4),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, options.arrayIndex * 6 + 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}

					mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
					mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
					desc.width = std::max(1u, desc.width / 2);
					desc.height = std::max(1u, desc.height / 2);

					mipgen.outputResolution.x = desc.width;
					mipgen.outputResolution.y = desc.height;
					mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
					mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
					mipgen.arrayIndex = options.arrayIndex;
					device->PushConstants(&mipgen, sizeof(mipgen), cmd);

					device->Dispatch(
						std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						6,
						cmd);

					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 0),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 1),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 2),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 3),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 4),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, options.arrayIndex * 6 + 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}
				}
			}
			else
			{
				// Cubemap
				switch (filter)
				{
				case MIPGENFILTER_POINT:
					device->EventBegin("GenerateMipChain Cube - PointFilter", cmd);
					device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
					break;
				case MIPGENFILTER_LINEAR:
					device->EventBegin("GenerateMipChain Cube - LinearFilter", cmd);
					device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAINCUBE_FLOAT4], cmd);
					mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
					break;
				default:
					assert(0); // not implemented
					break;
				}

				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 0),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 1),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 2),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 3),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 4),
							GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1, 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}

					mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
					mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
					desc.width = std::max(1u, desc.width / 2);
					desc.height = std::max(1u, desc.height / 2);

					mipgen.outputResolution.x = desc.width;
					mipgen.outputResolution.y = desc.height;
					mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
					mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
					mipgen.arrayIndex = 0;
					device->PushConstants(&mipgen, sizeof(mipgen), cmd);

					device->Dispatch(
						std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
						6,
						cmd);

					{
						GPUBarrier barriers[] = {
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 0),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 1),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 2),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 3),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 4),
							GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1, 5),
						};
						device->Barrier(barriers, arraysize(barriers), cmd);
					}
				}
			}

		}
		else
		{
			// Texture
			switch (filter)
			{
			case MIPGENFILTER_POINT:
				device->EventBegin("GenerateMipChain 2D - PointFilter", cmd);
				device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4], cmd);
				mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
				break;
			case MIPGENFILTER_LINEAR:
				device->EventBegin("GenerateMipChain 2D - LinearFilter", cmd);
				device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAIN2D_FLOAT4], cmd);
				mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
				break;
			case MIPGENFILTER_GAUSSIAN:
			{
				assert(options.gaussian_temp != nullptr); // needed for separate filter!
				device->EventBegin("GenerateMipChain 2D - GaussianFilter", cmd);
				// Gaussian filter is a bit different as we do it in a separable way:
				for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
				{
					Postprocess_Blur_Gaussian(texture, *options.gaussian_temp, texture, cmd, i, i + 1 , options.wide_gauss);
				}
				device->EventEnd(cmd);
				return;
			}
				break;
			default:
				assert(0);
				break;
			}

			for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
			{
				device->Barrier(GPUBarrier::Image(&texture, texture.desc.layout, ResourceState::UNORDERED_ACCESS, i + 1), cmd);

				mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
				mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
				desc.width = std::max(1u, desc.width / 2);
				desc.height = std::max(1u, desc.height / 2);

				mipgen.outputResolution.x = desc.width;
				mipgen.outputResolution.y = desc.height;
				mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
				mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
				mipgen.arrayIndex = options.arrayIndex >= 0 ? (uint)options.arrayIndex : 0;
				device->PushConstants(&mipgen, sizeof(mipgen), cmd);

				device->Dispatch(
					std::max(1u, (desc.width + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
					std::max(1u, (desc.height + GENERATEMIPCHAIN_2D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_2D_BLOCK_SIZE),
					1,
					cmd
				);

				device->Barrier(GPUBarrier::Image(&texture, ResourceState::UNORDERED_ACCESS, texture.desc.layout, i + 1), cmd);
			}
		}


		device->EventEnd(cmd);
	}
	else if (desc.type == TextureDesc::Type::TEXTURE_3D)
	{
		switch (filter)
		{
		case MIPGENFILTER_POINT:
			device->EventBegin("GenerateMipChain 3D - PointFilter", cmd);
			device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4], cmd);
			mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_POINT_CLAMP]);
			break;
		case MIPGENFILTER_LINEAR:
			device->EventBegin("GenerateMipChain 3D - LinearFilter", cmd);
			device->BindComputeShader(&shaders[CSTYPE_GENERATEMIPCHAIN3D_FLOAT4], cmd);
			mipgen.sampler_index = device->GetDescriptorIndex(&samplers[SAMPLER_LINEAR_CLAMP]);
			break;
		default:
			assert(0); // not implemented
			break;
		}

		for (uint32_t i = 0; i < desc.mip_levels - 1; ++i)
		{
			mipgen.texture_output = device->GetDescriptorIndex(&texture, SubresourceType::UAV, i + 1);
			mipgen.texture_input = device->GetDescriptorIndex(&texture, SubresourceType::SRV, i);
			desc.width = std::max(1u, desc.width / 2);
			desc.height = std::max(1u, desc.height / 2);
			desc.depth = std::max(1u, desc.depth / 2);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture,texture.desc.layout,ResourceState::UNORDERED_ACCESS,i + 1),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			mipgen.outputResolution.x = desc.width;
			mipgen.outputResolution.y = desc.height;
			mipgen.outputResolution.z = desc.depth;
			mipgen.outputResolution_rcp.x = 1.0f / mipgen.outputResolution.x;
			mipgen.outputResolution_rcp.y = 1.0f / mipgen.outputResolution.y;
			mipgen.outputResolution_rcp.z = 1.0f / mipgen.outputResolution.z;
			mipgen.arrayIndex = options.arrayIndex >= 0 ? (uint)options.arrayIndex : 0;
			mipgen.mipgen_options = 0;
			device->PushConstants(&mipgen, sizeof(mipgen), cmd);

			device->Dispatch(
				std::max(1u, (desc.width + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				std::max(1u, (desc.height + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				std::max(1u, (desc.depth + GENERATEMIPCHAIN_3D_BLOCK_SIZE - 1) / GENERATEMIPCHAIN_3D_BLOCK_SIZE),
				cmd);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&texture,ResourceState::UNORDERED_ACCESS,texture.desc.layout,i + 1),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}


		device->EventEnd(cmd);
	}
	else
	{
		assert(0);
	}
}

void BlockCompress(const Texture& texture_src, const Texture& texture_bc, CommandList cmd, uint32_t dst_slice_offset)
{
	const uint32_t block_size = GetFormatBlockSize(texture_bc.desc.format);
	TextureDesc desc;
	desc.width = std::max(1u, (texture_bc.desc.width + block_size - 1) / block_size);
	desc.height = std::max(1u, (texture_bc.desc.height + block_size - 1) / block_size);
	desc.bind_flags = BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::UNORDERED_ACCESS;

	Texture bc_raw_dest;
	{
		// Find a raw block texture that will fit the request:
		static std::mutex locker;
		std::scoped_lock lock(locker);
		static Texture bc_raw_uint2;
		static Texture bc_raw_uint4;
		static Texture bc_raw_uint4_cubemap;
		Texture* bc_raw = nullptr;
		switch (texture_bc.desc.format)
		{
		case Format::BC1_UNORM:
		case Format::BC1_UNORM_SRGB:
			desc.format = Format::R32G32_UINT;
			bc_raw = &bc_raw_uint2;
			device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC1], cmd);
			device->EventBegin("BlockCompress - BC1", cmd);
			break;
		case Format::BC3_UNORM:
		case Format::BC3_UNORM_SRGB:
			desc.format = Format::R32G32B32A32_UINT;
			bc_raw = &bc_raw_uint4;
			device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC3], cmd);
			device->EventBegin("BlockCompress - BC3", cmd);
			break;
		case Format::BC4_UNORM:
			desc.format = Format::R32G32_UINT;
			bc_raw = &bc_raw_uint2;
			device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC4], cmd);
			device->EventBegin("BlockCompress - BC4", cmd);
			break;
		case Format::BC5_UNORM:
			desc.format = Format::R32G32B32A32_UINT;
			bc_raw = &bc_raw_uint4;
			device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC5], cmd);
			device->EventBegin("BlockCompress - BC5", cmd);
			break;
		case Format::BC6H_UF16:
			desc.format = Format::R32G32B32A32_UINT;
			if (has_flag(texture_src.desc.misc_flags, ResourceMiscFlag::TEXTURECUBE))
			{
				bc_raw = &bc_raw_uint4_cubemap;
				device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC6H_CUBEMAP], cmd);
				device->EventBegin("BlockCompress - BC6H - Cubemap", cmd);
				desc.array_size = texture_src.desc.array_size; // src array size not dst!!
			}
			else
			{
				bc_raw = &bc_raw_uint4;
				device->BindComputeShader(&shaders[CSTYPE_BLOCKCOMPRESS_BC6H], cmd);
				device->EventBegin("BlockCompress - BC6H", cmd);
			}
			break;
		default:
			assert(0); // not supported
			return;
		}

		if (!bc_raw->IsValid() || bc_raw->desc.width < desc.width || bc_raw->desc.height < desc.height || bc_raw->desc.array_size < desc.array_size)
		{
			TextureDesc bc_raw_desc = desc;
			bc_raw_desc.width = std::max(64u, bc_raw_desc.width);
			bc_raw_desc.height = std::max(64u, bc_raw_desc.height);
			bc_raw_desc.width = std::max(bc_raw->desc.width, bc_raw_desc.width);
			bc_raw_desc.height = std::max(bc_raw->desc.height, bc_raw_desc.height);
			bc_raw_desc.width = wi::math::GetNextPowerOfTwo(bc_raw_desc.width);
			bc_raw_desc.height = wi::math::GetNextPowerOfTwo(bc_raw_desc.height);
			device->CreateTexture(&bc_raw_desc, nullptr, bc_raw);
			device->SetName(bc_raw, "bc_raw");

			device->ClearUAV(bc_raw, 0, cmd);
			device->Barrier(GPUBarrier::Memory(bc_raw), cmd);

			std::string info;
			info += "BlockCompress created a new raw block texture to fit request: " + std::string(GetFormatString(texture_bc.desc.format)) + " (" + std::to_string(texture_bc.desc.width) + ", " + std::to_string(texture_bc.desc.height) + ")";
			info += "\n\tFormat = ";
			info += GetFormatString(bc_raw_desc.format);
			info += "\n\tResolution = " + std::to_string(bc_raw_desc.width) + " * " + std::to_string(bc_raw_desc.height);
			info += "\n\tArray Size = " + std::to_string(bc_raw_desc.array_size);
			size_t total_size = 0;
			total_size += ComputeTextureMemorySizeInBytes(bc_raw_desc);
			info += "\n\tMemory = " + wi::helper::GetMemorySizeText(total_size) + "\n";
			wi::backlog::post(info);
		}

		bc_raw_dest = *bc_raw;
	}

	for (uint32_t mip = 0; mip < texture_bc.desc.mip_levels; ++mip)
	{
		const uint32_t src_width = std::max(1u, texture_bc.desc.width >> mip);
		const uint32_t src_height = std::max(1u, texture_bc.desc.height >> mip);
		const uint32_t dst_width = (src_width + block_size - 1) / block_size;
		const uint32_t dst_height = (src_height + block_size - 1) / block_size;
		device->BindResource(&texture_src, 0, cmd, texture_src.desc.mip_levels == 1 ? -1 : mip);
		device->BindUAV(&bc_raw_dest, 0, cmd);
		device->Dispatch((dst_width + 7u) / 8u, (dst_height + 7u) / 8u, desc.array_size, cmd);

		GPUBarrier barriers[] = {
			GPUBarrier::Image(&bc_raw_dest, ResourceState::UNORDERED_ACCESS, ResourceState::COPY_SRC),
			GPUBarrier::Image(&texture_bc, texture_bc.desc.layout, ResourceState::COPY_DST),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		for (uint32_t slice = 0; slice < desc.array_size; ++slice)
		{
			Box box;
			box.left = 0;
			box.right = dst_width;
			box.top = 0;
			box.bottom = dst_height;
			box.front = 0;
			box.back = 1;

			device->CopyTexture(
				&texture_bc, 0, 0, 0, mip, dst_slice_offset + slice,
				&bc_raw_dest, 0, slice,
				cmd,
				&box
			);
		}

		for (int i = 0; i < arraysize(barriers); ++i)
		{
			std::swap(barriers[i].image.layout_before, barriers[i].image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}

void CopyTexture2D(const Texture& dst, int DstMIP, int DstX, int DstY, const Texture& src, int SrcMIP, int SrcX, int SrcY, CommandList cmd, BORDEREXPANDSTYLE borderExpand, bool srgb_convert)
{
	const TextureDesc& desc_dst = dst.GetDesc();
	const TextureDesc& desc_src = src.GetDesc();

	assert(has_flag(desc_dst.bind_flags, BindFlag::UNORDERED_ACCESS));
	assert(has_flag(desc_src.bind_flags, BindFlag::SHADER_RESOURCE));

	if (borderExpand == BORDEREXPAND_DISABLE)
	{
		device->EventBegin("CopyTexture_FLOAT4", cmd);
		device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_FLOAT4], cmd);
	}
	else
	{
		device->EventBegin("CopyTexture_BORDEREXPAND_FLOAT4", cmd);
		device->BindComputeShader(&shaders[CSTYPE_COPYTEXTURE2D_FLOAT4_BORDEREXPAND], cmd);
	}

	CopyTextureCB cb;
	cb.xCopyDst.x = DstX;
	cb.xCopyDst.y = DstY;
	cb.xCopySrc.x = SrcX;
	cb.xCopySrc.y = SrcY;
	cb.xCopySrcSize.x = desc_src.width >> SrcMIP;
	cb.xCopySrcSize.y = desc_src.height >> SrcMIP;
	cb.xCopySrcMIP = SrcMIP;
	cb.xCopyFlags = 0;
	if (borderExpand == BORDEREXPAND_WRAP)
	{
		cb.xCopyFlags |= COPY_TEXTURE_WRAP;
	}
	if (srgb_convert)
	{
		cb.xCopyFlags |= COPY_TEXTURE_SRGB;
	}
	device->PushConstants(&cb, sizeof(cb), cmd);

	device->BindResource(&src, 0, cmd);

	device->BindUAV(&dst, 0, cmd, DstMIP);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&dst,dst.desc.layout,ResourceState::UNORDERED_ACCESS, DstMIP),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	XMUINT2 copy_dim = XMUINT2(
		std::min((uint32_t)cb.xCopySrcSize.x, uint32_t((desc_dst.width - DstX) >> DstMIP)),
		std::min((uint32_t)cb.xCopySrcSize.y, uint32_t((desc_dst.height - DstY) >> DstMIP))
	);
	device->Dispatch((copy_dim.x + 7) / 8, (copy_dim.y + 7) / 8, 1, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&dst,ResourceState::UNORDERED_ACCESS,dst.desc.layout, DstMIP),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}


void RayTraceScene(
	const Scene& scene,
	const Texture& output,
	int accumulation_sample, 
	CommandList cmd,
	const Texture* output_albedo,
	const Texture* output_normal,
	const Texture* output_depth,
	const Texture* output_stencil,
	const Texture* output_depth_stencil
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	device->EventBegin("RayTraceScene", cmd);
	auto range = wi::profiler::BeginRangeGPU("RayTraceScene", cmd);

	const TextureDesc& desc = output.GetDesc();

	BindCommonResources(cmd);

	const XMFLOAT4& halton = wi::math::GetHaltonSequence(accumulation_sample);
	RaytracingCB cb = {};
	cb.xTracePixelOffset = XMFLOAT2(halton.x, halton.y);
	cb.xTraceAccumulationFactor = 1.0f / ((float)accumulation_sample + 1.0f);
	cb.xTraceResolution.x = desc.width;
	cb.xTraceResolution.y = desc.height;
	cb.xTraceResolution_rcp.x = 1.0f / cb.xTraceResolution.x;
	cb.xTraceResolution_rcp.y = 1.0f / cb.xTraceResolution.y;
	cb.xTraceUserData.x = raytraceBounceCount;
	uint8_t instanceInclusionMask = 0xFF;
	cb.xTraceUserData.y = instanceInclusionMask;
	cb.xTraceSampleIndex = (uint32_t)accumulation_sample;
	device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(RaytracingCB), cmd);

	device->BindComputeShader(&shaders[CSTYPE_RAYTRACE], cmd);

	GPUResource nullUAV;
	const GPUResource* uavs[] = {
		&output,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
		&nullUAV,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	PushBarrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS));

	if (output_albedo != nullptr)
	{
		device->BindUAV(output_albedo, 1, cmd);
	}
	if (output_normal != nullptr)
	{
		device->BindUAV(output_normal, 2, cmd);
	}
	if (output_depth != nullptr)
	{
		device->BindUAV(output_depth, 3, cmd);
		PushBarrier(GPUBarrier::Image(output_depth, output_depth->desc.layout, ResourceState::UNORDERED_ACCESS));
	}
	if (output_stencil != nullptr)
	{
		device->BindUAV(output_stencil, 4, cmd);
		PushBarrier(GPUBarrier::Image(output_stencil, output_stencil->desc.layout, ResourceState::UNORDERED_ACCESS));
	}
	FlushBarriers(cmd);

	if (accumulation_sample == 0)
	{
		device->ClearUAV(&output, 0, cmd);
		PushBarrier(GPUBarrier::Memory(&output));
		if (output_albedo != nullptr)
		{
			device->ClearUAV(output_albedo, 0, cmd);
			PushBarrier(GPUBarrier::Memory(output_albedo));
		}
		if (output_normal != nullptr)
		{
			device->ClearUAV(output_normal, 0, cmd);
			PushBarrier(GPUBarrier::Memory(output_normal));
		}
		if (output_depth != nullptr)
		{
			device->ClearUAV(output_depth, 0, cmd);
			PushBarrier(GPUBarrier::Memory(output_depth));
		}
		if (output_stencil != nullptr)
		{
			device->ClearUAV(output_stencil, 0, cmd);
			PushBarrier(GPUBarrier::Memory(output_stencil));
		}
		FlushBarriers(cmd);
	}

	device->Dispatch(
		(desc.width + RAYTRACING_LAUNCH_BLOCKSIZE - 1) / RAYTRACING_LAUNCH_BLOCKSIZE,
		(desc.height + RAYTRACING_LAUNCH_BLOCKSIZE - 1) / RAYTRACING_LAUNCH_BLOCKSIZE,
		1,
		cmd
	);

	PushBarrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout));
	if (output_depth != nullptr)
	{
		PushBarrier(GPUBarrier::Image(output_depth, ResourceState::UNORDERED_ACCESS, output_depth->desc.layout));
	}
	if (output_stencil != nullptr)
	{
		PushBarrier(GPUBarrier::Image(output_stencil, ResourceState::UNORDERED_ACCESS, output_stencil->desc.layout));
	}
	FlushBarriers(cmd);

	if (output_depth_stencil != nullptr)
	{
		CopyDepthStencil(
			output_depth,
			output_stencil,
			*output_depth_stencil,
			cmd,
			0xFF
		);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd); // RayTraceScene
}
void RayTraceSceneBVH(const Scene& scene, CommandList cmd)
{
	if (!scene.BVH.IsValid())
		return;
	device->EventBegin("RayTraceSceneBVH", cmd);
	device->BindPipelineState(&PSO_debug[DEBUGRENDERING_RAYTRACE_BVH], cmd);
	device->Draw(3, 0, cmd);
	device->EventEnd(cmd);
}

void RefreshLightmaps(const Scene& scene, CommandList cmd)
{
	if (!scene.IsLightmapUpdateRequested())
		return;
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	const uint32_t lightmap_request_count = scene.lightmap_request_allocator.load();

	auto range = wi::profiler::BeginRangeGPU("Lightmap Processing", cmd);

	BindCommonResources(cmd);

	// Scan for max allocation, to avoid multiple allocations in loop (because dealloc is deferred for multiple frames, it could introduce overallocation):
	uint32_t max_width = 0;
	uint32_t max_height = 0;
	for (uint32_t requestIndex = 0; requestIndex < lightmap_request_count; ++requestIndex)
	{
		uint32_t objectIndex = *(scene.lightmap_requests.data() + requestIndex);
		const ObjectComponent& object = scene.objects[objectIndex];
		if (!object.lightmap.IsValid())
			continue;
		if (!object.lightmap_render.IsValid())
			continue;

		if (object.IsLightmapRenderRequested())
		{
			max_width = std::max(max_width, object.lightmap.desc.width);
			max_height = std::max(max_height, object.lightmap.desc.height);
		}
	}

	static Texture lightmap_color_tmp;
	static Texture lightmap_depth_tmp;
	if (lightmap_color_tmp.desc.width < max_width || lightmap_color_tmp.desc.height < max_height)
	{
		TextureDesc desc;
		desc.width = wi::math::GetNextPowerOfTwo(max_width);
		desc.height = wi::math::GetNextPowerOfTwo(max_height);
		desc.format = Format::R16G16B16A16_FLOAT;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS | BindFlag::RENDER_TARGET; // Note: RENDER_TARGET is not used, but it's created with support for ResourceMiscFlag::ALIASING_TEXTURE_RT_DS
		desc.misc_flags = ResourceMiscFlag::ALIASING_TEXTURE_RT_DS;
		device->CreateTexture(&desc, nullptr, &lightmap_color_tmp);

		desc.format = Format::D16_UNORM;
		desc.bind_flags = BindFlag::DEPTH_STENCIL;
		desc.layout = ResourceState::DEPTHSTENCIL;
		desc.misc_flags = ResourceMiscFlag::NONE;
		device->CreateTexture(&desc, nullptr, &lightmap_depth_tmp, &lightmap_color_tmp); // aliased!
	}

	// Render lightmaps for each object:
	for (uint32_t requestIndex = 0; requestIndex < lightmap_request_count; ++requestIndex)
	{
		uint32_t objectIndex = *(scene.lightmap_requests.data() + requestIndex);
		const ObjectComponent& object = scene.objects[objectIndex];
		if (!object.lightmap.IsValid())
			continue;
		if (!object.lightmap_render.IsValid())
			continue;

		if (object.IsLightmapRenderRequested())
		{
			device->EventBegin("RenderObjectLightMap", cmd);

			const MeshComponent& mesh = scene.meshes[object.mesh_index];
			assert(!mesh.vertex_atlas.empty());
			assert(mesh.vb_atl.IsValid());

			const TextureDesc& desc = object.lightmap_render.GetDesc();

			device->Barrier(GPUBarrier::Aliasing(&lightmap_color_tmp, &lightmap_depth_tmp), cmd);

			// Note: depth is used to disallow overlapped pixel/primitive writes with conservative rasterization!
			if (object.lightmapIterationCount == 0)
			{
				RenderPassImage rp[] = {
					RenderPassImage::RenderTarget(&object.lightmap_render, RenderPassImage::LoadOp::CLEAR),
					RenderPassImage::DepthStencil(&lightmap_depth_tmp, RenderPassImage::LoadOp::CLEAR),
				};
				device->RenderPassBegin(rp, arraysize(rp), cmd);
			}
			else
			{
				RenderPassImage rp[] = {
					RenderPassImage::RenderTarget(&object.lightmap_render, RenderPassImage::LoadOp::LOAD),
					RenderPassImage::DepthStencil(&lightmap_depth_tmp, RenderPassImage::LoadOp::CLEAR),
				};
				device->RenderPassBegin(rp, arraysize(rp), cmd);
			}

			Viewport vp;
			vp.width = (float)desc.width;
			vp.height = (float)desc.height;
			device->BindViewports(1, &vp, cmd);

			device->BindPipelineState(&PSO_renderlightmap, cmd);

			device->BindIndexBuffer(&mesh.generalBuffer, mesh.GetIndexFormat(), mesh.ib.offset, cmd);

			LightmapPushConstants push;
			push.vb_pos_wind = mesh.vb_pos_wind.descriptor_srv;
			push.vb_nor = mesh.vb_nor.descriptor_srv;
			push.vb_atl = mesh.vb_atl.descriptor_srv;
			push.instanceIndex = objectIndex;
			device->PushConstants(&push, sizeof(push), cmd);

			RaytracingCB cb = {};
			cb.xTraceResolution.x = desc.width;
			cb.xTraceResolution.y = desc.height;
			cb.xTraceResolution_rcp.x = 1.0f / cb.xTraceResolution.x;
			cb.xTraceResolution_rcp.y = 1.0f / cb.xTraceResolution.y;
			cb.xTraceAccumulationFactor = 1.0f / (object.lightmapIterationCount + 1.0f); // accumulation factor (alpha)
			cb.xTraceUserData.x = raytraceBounceCount;
			XMFLOAT4 halton = wi::math::GetHaltonSequence(object.lightmapIterationCount); // for jittering the rasterization (good for eliminating atlas border artifacts)
			cb.xTracePixelOffset.x = (halton.x * 2 - 1) * cb.xTraceResolution_rcp.x;
			cb.xTracePixelOffset.y = (halton.y * 2 - 1) * cb.xTraceResolution_rcp.y;
			cb.xTracePixelOffset.x *= 1.4f;	// boost the jitter by a bit
			cb.xTracePixelOffset.y *= 1.4f;	// boost the jitter by a bit
			uint8_t instanceInclusionMask = 0xFF;
			cb.xTraceUserData.y = instanceInclusionMask;
			cb.xTraceSampleIndex = object.lightmapIterationCount;
			device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(RaytracingCB), cmd);

			uint32_t indexStart = ~0u;
			uint32_t indexEnd = 0;

			uint32_t first_subset = 0;
			uint32_t last_subset = 0;
			mesh.GetLODSubsetRange(0, first_subset, last_subset);
			for (uint32_t subsetIndex = first_subset; subsetIndex < last_subset; ++subsetIndex)
			{
				const MeshComponent::MeshSubset& subset = mesh.subsets[subsetIndex];
				if (subset.indexCount == 0)
					continue;
				indexStart = std::min(indexStart, subset.indexOffset);
				indexEnd = std::max(indexEnd, subset.indexOffset + subset.indexCount);
			}

			if (indexEnd > indexStart)
			{
				const uint32_t indexCount = indexEnd - indexStart;
				device->DrawIndexed(indexCount, indexStart, 0, cmd);
				object.lightmapIterationCount++;
			}

			device->RenderPassEnd(cmd);

			device->Barrier(GPUBarrier::Aliasing(&lightmap_depth_tmp, &lightmap_color_tmp), cmd);

			// Expand opaque areas:
			{
				device->EventBegin("Lightmap expand", cmd);

				device->BindComputeShader(&shaders[CSTYPE_LIGHTMAP_EXPAND], cmd);

				// render -> lightmap
				{
					device->BindResource(&object.lightmap_render, 0, cmd);
					device->BindUAV(&object.lightmap, 0, cmd);

					device->Barrier(GPUBarrier::Image(&object.lightmap, object.lightmap.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

					device->Dispatch((desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, (desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, 1, cmd);

					device->Barrier(GPUBarrier::Image(&object.lightmap, ResourceState::UNORDERED_ACCESS, object.lightmap.desc.layout), cmd);
				}
				for (int repeat = 0; repeat < 2; ++repeat)
				{
					// lightmap -> temp
					{
						device->BindResource(&object.lightmap, 0, cmd);
						device->BindUAV(&lightmap_color_tmp, 0, cmd);

						device->Barrier(GPUBarrier::Image(&lightmap_color_tmp, lightmap_color_tmp.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

						device->Dispatch((desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, (desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, 1, cmd);

						device->Barrier(GPUBarrier::Image(&lightmap_color_tmp, ResourceState::UNORDERED_ACCESS, lightmap_color_tmp.desc.layout), cmd);
					}
					// temp -> lightmap
					{
						device->BindResource(&lightmap_color_tmp, 0, cmd);
						device->BindUAV(&object.lightmap, 0, cmd);

						device->Barrier(GPUBarrier::Image(&object.lightmap, object.lightmap.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

						device->Dispatch((desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, (desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, 1, cmd);

						device->Barrier(GPUBarrier::Image(&object.lightmap, ResourceState::UNORDERED_ACCESS, object.lightmap.desc.layout), cmd);
					}
				}
				device->EventEnd(cmd);
			}

			device->EventEnd(cmd);
		}
	}

	wi::profiler::EndRange(range);

}

void RefreshWetmaps(const Visibility& vis, CommandList cmd)
{
	if (!vis.scene->IsWetmapProcessingRequired())
		return;

	device->EventBegin("RefreshWetmaps", cmd);

	BindCommonResources(cmd);
	device->BindComputeShader(&shaders[CSTYPE_WETMAP_UPDATE], cmd);

	WetmapPush push = {};
	push.rain_amount = vis.scene->weather.rain_amount;

	// Note: every object wetmap is updated, not just visible
	for (uint32_t objectIndex = 0; objectIndex < vis.scene->objects.GetCount(); ++objectIndex)
	{
		const ObjectComponent& object = vis.scene->objects[objectIndex];
		if (!object.wetmap.IsValid())
			continue;

		uint32_t vertexCount = uint32_t(object.wetmap.desc.size / GetFormatStride(object.wetmap.desc.format));

		push.wetmap = device->GetDescriptorIndex(&object.wetmap, SubresourceType::UAV);

		if (push.wetmap < 0)
			continue;

		push.instanceID = objectIndex;
		device->PushConstants(&push, sizeof(push), cmd);

		device->Dispatch((vertexCount + 63u) / 64u, 1, 1, cmd);
	}

	// Note: only visible hair particles will be updated, becasue invisible ones will not have valid vertices
	for (uint32_t hairIndex : vis.visibleHairs)
	{
		const wi::HairParticleSystem& hair = vis.scene->hairs[hairIndex];
		if (!hair.wetmap.IsValid())
			continue;

		uint32_t vertexCount = uint32_t(hair.wetmap.size / sizeof(uint16_t));

		push.wetmap = hair.wetmap.descriptor_uav;

		if (push.wetmap < 0)
			continue;

		push.instanceID = uint32_t(vis.scene->objects.GetCount() + hairIndex);
		device->PushConstants(&push, sizeof(push), cmd);

		device->Dispatch((vertexCount + 63u) / 64u, 1, 1, cmd);
	}

	device->EventEnd(cmd);
}

void BindCommonResources(CommandList cmd)
{
	device->BindConstantBuffer(&buffers[BUFFERTYPE_FRAMECB], CBSLOT_RENDERER_FRAME, cmd);
}

void BindCameraCB(
	const CameraComponent& camera,
	const CameraComponent& camera_previous,
	const CameraComponent& camera_reflection,
	CommandList cmd
)
{
	CameraCB cb;
	cb.init();
	ShaderCamera& shadercam = cb.cameras[0];

	shadercam.options = camera.shadercamera_options;
	if (camera.IsOrtho())
	{
		shadercam.options |= SHADERCAMERA_OPTION_ORTHO;
	}

	XMMATRIX invVP = camera.GetInvViewProjection();

	XMStoreFloat4x4(&shadercam.view_projection, camera.GetViewProjection());
	XMStoreFloat4x4(&shadercam.view, camera.GetView());
	XMStoreFloat4x4(&shadercam.projection, camera.GetProjection());
	shadercam.position = camera.Eye;
	shadercam.distance_from_origin = XMVectorGetX(XMVector3Length(XMLoadFloat3(&shadercam.position)));
	XMStoreFloat4x4(&shadercam.inverse_view, camera.GetInvView());
	XMStoreFloat4x4(&shadercam.inverse_projection, camera.GetInvProjection());
	XMStoreFloat4x4(&shadercam.inverse_view_projection, invVP);
	shadercam.forward = camera.At;
	shadercam.up = camera.Up;
	shadercam.z_near = camera.zNearP;
	shadercam.z_far = camera.zFarP;
	shadercam.z_near_rcp = 1.0f / std::max(0.0001f, shadercam.z_near);
	shadercam.z_far_rcp = 1.0f / std::max(0.0001f, shadercam.z_far);
	shadercam.z_range = abs(shadercam.z_far - shadercam.z_near);
	shadercam.z_range_rcp = 1.0f / std::max(0.0001f, shadercam.z_range);
	shadercam.clip_plane = camera.clipPlane;
	shadercam.reflection_plane = camera_reflection.clipPlaneOriginal;

	static_assert(arraysize(camera.frustum.planes) == arraysize(shadercam.frustum.planes), "Mismatch!");
	for (int i = 0; i < arraysize(camera.frustum.planes); ++i)
	{
		shadercam.frustum.planes[i] = camera.frustum.planes[i];
	}

	XMStoreFloat4(&shadercam.frustum_corners.cornersNEAR[0], XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersNEAR[1], XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersNEAR[2], XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersNEAR[3], XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), invVP));

	XMStoreFloat4(&shadercam.frustum_corners.cornersFAR[0], XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersFAR[1], XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersFAR[2], XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), invVP));
	XMStoreFloat4(&shadercam.frustum_corners.cornersFAR[3], XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), invVP));

	shadercam.temporalaa_jitter = camera.jitter;
	shadercam.temporalaa_jitter_prev = camera_previous.jitter;

	XMStoreFloat4x4(&shadercam.previous_view, camera_previous.GetView());
	XMStoreFloat4x4(&shadercam.previous_projection, camera_previous.GetProjection());
	XMStoreFloat4x4(&shadercam.previous_view_projection, camera_previous.GetViewProjection());
	XMStoreFloat4x4(&shadercam.previous_inverse_view_projection, camera_previous.GetInvViewProjection());
	XMStoreFloat4x4(&shadercam.reflection_view_projection, camera_reflection.GetViewProjection());
	XMStoreFloat4x4(&shadercam.reflection_inverse_view_projection, camera_reflection.GetInvViewProjection());
	XMStoreFloat4x4(&shadercam.reprojection, camera.GetInvViewProjection() * camera_previous.GetViewProjection());

	shadercam.focal_length = camera.focal_length;
	shadercam.aperture_size = camera.aperture_size;
	shadercam.aperture_shape = camera.aperture_shape;

	shadercam.canvas_size = float2(camera.canvas.GetLogicalWidth(), camera.canvas.GetLogicalHeight());
	shadercam.canvas_size_rcp = float2(1.0f / shadercam.canvas_size.x, 1.0f / shadercam.canvas_size.y);
	shadercam.internal_resolution = uint2((uint)camera.width, (uint)camera.height);
	shadercam.internal_resolution_rcp = float2(1.0f / std::max(1u, shadercam.internal_resolution.x), 1.0f / std::max(1u, shadercam.internal_resolution.y));

	shadercam.scissor.x = camera.scissor.left;
	shadercam.scissor.y = camera.scissor.top;
	shadercam.scissor.z = camera.scissor.right;
	shadercam.scissor.w = camera.scissor.bottom;

	// scissor_uv is also offset by 0.5 (half pixel) to avoid going over last pixel center with bilinear sampler:
	shadercam.scissor_uv.x = (shadercam.scissor.x + 0.5f) * shadercam.internal_resolution_rcp.x;
	shadercam.scissor_uv.y = (shadercam.scissor.y + 0.5f) * shadercam.internal_resolution_rcp.y;
	shadercam.scissor_uv.z = (shadercam.scissor.z - 0.5f) * shadercam.internal_resolution_rcp.x;
	shadercam.scissor_uv.w = (shadercam.scissor.w - 0.5f) * shadercam.internal_resolution_rcp.y;

	shadercam.entity_culling_tilecount = GetEntityCullingTileCount(shadercam.internal_resolution);
	shadercam.entity_culling_tile_bucket_count_flat = shadercam.entity_culling_tilecount.x * shadercam.entity_culling_tilecount.y * SHADER_ENTITY_TILE_BUCKET_COUNT;
	shadercam.sample_count = camera.sample_count;
	shadercam.visibility_tilecount = GetVisibilityTileCount(shadercam.internal_resolution);
	shadercam.visibility_tilecount_flat = shadercam.visibility_tilecount.x * shadercam.visibility_tilecount.y;

	shadercam.texture_primitiveID_index = camera.texture_primitiveID_index;
	shadercam.texture_depth_index = camera.texture_depth_index;
	shadercam.texture_lineardepth_index = camera.texture_lineardepth_index;
	shadercam.texture_velocity_index = camera.texture_velocity_index;
	shadercam.texture_normal_index = camera.texture_normal_index;
	shadercam.texture_roughness_index = camera.texture_roughness_index;
	shadercam.buffer_entitytiles_index = camera.buffer_entitytiles_index;
	shadercam.texture_reflection_index = camera.texture_reflection_index;
	shadercam.texture_reflection_depth_index = camera.texture_reflection_depth_index;
	shadercam.texture_refraction_index = camera.texture_refraction_index;
	shadercam.texture_waterriples_index = camera.texture_waterriples_index;
	shadercam.texture_ao_index = camera.texture_ao_index;
	shadercam.texture_ssr_index = camera.texture_ssr_index;
	shadercam.texture_ssgi_index = camera.texture_ssgi_index;
	shadercam.texture_rtshadow_index = camera.texture_rtshadow_index;
	shadercam.texture_rtdiffuse_index = camera.texture_rtdiffuse_index;
	shadercam.texture_surfelgi_index = camera.texture_surfelgi_index;
	shadercam.texture_depth_index_prev = camera_previous.texture_depth_index;
	shadercam.texture_vxgi_diffuse_index = camera.texture_vxgi_diffuse_index;
	shadercam.texture_vxgi_specular_index = camera.texture_vxgi_specular_index;
	shadercam.texture_reprojected_depth_index = camera.texture_reprojected_depth_index;

	device->BindDynamicConstantBuffer(cb, CBSLOT_RENDERER_CAMERA, cmd);
}

void CreateLuminanceResources(LuminanceResources& res, XMUINT2 resolution)
{
	float values[LUMINANCE_NUM_HISTOGRAM_BINS + 1 + 1] = {}; // 1 exposure + 1 luminance value + histogram
	GPUBufferDesc desc;
	desc.size = sizeof(values);
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.misc_flags = ResourceMiscFlag::BUFFER_RAW;
	device->CreateBuffer(&desc, values, &res.luminance);
	device->SetName(&res.luminance, "luminance");
}
void ComputeLuminance(
	const LuminanceResources& res,
	const Texture& sourceImage,
	CommandList cmd,
	float adaption_rate,
	float eyeadaptionkey
)
{
	device->EventBegin("Compute Luminance", cmd);
	auto range = wi::profiler::BeginRangeGPU("Luminance", cmd);

	PostProcess postprocess;
	postprocess.resolution.x = sourceImage.desc.width / 2;
	postprocess.resolution.y = sourceImage.desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	luminance_adaptionrate = adaption_rate;
	luminance_log_min = -10.0f;
	luminance_log_max = 2.0f;
	luminance_log_range = luminance_log_max - luminance_log_min;
	luminance_log_range_rcp = 1.0f / luminance_log_range;
	luminance_pixelcount = float(postprocess.resolution.x * postprocess.resolution.y);
	luminance_eyeadaptionkey = eyeadaptionkey;

	device->BindUAV(&res.luminance, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&res.luminance, ResourceState::SHADER_RESOURCE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Pass 1 : Compute log luminance and reduction
	{
		device->BindComputeShader(&shaders[CSTYPE_LUMINANCE_PASS1], cmd);
		device->BindResource(&sourceImage, 0, cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(
			(postprocess.resolution.x + LUMINANCE_BLOCKSIZE - 1) / LUMINANCE_BLOCKSIZE,
			(postprocess.resolution.y + LUMINANCE_BLOCKSIZE - 1) / LUMINANCE_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}

	// Pass 2 : Reduce into 1x1 texture
	{
		device->BindComputeShader(&shaders[CSTYPE_LUMINANCE_PASS2], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(1, 1, 1, cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.luminance, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateBloomResources(BloomResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.format = Format::R11G11B10_FLOAT;
	desc.width = resolution.x / 4;
	desc.height = resolution.y / 4;
	desc.mip_levels = std::min(5u, (uint32_t)std::log2(std::max(desc.width, desc.height)));
	device->CreateTexture(&desc, nullptr, &res.texture_bloom);
	device->SetName(&res.texture_bloom, "bloom.texture_bloom");
	device->CreateTexture(&desc, nullptr, &res.texture_temp);
	device->SetName(&res.texture_temp, "bloom.texture_temp");

	for (uint32_t i = 0; i < res.texture_bloom.desc.mip_levels; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&res.texture_bloom, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_temp, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_bloom, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_temp, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}
}
void ComputeBloom(
	const BloomResources& res,
	const Texture& input,
	CommandList cmd,
	float threshold,
	float exposure,
	const GPUBuffer* buffer_luminance
)
{
	device->EventBegin("Bloom", cmd);
	auto range = wi::profiler::BeginRangeGPU("Bloom", cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_bloom, res.texture_bloom.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temp, res.texture_temp.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&res.texture_bloom, 0, cmd);
	device->ClearUAV(&res.texture_temp, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(&res.texture_bloom),
			GPUBarrier::Memory(&res.texture_temp),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Separate bright parts of image to bloom texture:
	{
		device->EventBegin("Bloom Separate", cmd);

		const TextureDesc& desc = res.texture_bloom.GetDesc();

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_BLOOMSEPARATE], cmd);

		Bloom bloom;
		bloom.resolution_rcp.x = 1.0f / desc.width;
		bloom.resolution_rcp.y = 1.0f / desc.height;
		bloom.threshold = threshold;
		bloom.exposure = exposure;
		bloom.texture_input = device->GetDescriptorIndex(&input, SubresourceType::SRV);
		bloom.texture_output = device->GetDescriptorIndex(&res.texture_bloom, SubresourceType::UAV);
		bloom.buffer_input_luminance = device->GetDescriptorIndex(buffer_luminance, SubresourceType::SRV);
		device->PushConstants(&bloom, sizeof(bloom), cmd);

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_bloom, ResourceState::UNORDERED_ACCESS, res.texture_bloom.desc.layout),
			GPUBarrier::Image(&res.texture_temp, ResourceState::UNORDERED_ACCESS, res.texture_temp.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Bloom Mipchain", cmd);
	MIPGEN_OPTIONS mipopt;
	mipopt.gaussian_temp = &res.texture_temp;
	mipopt.wide_gauss = true;
	GenerateMipChain(res.texture_bloom, MIPGENFILTER_GAUSSIAN, cmd, mipopt);
	device->EventEnd(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void ComputeShadingRateClassification(
	const Texture& output,
	const Texture& debugUAV,
	CommandList cmd
)
{
	device->EventBegin("ComputeShadingRateClassification", cmd);
	auto range = wi::profiler::BeginRangeGPU("ComputeShadingRateClassification", cmd);

	if (GetVariableRateShadingClassificationDebug())
	{
		device->BindUAV(&debugUAV, 1, cmd);

		device->BindComputeShader(&shaders[CSTYPE_SHADINGRATECLASSIFICATION_DEBUG], cmd);
	}
	else
	{
		device->BindComputeShader(&shaders[CSTYPE_SHADINGRATECLASSIFICATION], cmd);
	}

	const TextureDesc& desc = output.GetDesc();

	ShadingRateClassification shadingrate = {}; // zero init the shading rates!
	shadingrate.TileSize = device->GetVariableRateShadingTileSize();
	device->WriteShadingRateValue(ShadingRate::RATE_1X1, &shadingrate.SHADING_RATE_1X1);
	device->WriteShadingRateValue(ShadingRate::RATE_1X2, &shadingrate.SHADING_RATE_1X2);
	device->WriteShadingRateValue(ShadingRate::RATE_2X1, &shadingrate.SHADING_RATE_2X1);
	device->WriteShadingRateValue(ShadingRate::RATE_2X2, &shadingrate.SHADING_RATE_2X2);
	device->WriteShadingRateValue(ShadingRate::RATE_2X4, &shadingrate.SHADING_RATE_2X4);
	device->WriteShadingRateValue(ShadingRate::RATE_4X2, &shadingrate.SHADING_RATE_4X2);
	device->WriteShadingRateValue(ShadingRate::RATE_4X4, &shadingrate.SHADING_RATE_4X4);
	device->PushConstants(&shadingrate, sizeof(shadingrate), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	// Whole threadgroup for each tile:
	device->Dispatch(desc.width, desc.height, 1, cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateVisibilityResources(VisibilityResources& res, XMUINT2 resolution)
{
	res.tile_count = GetVisibilityTileCount(resolution);
	{
		GPUBufferDesc desc;
		desc.stride = sizeof(ShaderTypeBin);
		desc.size = desc.stride * (MaterialComponent::SHADERTYPE_COUNT + 1); // +1 for sky
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED | ResourceMiscFlag::INDIRECT_ARGS;
		bool success = device->CreateBuffer(&desc, nullptr, &res.bins);
		assert(success);
		device->SetName(&res.bins, "res.bins");

		desc.stride = sizeof(VisibilityTile);
		desc.size = desc.stride * res.tile_count.x * res.tile_count.y * (MaterialComponent::SHADERTYPE_COUNT + 1); // +1 for sky
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
		success = device->CreateBuffer(&desc, nullptr, &res.binned_tiles);
		assert(success);
		device->SetName(&res.binned_tiles, "res.binned_tiles");
	}
	{
		TextureDesc desc;
		desc.width = resolution.x;
		desc.height = resolution.y;
		desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
		desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

		desc.format = Format::R16G16_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.texture_normals);
		device->SetName(&res.texture_normals, "res.texture_normals");

		desc.format = Format::R8_UNORM;
		device->CreateTexture(&desc, nullptr, &res.texture_roughness);
		device->SetName(&res.texture_roughness, "res.texture_roughness");

		desc.format = Format::R32G32B32A32_UINT;
		device->CreateTexture(&desc, nullptr, &res.texture_payload_0);
		device->SetName(&res.texture_payload_0, "res.texture_payload_0");
		device->CreateTexture(&desc, nullptr, &res.texture_payload_1);
		device->SetName(&res.texture_payload_1, "res.texture_payload_1");
	}
}
void Visibility_Prepare(
	const VisibilityResources& res,
	const Texture& input_primitiveID, // can be MSAA
	CommandList cmd
)
{
	device->EventBegin("Visibility_Prepare", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Prepare", cmd);

	BindCommonResources(cmd);

	// Note: the tile_count here must be valid whether the VisibilityResources was created or not!
	XMUINT2 tile_count = GetVisibilityTileCount(XMUINT2(input_primitiveID.desc.width, input_primitiveID.desc.height));

	// Beginning barriers, clears:
	if(res.IsValid())
	{
		ShaderTypeBin bins[SHADERTYPE_BIN_COUNT + 1];
		for (uint i = 0; i < arraysize(bins); ++i)
		{
			ShaderTypeBin& bin = bins[i];
			bin.dispatchX = 0; // will be used for atomic add in shader
			bin.dispatchY = 1;
			bin.dispatchZ = 1;
			bin.shaderType = i;
		}
		device->UpdateBuffer(&res.bins, bins, cmd);
		PushBarrier(GPUBarrier::Buffer(&res.bins, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS));
		PushBarrier(GPUBarrier::Buffer(&res.binned_tiles, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		FlushBarriers(cmd);
	}

	// Resolve:
	//	PrimitiveID -> depth, lineardepth
	//	Binning classification
	{
		device->EventBegin("Resolve", cmd);
		const bool msaa = input_primitiveID.GetDesc().sample_count > 1;

		device->BindResource(&input_primitiveID, 0, cmd);

		GPUResource unbind;

		if (res.IsValid())
		{
			device->BindUAV(&res.bins, 0, cmd);
			device->BindUAV(&res.binned_tiles, 1, cmd);
		}
		else
		{
			device->BindUAV(&unbind, 0, cmd);
			device->BindUAV(&unbind, 1, cmd);
		}

		if (res.depthbuffer)
		{
			device->BindUAV(res.depthbuffer, 3, cmd, 0);
			device->BindUAV(res.depthbuffer, 4, cmd, 1);
			device->BindUAV(res.depthbuffer, 5, cmd, 2);
			device->BindUAV(res.depthbuffer, 6, cmd, 3);
			device->BindUAV(res.depthbuffer, 7, cmd, 4);
			PushBarrier(GPUBarrier::Image(res.depthbuffer, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 3, cmd);
			device->BindUAV(&unbind, 4, cmd);
			device->BindUAV(&unbind, 5, cmd);
			device->BindUAV(&unbind, 6, cmd);
			device->BindUAV(&unbind, 7, cmd);
		}
		if (res.lineardepth)
		{
			device->BindUAV(res.lineardepth, 8, cmd, 0);
			device->BindUAV(res.lineardepth, 9, cmd, 1);
			device->BindUAV(res.lineardepth, 10, cmd, 2);
			device->BindUAV(res.lineardepth, 11, cmd, 3);
			device->BindUAV(res.lineardepth, 12, cmd, 4);
			PushBarrier(GPUBarrier::Image(res.lineardepth, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 8, cmd);
			device->BindUAV(&unbind, 9, cmd);
			device->BindUAV(&unbind, 10, cmd);
			device->BindUAV(&unbind, 11, cmd);
			device->BindUAV(&unbind, 12, cmd);
		}
		if (res.primitiveID_resolved)
		{
			device->BindUAV(res.primitiveID_resolved, 13, cmd);
			PushBarrier(GPUBarrier::Image(res.primitiveID_resolved, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
		}
		else
		{
			device->BindUAV(&unbind, 13, cmd);
		}
		FlushBarriers(cmd);

		if (res.depthbuffer)
		{
			device->ClearUAV(res.depthbuffer, 0, cmd);
		}
		if (res.lineardepth)
		{
			device->ClearUAV(res.lineardepth, 0, cmd);
		}
		if (res.primitiveID_resolved)
		{
			device->ClearUAV(res.primitiveID_resolved, 0, cmd);
		}
		device->Barrier(GPUBarrier::Memory(), cmd);

		device->BindComputeShader(&shaders[msaa ? CSTYPE_VISIBILITY_RESOLVE_MSAA : CSTYPE_VISIBILITY_RESOLVE], cmd);

		device->Dispatch(
			tile_count.x,
			tile_count.y,
			1,
			cmd
		);

		if (res.depthbuffer)
		{
			PushBarrier(GPUBarrier::Image(res.depthbuffer, ResourceState::UNORDERED_ACCESS, res.depthbuffer->desc.layout));
		}
		if (res.lineardepth)
		{
			PushBarrier(GPUBarrier::Image(res.lineardepth, ResourceState::UNORDERED_ACCESS, res.lineardepth->desc.layout));
		}
		if (res.primitiveID_resolved)
		{
			PushBarrier(GPUBarrier::Image(res.primitiveID_resolved, ResourceState::UNORDERED_ACCESS, res.primitiveID_resolved->desc.layout));
		}
		if (res.IsValid())
		{
			PushBarrier(GPUBarrier::Buffer(&res.bins, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT));
			PushBarrier(GPUBarrier::Buffer(&res.binned_tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE));
		}
		FlushBarriers(cmd);

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Surface(
	const VisibilityResources& res,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Surface", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Surface", cmd);

	BindCommonResources(cmd);

	// First, do a bunch of resource discards to initialize texture metadata:
	PushBarrier(GPUBarrier::Image(&output, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&res.texture_normals, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&res.texture_roughness, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&res.texture_payload_0, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&res.texture_payload_1, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS));
	FlushBarriers(cmd);

	device->ClearUAV(&res.texture_normals, 0, cmd);
	device->ClearUAV(&res.texture_roughness, 0, cmd);
	device->ClearUAV(&res.texture_payload_0, 0, cmd);
	device->ClearUAV(&res.texture_payload_1, 0, cmd);
	device->Barrier(GPUBarrier::Memory(), cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindUAV(&output, 0, cmd);
	device->BindUAV(&res.texture_normals, 1, cmd);
	device->BindUAV(&res.texture_roughness, 2, cmd);
	device->BindUAV(&res.texture_payload_0, 3, cmd);
	device->BindUAV(&res.texture_payload_1, 4, cmd);

	const uint visibility_tilecount_flat = res.tile_count.x * res.tile_count.y;
	uint visibility_tile_offset = 0;

	// surface dispatches per material type:
	device->EventBegin("Surface parameters", cmd);
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SURFACE_PERMUTATION_BEGIN + i], cmd);
		device->PushConstants(&visibility_tile_offset, sizeof(visibility_tile_offset), cmd);
		device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
		visibility_tile_offset += visibility_tilecount_flat;
	}
	device->EventEnd(cmd);

	// sky dispatch:
	device->EventBegin("Sky", cmd);
	device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SKY], cmd);
	device->PushConstants(&visibility_tile_offset, sizeof(visibility_tile_offset), cmd);
	device->DispatchIndirect(&res.bins, MaterialComponent::SHADERTYPE_COUNT * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
	device->EventEnd(cmd);

	// Ending barriers:
	//	These resources will be used by other post processing effects
	PushBarrier(GPUBarrier::Image(&res.texture_normals, ResourceState::UNORDERED_ACCESS, res.texture_normals.desc.layout));
	PushBarrier(GPUBarrier::Image(&res.texture_roughness, ResourceState::UNORDERED_ACCESS, res.texture_roughness.desc.layout));
	FlushBarriers(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Surface_Reduced(
	const VisibilityResources& res,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Surface_Reduced", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Surface_Reduced", cmd);

	BindCommonResources(cmd);

	PushBarrier(GPUBarrier::Image(&res.texture_normals, res.texture_normals.desc.layout, ResourceState::UNORDERED_ACCESS));
	PushBarrier(GPUBarrier::Image(&res.texture_roughness, res.texture_roughness.desc.layout, ResourceState::UNORDERED_ACCESS));
	FlushBarriers(cmd);

	device->ClearUAV(&res.texture_normals, 0, cmd);
	device->ClearUAV(&res.texture_roughness, 0, cmd);
	device->Barrier(GPUBarrier::Memory(), cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindUAV(&res.texture_normals, 1, cmd);
	device->BindUAV(&res.texture_roughness, 2, cmd);

	const uint visibility_tilecount_flat = res.tile_count.x * res.tile_count.y;
	uint visibility_tile_offset = 0;

	// surface dispatches per material type:
	device->EventBegin("Surface parameters", cmd);
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		if (i != MaterialComponent::SHADERTYPE_UNLIT) // this won't need surface parameter write out
		{
			device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SURFACE_REDUCED_PERMUTATION_BEGIN + i], cmd);
			device->PushConstants(&visibility_tile_offset, sizeof(visibility_tile_offset), cmd);
			device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
		}
		visibility_tile_offset += visibility_tilecount_flat;
	}
	device->EventEnd(cmd);

	// Ending barriers:
	//	These resources will be used by other post processing effects
	PushBarrier(GPUBarrier::Image(&res.texture_normals, ResourceState::UNORDERED_ACCESS, res.texture_normals.desc.layout));
	PushBarrier(GPUBarrier::Image(&res.texture_roughness, ResourceState::UNORDERED_ACCESS, res.texture_roughness.desc.layout));
	FlushBarriers(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Shade(
	const VisibilityResources& res,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Shade", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Shade", cmd);

	BindCommonResources(cmd);

	PushBarrier(GPUBarrier::Image(&res.texture_payload_0, ResourceState::UNORDERED_ACCESS, res.texture_payload_0.desc.layout));
	PushBarrier(GPUBarrier::Image(&res.texture_payload_1, ResourceState::UNORDERED_ACCESS, res.texture_payload_1.desc.layout));
	FlushBarriers(cmd);

	device->BindResource(&res.binned_tiles, 0, cmd);
	device->BindResource(&res.texture_payload_0, 2, cmd);
	device->BindResource(&res.texture_payload_1, 3, cmd);
	device->BindUAV(&output, 0, cmd);

	const uint visibility_tilecount_flat = res.tile_count.x * res.tile_count.y;
	uint visibility_tile_offset = 0;

	// shading dispatches per material type:
	for (uint i = 0; i < MaterialComponent::SHADERTYPE_COUNT; ++i)
	{
		if (i != MaterialComponent::SHADERTYPE_UNLIT && i != MaterialComponent::SHADERTYPE_INTERIORMAPPING) // these shaders are special, already written out their final color in the surface shader
		{
			device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_SHADE_PERMUTATION_BEGIN + i], cmd);
			device->PushConstants(&visibility_tile_offset, sizeof(visibility_tile_offset), cmd);
			device->DispatchIndirect(&res.bins, i * sizeof(ShaderTypeBin) + offsetof(ShaderTypeBin, dispatchX), cmd);
		}
		visibility_tile_offset += visibility_tilecount_flat;
	}

	PushBarrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout));
	FlushBarriers(cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Visibility_Velocity(
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Visibility_Velocity", cmd);
	auto range = wi::profiler::BeginRangeGPU("Visibility_Velocity", cmd);

	BindCommonResources(cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->BindComputeShader(&shaders[CSTYPE_VISIBILITY_VELOCITY], cmd);
	device->BindUAV(&output, 0, cmd);
	device->Dispatch(
		(output.desc.width + 7u) / 8u,
		(output.desc.height + 7u) / 8u,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

void CreateSurfelGIResources(SurfelGIResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.format = Format::R11G11B10_FLOAT;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	device->CreateTexture(&desc, nullptr, &res.result_halfres);
	device->SetName(&res.result_halfres, "surfelGI.result_halfres");
	desc.width = resolution.x;
	desc.height = resolution.y;
	device->CreateTexture(&desc, nullptr, &res.result);
	device->SetName(&res.result, "surfelGI.result");
}
void SurfelGI_Coverage(
	const SurfelGIResources& res,
	const Scene& scene,
	const Texture& lineardepth,
	const Texture& debugUAV,
	CommandList cmd
)
{
	device->EventBegin("SurfelGI - Coverage", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("SurfelGI - Coverage", cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&scene.surfelgi.statsBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.result_halfres, res.result_halfres.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.result, res.result.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}
	device->ClearUAV(&res.result_halfres, 0, cmd);
	device->ClearUAV(&res.result, 0, cmd);
	device->Barrier(GPUBarrier::Memory(), cmd);


	// Coverage:
	{
		device->EventBegin("Coverage", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_COVERAGE], cmd);

		SurfelDebugPushConstants push;
		push.debug = GetSurfelGIDebugEnabled();
		device->PushConstants(&push, sizeof(push), cmd);

		device->BindResource(&scene.surfelgi.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelgi.gridBuffer, 1, cmd);
		device->BindResource(&scene.surfelgi.cellBuffer, 2, cmd);
		device->BindResource(&scene.surfelgi.momentsTexture, 3, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.dataBuffer,
			&scene.surfelgi.deadBuffer,
			&scene.surfelgi.aliveBuffer[1],
			&scene.surfelgi.statsBuffer,
			&res.result_halfres,
			&debugUAV
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.result_halfres.desc.width + 15) / 16,
			(res.result_halfres.desc.height + 15) / 16,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.result_halfres, ResourceState::UNORDERED_ACCESS, res.result_halfres.desc.layout),
				GPUBarrier::Image(&res.result, ResourceState::UNORDERED_ACCESS, res.result.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// surfel count -> indirect args (for next frame):
	{
		device->EventBegin("Indirect args", cmd);
		const GPUResource* uavs[] = {
			&scene.surfelgi.statsBuffer,
			&scene.surfelgi.indirectBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_INDIRECTPREPARE], cmd);
		device->Dispatch(1, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	Postprocess_Upsample_Bilateral(
		res.result_halfres,
		lineardepth,
		res.result,
		cmd,
		false,
		2
	);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void SurfelGI(
	const SurfelGIResources& res,
	const Scene& scene,
	CommandList cmd
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	auto prof_range = wi::profiler::BeginRangeGPU("SurfelGI", cmd);
	device->EventBegin("SurfelGI", cmd);

	BindCommonResources(cmd);

	if (!scene.surfelgi.cleared)
	{
		scene.surfelgi.cleared = true;
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&scene.surfelgi.momentsTexture, scene.surfelgi.momentsTexture.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&scene.surfelgi.momentsTexture, 0, cmd);
		device->ClearUAV(&scene.surfelgi.varianceBuffer, 0, cmd);
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->Barrier(GPUBarrier::Memory(), cmd);
	}

	// Grid reset:
	{
		device->EventBegin("Grid Reset", cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.gridBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->ClearUAV(&scene.surfelgi.gridBuffer, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&scene.surfelgi.gridBuffer), cmd);

		device->EventEnd(cmd);
	}

	// Update:
	{
		device->EventBegin("Update", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_UPDATE], cmd);

		device->BindResource(&scene.surfelgi.aliveBuffer[0], 1, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.surfelBuffer,
			&scene.surfelgi.gridBuffer,
			&scene.surfelgi.aliveBuffer[1],
			&scene.surfelgi.deadBuffer,
			&scene.surfelgi.statsBuffer,
			&scene.surfelgi.rayBuffer,
			&scene.surfelgi.dataBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.surfelgi.indirectBuffer, SURFEL_INDIRECT_OFFSET_ITERATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.surfelBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Buffer(&scene.surfelgi.dataBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Grid offsets:
	{
		device->EventBegin("Grid Offsets", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_GRIDOFFSETS], cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.gridBuffer,
			&scene.surfelgi.cellBuffer,
			&scene.surfelgi.statsBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.cellBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(SURFEL_TABLE_SIZE + 63) / 64,
			1,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.statsBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Binning:
	{
		device->EventBegin("Binning", cmd);
		device->BindComputeShader(&shaders[CSTYPE_SURFEL_BINNING], cmd);

		device->BindResource(&scene.surfelgi.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelgi.aliveBuffer[0], 1, cmd);
		device->BindResource(&scene.surfelgi.statsBuffer, 2, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.gridBuffer,
			&scene.surfelgi.cellBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.surfelgi.indirectBuffer, SURFEL_INDIRECT_OFFSET_ITERATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.gridBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Buffer(&scene.surfelgi.cellBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Raytracing:
	{
		device->EventBegin("Raytrace", cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_RAYTRACE], cmd);

		PushConstantsSurfelRaytrace push;
		uint8_t instanceInclusionMask = 0xFF;
		push.instanceInclusionMask = instanceInclusionMask;
		device->PushConstants(&push, sizeof(push), cmd);

		device->BindResource(&scene.surfelgi.surfelBuffer, 0, cmd);
		device->BindResource(&scene.surfelgi.statsBuffer, 1, cmd);
		device->BindResource(&scene.surfelgi.gridBuffer, 2, cmd);
		device->BindResource(&scene.surfelgi.cellBuffer, 3, cmd);
		device->BindResource(&scene.surfelgi.aliveBuffer[0], 4, cmd);
		device->BindResource(&scene.surfelgi.momentsTexture, 5, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.rayBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.surfelgi.indirectBuffer, SURFEL_INDIRECT_OFFSET_RAYTRACE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.rayBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Integrate rays:
	{
		device->EventBegin("Integrate", cmd);

		device->BindComputeShader(&shaders[CSTYPE_SURFEL_INTEGRATE], cmd);

		device->BindResource(&scene.surfelgi.statsBuffer, 0, cmd);
		device->BindResource(&scene.surfelgi.gridBuffer, 1, cmd);
		device->BindResource(&scene.surfelgi.cellBuffer, 2, cmd);
		device->BindResource(&scene.surfelgi.aliveBuffer[0], 3, cmd);
		device->BindResource(&scene.surfelgi.rayBuffer, 4, cmd);

		const GPUResource* uavs[] = {
			&scene.surfelgi.dataBuffer,
			&scene.surfelgi.varianceBuffer,
			&scene.surfelgi.momentsTexture,
			&scene.surfelgi.surfelBuffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.dataBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Buffer(&scene.surfelgi.surfelBuffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&scene.surfelgi.momentsTexture, scene.surfelgi.momentsTexture.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->DispatchIndirect(&scene.surfelgi.indirectBuffer, SURFEL_INDIRECT_OFFSET_INTEGRATE, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&scene.surfelgi.dataBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Buffer(&scene.surfelgi.surfelBuffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Image(&scene.surfelgi.momentsTexture, ResourceState::UNORDERED_ACCESS, scene.surfelgi.momentsTexture.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}

void DDGI(
	const wi::scene::Scene& scene,
	CommandList cmd
)
{
	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (GetDDGIRayCount() == 0)
		return;

	if (!scene.ddgi.ray_buffer.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	auto prof_range = wi::profiler::BeginRangeGPU("DDGI", cmd);
	device->EventBegin("DDGI", cmd);

	if (scene.ddgi.frame_index == 0)
	{
		device->Barrier(GPUBarrier::Image(&scene.ddgi.depth_texture, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS), cmd);
		device->Barrier(GPUBarrier::Buffer(&scene.ddgi.probe_buffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS), cmd);
		device->Barrier(GPUBarrier::Buffer(&scene.ddgi.variance_buffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS), cmd);
		device->ClearUAV(&scene.ddgi.probe_buffer, 0, cmd);
		device->ClearUAV(&scene.ddgi.depth_texture, 0, cmd);
		device->ClearUAV(&scene.ddgi.variance_buffer, 0, cmd);
		device->ClearUAV(&scene.ddgi.rayallocation_buffer, 0, cmd);
		device->ClearUAV(&scene.ddgi.raycount_buffer, 0, cmd);
		device->ClearUAV(&scene.ddgi.ray_buffer, 0, cmd);
		device->Barrier(GPUBarrier::Memory(), cmd);
		device->Barrier(GPUBarrier::Image(&scene.ddgi.depth_texture, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE), cmd);
		device->Barrier(GPUBarrier::Buffer(&scene.ddgi.probe_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE), cmd);
		device->Barrier(GPUBarrier::Buffer(&scene.ddgi.variance_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE), cmd);
	}

	BindCommonResources(cmd);

	DDGIPushConstants push;
	uint8_t instanceInclusionMask = 0xFF;
	push.instanceInclusionMask = instanceInclusionMask;
	push.frameIndex = scene.ddgi.frame_index;
	push.rayCount = std::min(GetDDGIRayCount(), DDGI_MAX_RAYCOUNT);
	push.blendSpeed = GetDDGIBlendSpeed();

	// Ray allocation:
	{
		device->EventBegin("Ray allocation", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_RAYALLOCATION], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		const GPUResource* res[] = {
			&scene.ddgi.variance_buffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgi.rayallocation_buffer,
			&scene.ddgi.raycount_buffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->ClearUAV(&scene.ddgi.rayallocation_buffer, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&scene.ddgi.rayallocation_buffer), cmd);

		device->Dispatch(scene.shaderscene.ddgi.probe_count, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(&scene.ddgi.rayallocation_buffer),
			GPUBarrier::Buffer(&scene.ddgi.raycount_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Indirect prepare:
	{
		device->EventBegin("Indirect prepare", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_INDIRECTPREPARE], cmd);

		const GPUResource* uavs[] = {
			&scene.ddgi.rayallocation_buffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(1, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&scene.ddgi.rayallocation_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT | ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Raytracing:
	{
		device->EventBegin("Raytrace", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_RAYTRACE], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		MiscCB cb = {};
		float angle = wi::random::GetRandom(0.0f, 1.0f) * XM_2PI;
		XMVECTOR axis = XMVectorSet(
			wi::random::GetRandom(-1.0f, 1.0f),
			wi::random::GetRandom(-1.0f, 1.0f),
			wi::random::GetRandom(-1.0f, 1.0f),
			0
		);
		axis = XMVector3Normalize(axis);
		XMStoreFloat4x4(&cb.g_xTransform, XMMatrixRotationAxis(axis, angle));
		device->BindDynamicConstantBuffer(cb, CB_GETBINDSLOT(MiscCB), cmd);

		const GPUResource* res[] = {
			&scene.ddgi.rayallocation_buffer,
			&scene.ddgi.raycount_buffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgi.ray_buffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->DispatchIndirect(&scene.ddgi.rayallocation_buffer, 0, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&scene.ddgi.ray_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Image(&scene.ddgi.depth_texture, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&scene.ddgi.variance_buffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&scene.ddgi.raycount_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Buffer(&scene.ddgi.probe_buffer, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Update:
	{
		device->EventBegin("Update", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_UPDATE], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		const GPUResource* res[] = {
			&scene.ddgi.ray_buffer,
			&scene.ddgi.raycount_buffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgi.variance_buffer,
			&scene.ddgi.probe_buffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(scene.shaderscene.ddgi.probe_count, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	// Update Depth:
	{
		device->EventBegin("Update Depth", cmd);

		device->BindComputeShader(&shaders[CSTYPE_DDGI_UPDATE_DEPTH], cmd);
		device->PushConstants(&push, sizeof(push), cmd);

		const GPUResource* res[] = {
			&scene.ddgi.ray_buffer,
			&scene.ddgi.raycount_buffer,
		};
		device->BindResources(res, 0, arraysize(res), cmd);

		const GPUResource* uavs[] = {
			&scene.ddgi.depth_texture,
			&scene.ddgi.probe_buffer,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(scene.shaderscene.ddgi.probe_count, 1, 1, cmd);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&scene.ddgi.probe_buffer, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
			GPUBarrier::Image(&scene.ddgi.depth_texture, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}

void Postprocess_Blur_Gaussian(
	const Texture& input,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	int mip_src,
	int mip_dst,
	bool wide
)
{
	device->EventBegin("Postprocess_Blur_Gaussian", cmd);

	SHADERTYPE cs = CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4;
	switch (output.GetDesc().format)
	{
	case Format::R16_UNORM:
	case Format::R8_UNORM:
	case Format::R16_FLOAT:
	case Format::R32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT1 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT1;
		break;
	case Format::R11G11B10_FLOAT:
	case Format::R16G16B16A16_UNORM:
	case Format::R8G8B8A8_UNORM:
	case Format::B8G8R8A8_UNORM:
	case Format::R10G10B10A2_UNORM:
	case Format::R16G16B16A16_FLOAT:
	case Format::R32G32B32A32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_WIDE_FLOAT4 : CSTYPE_POSTPROCESS_BLUR_GAUSSIAN_FLOAT4;
		break;
	default:
		assert(0); // implement format!
		break;
	}
	device->BindComputeShader(&shaders[cs], cmd);

	PushBarrier(GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst));
	FlushBarriers(cmd);

	if (mip_dst < 0)
	{
		device->ClearUAV(&temp, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&temp), cmd);
	}
	
	// Horizontal:
	{
		const TextureDesc& desc = temp.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 1;
		postprocess.params0.y = 0;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd, mip_src);
		device->BindUAV(&temp, 0, cmd, mip_dst);

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
		);

	}

	PushBarrier(GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout, mip_dst));
	PushBarrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst));
	FlushBarriers(cmd);

	if (mip_dst < 0)
	{
		device->ClearUAV(&output, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&output), cmd);
	}

	// Vertical:
	{
		const TextureDesc& desc = output.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&temp, 0, cmd, mip_dst); // <- also mip_dst because it's second pass!
		device->BindUAV(&output, 0, cmd, mip_dst);

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			1,
			cmd
		);

		device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout, mip_dst), cmd);

	}

	device->EventEnd(cmd);
}
void Postprocess_Blur_Bilateral(
	const Texture& input,
	const Texture& lineardepth,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	float depth_threshold,
	int mip_src,
	int mip_dst,
	bool wide
)
{
	device->EventBegin("Postprocess_Blur_Bilateral", cmd);

	SHADERTYPE cs = CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4;
	switch (output.GetDesc().format)
	{
	case Format::R16_UNORM:
	case Format::R8_UNORM:
	case Format::R16_FLOAT:
	case Format::R32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT1 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT1;
		break;
	case Format::R11G11B10_FLOAT:
	case Format::R16G16B16A16_UNORM:
	case Format::R8G8B8A8_UNORM:
	case Format::B8G8R8A8_UNORM:
	case Format::R10G10B10A2_UNORM:
	case Format::R16G16B16A16_FLOAT:
	case Format::R32G32B32A32_FLOAT:
		cs = wide ? CSTYPE_POSTPROCESS_BLUR_BILATERAL_WIDE_FLOAT4 : CSTYPE_POSTPROCESS_BLUR_BILATERAL_FLOAT4;
		break;
	default:
		assert(0); // implement format!
		break;
	}
	device->BindComputeShader(&shaders[cs], cmd);

	PushBarrier(GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst));
	FlushBarriers(cmd);

	if (mip_dst < 0)
	{
		device->ClearUAV(&temp, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&temp), cmd);
	}

	// Horizontal:
	{
		const TextureDesc& desc = temp.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 1;
		postprocess.params0.y = 0;
		postprocess.params0.w = depth_threshold;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&input, 0, cmd, mip_src);
		device->BindUAV(&temp, 0, cmd, mip_dst);

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
		);

	}

	PushBarrier(GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout, mip_dst));
	PushBarrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS, mip_dst));
	FlushBarriers(cmd);

	if (mip_dst < 0)
	{
		device->ClearUAV(&output, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&output), cmd);
	}

	// Vertical:
	{
		const TextureDesc& desc = output.GetDesc();

		PostProcess postprocess;
		postprocess.resolution.x = desc.width;
		postprocess.resolution.y = desc.height;
		if (mip_dst > 0)
		{
			postprocess.resolution.x >>= mip_dst;
			postprocess.resolution.y >>= mip_dst;
		}
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		postprocess.params0.w = depth_threshold;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&temp, 0, cmd, mip_dst); // <- also mip_dst because it's second pass!
		device->BindUAV(&output, 0, cmd, mip_dst);

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT - 1) / POSTPROCESS_BLUR_GAUSSIAN_THREADCOUNT,
			1,
			cmd
		);

		device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout, mip_dst), cmd);

	}

	device->EventEnd(cmd);
}
void CreateSSAOResources(SSAOResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.format = Format::R8_UNORM;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::UNORDERED_ACCESS | BindFlag::SHADER_RESOURCE;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.temp);
}
void Postprocess_SSAO(
	const SSAOResources& res,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float range,
	uint32_t samplecount,
	float power
)
{
	device->EventBegin("Postprocess_SSAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("SSAO", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSAO], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	ssao_range = range;
	ssao_samplecount = (float)samplecount;
	ssao_power = power;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.temp, res.temp.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&output, 0, cmd);
	device->ClearUAV(&res.temp, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			GPUBarrier::Image(&res.temp, ResourceState::UNORDERED_ACCESS, res.temp.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}


	Postprocess_Blur_Bilateral(output, lineardepth, res.temp, output, cmd, 1.2f, -1, -1, true);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void Postprocess_HBAO(
	const SSAOResources& res,
	const CameraComponent& camera,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float power
)
{
	device->EventBegin("Postprocess_HBAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("HBAO", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_HBAO], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = 1;
	postprocess.params0.y = 0;
	hbao_power = power;

	// Load first element of projection matrix which is the cotangent of the horizontal FOV divided by 2.
	const float TanHalfFovH = 1.0f / camera.Projection.m[0][0];
	const float FocalLenX = 1.0f / TanHalfFovH * ((float)postprocess.resolution.y / (float)postprocess.resolution.x);
	const float FocalLenY = 1.0f / TanHalfFovH;
	const float InvFocalLenX = 1.0f / FocalLenX;
	const float InvFocalLenY = 1.0f / FocalLenY;
	const float UVToViewAX = 2.0f * InvFocalLenX;
	const float UVToViewAY = -2.0f * InvFocalLenY;
	const float UVToViewBX = -1.0f * InvFocalLenX;
	const float UVToViewBY = 1.0f * InvFocalLenY;
	postprocess.params1.x = UVToViewAX;
	postprocess.params1.y = UVToViewAY;
	postprocess.params1.z = UVToViewBX;
	postprocess.params1.w = UVToViewBY;

	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	// horizontal pass:
	{
		device->BindResource(wi::texturehelper::getWhite(), 0, cmd);
		const GPUResource* uavs[] = {
			&res.temp,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.temp, res.temp.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->ClearUAV(&output, 0, cmd);
		device->ClearUAV(&res.temp, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&res.temp), cmd);

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_HBAO_THREADCOUNT - 1) / POSTPROCESS_HBAO_THREADCOUNT,
			postprocess.resolution.y,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(&output),
				GPUBarrier::Image(&res.temp, ResourceState::UNORDERED_ACCESS, res.temp.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// vertical pass:
	{
		postprocess.params0.x = 0;
		postprocess.params0.y = 1;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.temp, 0, cmd);
		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			postprocess.resolution.x,
			(postprocess.resolution.y + POSTPROCESS_HBAO_THREADCOUNT - 1) / POSTPROCESS_HBAO_THREADCOUNT,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	//Postprocess_Blur_Bilateral(output, lineardepth, res.temp, output, cmd, 1.2f, -1, -1, true);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateMSAOResources(MSAOResources& res, XMUINT2 resolution)
{
	res.cleared = false;

	TextureDesc saved_desc;
	saved_desc.format = Format::R32_FLOAT;
	saved_desc.width = resolution.x;
	saved_desc.height = resolution.y;
	saved_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	saved_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	const uint32_t bufferWidth = saved_desc.width;
	const uint32_t bufferWidth1 = (bufferWidth + 1) / 2;
	const uint32_t bufferWidth2 = (bufferWidth + 3) / 4;
	const uint32_t bufferWidth3 = (bufferWidth + 7) / 8;
	const uint32_t bufferWidth4 = (bufferWidth + 15) / 16;
	const uint32_t bufferWidth5 = (bufferWidth + 31) / 32;
	const uint32_t bufferWidth6 = (bufferWidth + 63) / 64;
	const uint32_t bufferHeight = saved_desc.height;
	const uint32_t bufferHeight1 = (bufferHeight + 1) / 2;
	const uint32_t bufferHeight2 = (bufferHeight + 3) / 4;
	const uint32_t bufferHeight3 = (bufferHeight + 7) / 8;
	const uint32_t bufferHeight4 = (bufferHeight + 15) / 16;
	const uint32_t bufferHeight5 = (bufferHeight + 31) / 32;
	const uint32_t bufferHeight6 = (bufferHeight + 63) / 64;

	TextureDesc desc = saved_desc;
	desc.width = bufferWidth1;
	desc.height = bufferHeight1;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize1);
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled1);

	desc = saved_desc;
	desc.width = bufferWidth2;
	desc.height = bufferHeight2;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize2);
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled2);

	desc = saved_desc;
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize3);
	desc.width = bufferWidth5;
	desc.height = bufferHeight5;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled3);

	desc = saved_desc;
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_downsize4);
	desc.width = bufferWidth6;
	desc.height = bufferHeight6;
	desc.array_size = 16;
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_lineardepth_tiled4);

	desc = saved_desc;
	desc.format = Format::R8_UNORM;
	desc.width = bufferWidth1;
	desc.height = bufferHeight1;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged1);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq1);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth1);
	desc.width = bufferWidth2;
	desc.height = bufferHeight2;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged2);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq2);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth2);
	desc.width = bufferWidth3;
	desc.height = bufferHeight3;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged3);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq3);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_smooth3);
	desc.width = bufferWidth4;
	desc.height = bufferHeight4;
	device->CreateTexture(&desc, nullptr, &res.texture_ao_merged4);
	device->CreateTexture(&desc, nullptr, &res.texture_ao_hq4);
}
void Postprocess_MSAO(
	const MSAOResources& res,
	const CameraComponent& camera,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float power
	)
{
	device->EventBegin("Postprocess_MSAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("MSAO", cmd);

	// Pre-clear:
	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_downsize1, res.texture_lineardepth_downsize1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_tiled1, res.texture_lineardepth_tiled1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_downsize2, res.texture_lineardepth_downsize2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_tiled2, res.texture_lineardepth_tiled2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_downsize3, res.texture_lineardepth_downsize3.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_tiled3, res.texture_lineardepth_tiled3.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_downsize4, res.texture_lineardepth_downsize4.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_lineardepth_tiled4, res.texture_lineardepth_tiled4.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_merged1, res.texture_ao_merged1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_hq1, res.texture_ao_hq1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_smooth1, res.texture_ao_smooth1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_merged2, res.texture_ao_merged2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_hq2, res.texture_ao_hq2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_smooth2, res.texture_ao_smooth2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_merged3, res.texture_ao_merged3.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_hq3, res.texture_ao_hq3.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_smooth3, res.texture_ao_smooth3.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_merged4, res.texture_ao_merged4.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_ao_hq4, res.texture_ao_hq4.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&output, 0, cmd); // always clear this
		if (!res.cleared) // clear the rest only at first time
		{
			res.cleared = true;
			device->ClearUAV(&res.texture_lineardepth_downsize1, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_tiled1, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_downsize2, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_tiled2, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_downsize3, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_tiled3, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_downsize4, 0, cmd);
			device->ClearUAV(&res.texture_lineardepth_tiled4, 0, cmd);
			device->ClearUAV(&res.texture_ao_merged1, 0, cmd);
			device->ClearUAV(&res.texture_ao_hq1, 0, cmd);
			device->ClearUAV(&res.texture_ao_smooth1, 0, cmd);
			device->ClearUAV(&res.texture_ao_merged2, 0, cmd);
			device->ClearUAV(&res.texture_ao_hq2, 0, cmd);
			device->ClearUAV(&res.texture_ao_smooth2, 0, cmd);
			device->ClearUAV(&res.texture_ao_merged3, 0, cmd);
			device->ClearUAV(&res.texture_ao_hq3, 0, cmd);
			device->ClearUAV(&res.texture_ao_smooth3, 0, cmd);
			device->ClearUAV(&res.texture_ao_merged4, 0, cmd);
			device->ClearUAV(&res.texture_ao_hq4, 0, cmd);
		}
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Depth downsampling + deinterleaving pass1:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS1], cmd);

		const GPUResource* uavs[] = {
			&res.texture_lineardepth_downsize1,
			&res.texture_lineardepth_tiled1,
			&res.texture_lineardepth_downsize2,
			&res.texture_lineardepth_tiled2,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize1, res.texture_lineardepth_downsize1.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled1, res.texture_lineardepth_tiled1.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_downsize2, res.texture_lineardepth_downsize2.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled2, res.texture_lineardepth_tiled2.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const TextureDesc& desc = res.texture_lineardepth_tiled2.GetDesc();
		device->Dispatch(desc.width, desc.height, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize1, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize1.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled1, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled1.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_downsize2, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize2.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled2, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled2.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Depth downsampling + deinterleaving pass2:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_PREPAREDEPTHBUFFERS2], cmd);

		device->BindResource(&res.texture_lineardepth_downsize2, 0, cmd);

		const GPUResource* uavs[] = {
			&res.texture_lineardepth_downsize3,
			&res.texture_lineardepth_tiled3,
			&res.texture_lineardepth_downsize4,
			&res.texture_lineardepth_tiled4,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize3, res.texture_lineardepth_downsize3.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled3, res.texture_lineardepth_tiled3.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_downsize4, res.texture_lineardepth_downsize4.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_lineardepth_tiled4, res.texture_lineardepth_tiled4.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		const TextureDesc& desc = res.texture_lineardepth_tiled4.GetDesc();
		device->Dispatch(desc.width, desc.height, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_lineardepth_downsize3, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize3.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled3, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled3.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_downsize4, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_downsize4.desc.layout),
				GPUBarrier::Image(&res.texture_lineardepth_tiled4, ResourceState::UNORDERED_ACCESS, res.texture_lineardepth_tiled4.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}


	float SampleThickness[12];
	SampleThickness[0] = sqrt(1.0f - 0.2f * 0.2f);
	SampleThickness[1] = sqrt(1.0f - 0.4f * 0.4f);
	SampleThickness[2] = sqrt(1.0f - 0.6f * 0.6f);
	SampleThickness[3] = sqrt(1.0f - 0.8f * 0.8f);
	SampleThickness[4] = sqrt(1.0f - 0.2f * 0.2f - 0.2f * 0.2f);
	SampleThickness[5] = sqrt(1.0f - 0.2f * 0.2f - 0.4f * 0.4f);
	SampleThickness[6] = sqrt(1.0f - 0.2f * 0.2f - 0.6f * 0.6f);
	SampleThickness[7] = sqrt(1.0f - 0.2f * 0.2f - 0.8f * 0.8f);
	SampleThickness[8] = sqrt(1.0f - 0.4f * 0.4f - 0.4f * 0.4f);
	SampleThickness[9] = sqrt(1.0f - 0.4f * 0.4f - 0.6f * 0.6f);
	SampleThickness[10] = sqrt(1.0f - 0.4f * 0.4f - 0.8f * 0.8f);
	SampleThickness[11] = sqrt(1.0f - 0.6f * 0.6f - 0.6f * 0.6f);
	const float RejectionFalloff = 2.0f * power;
	const float Accentuation = 0.1f;

	// The msao_compute will be called repeatedly, so create a local lambda for it:
	auto msao_compute = [&](const Texture& write_result, const Texture& read_depth) 
	{
		const TextureDesc& desc = read_depth.GetDesc();

		MSAO msao;

		// Load first element of projection matrix which is the cotangent of the horizontal FOV divided by 2.
		const float TanHalfFovH = 1.0f / camera.Projection.m[0][0];

		// Here we compute multipliers that convert the center depth value into (the reciprocal of)
		// sphere thicknesses at each sample location.  This assumes a maximum sample radius of 5
		// units, but since a sphere has no thickness at its extent, we don't need to sample that far
		// out.  Only samples whole integer offsets with distance less than 25 are used.  This means
		// that there is no sample at (3, 4) because its distance is exactly 25 (and has a thickness of 0.)

		// The shaders are set up to sample a circular region within a 5-pixel radius.
		const float ScreenspaceDiameter = 10.0f;

		// SphereDiameter = CenterDepth * ThicknessMultiplier.  This will compute the thickness of a sphere centered
		// at a specific depth.  The ellipsoid scale can stretch a sphere into an ellipsoid, which changes the
		// characteristics of the AO.
		// TanHalfFovH:  Radius of sphere in depth units if its center lies at Z = 1
		// ScreenspaceDiameter:  Diameter of sample sphere in pixel units
		// ScreenspaceDiameter / BufferWidth:  Ratio of the screen width that the sphere actually covers
		// Note about the "2.0f * ":  Diameter = 2 * Radius
		float ThicknessMultiplier = 2.0f * TanHalfFovH * ScreenspaceDiameter / desc.width;
		if (desc.array_size == 1)
		{
			ThicknessMultiplier *= 2.0f;
		}

		// This will transform a depth value from [0, thickness] to [0, 1].
		float InverseRangeFactor = 1.0f / ThicknessMultiplier;

		// The thicknesses are smaller for all off-center samples of the sphere.  Compute thicknesses relative
		// to the center sample.
		msao.xInvThicknessTable[0].x = InverseRangeFactor / SampleThickness[0];
		msao.xInvThicknessTable[0].y = InverseRangeFactor / SampleThickness[1];
		msao.xInvThicknessTable[0].z = InverseRangeFactor / SampleThickness[2];
		msao.xInvThicknessTable[0].w = InverseRangeFactor / SampleThickness[3];
		msao.xInvThicknessTable[1].x = InverseRangeFactor / SampleThickness[4];
		msao.xInvThicknessTable[1].y = InverseRangeFactor / SampleThickness[5];
		msao.xInvThicknessTable[1].z = InverseRangeFactor / SampleThickness[6];
		msao.xInvThicknessTable[1].w = InverseRangeFactor / SampleThickness[7];
		msao.xInvThicknessTable[2].x = InverseRangeFactor / SampleThickness[8];
		msao.xInvThicknessTable[2].y = InverseRangeFactor / SampleThickness[9];
		msao.xInvThicknessTable[2].z = InverseRangeFactor / SampleThickness[10];
		msao.xInvThicknessTable[2].w = InverseRangeFactor / SampleThickness[11];

		// These are the weights that are multiplied against the samples because not all samples are
		// equally important.  The farther the sample is from the center location, the less they matter.
		// We use the thickness of the sphere to determine the weight.  The scalars in front are the number
		// of samples with this weight because we sum the samples together before multiplying by the weight,
		// so as an aggregate all of those samples matter more.  After generating this table, the weights
		// are normalized.
		msao.xSampleWeightTable[0].x = 4.0f * SampleThickness[0];    // Axial
		msao.xSampleWeightTable[0].y = 4.0f * SampleThickness[1];    // Axial
		msao.xSampleWeightTable[0].z = 4.0f * SampleThickness[2];    // Axial
		msao.xSampleWeightTable[0].w = 4.0f * SampleThickness[3];    // Axial
		msao.xSampleWeightTable[1].x = 4.0f * SampleThickness[4];    // Diagonal
		msao.xSampleWeightTable[1].y = 8.0f * SampleThickness[5];    // L-shaped
		msao.xSampleWeightTable[1].z = 8.0f * SampleThickness[6];    // L-shaped
		msao.xSampleWeightTable[1].w = 8.0f * SampleThickness[7];    // L-shaped
		msao.xSampleWeightTable[2].x = 4.0f * SampleThickness[8];    // Diagonal
		msao.xSampleWeightTable[2].y = 8.0f * SampleThickness[9];    // L-shaped
		msao.xSampleWeightTable[2].z = 8.0f * SampleThickness[10];   // L-shaped
		msao.xSampleWeightTable[2].w = 4.0f * SampleThickness[11];   // Diagonal

		// If we aren't using all of the samples, delete their weights before we normalize.
#ifndef MSAO_SAMPLE_EXHAUSTIVELY
		msao.xSampleWeightTable[0].x = 0.0f;
		msao.xSampleWeightTable[0].z = 0.0f;
		msao.xSampleWeightTable[1].y = 0.0f;
		msao.xSampleWeightTable[1].w = 0.0f;
		msao.xSampleWeightTable[2].y = 0.0f;
#endif

		// Normalize the weights by dividing by the sum of all weights
		float totalWeight = 0.0f;
		totalWeight += msao.xSampleWeightTable[0].x;
		totalWeight += msao.xSampleWeightTable[0].y;
		totalWeight += msao.xSampleWeightTable[0].z;
		totalWeight += msao.xSampleWeightTable[0].w;
		totalWeight += msao.xSampleWeightTable[1].x;
		totalWeight += msao.xSampleWeightTable[1].y;
		totalWeight += msao.xSampleWeightTable[1].z;
		totalWeight += msao.xSampleWeightTable[1].w;
		totalWeight += msao.xSampleWeightTable[2].x;
		totalWeight += msao.xSampleWeightTable[2].y;
		totalWeight += msao.xSampleWeightTable[2].z;
		totalWeight += msao.xSampleWeightTable[2].w;
		msao.xSampleWeightTable[0].x /= totalWeight;
		msao.xSampleWeightTable[0].y /= totalWeight;
		msao.xSampleWeightTable[0].z /= totalWeight;
		msao.xSampleWeightTable[0].w /= totalWeight;
		msao.xSampleWeightTable[1].x /= totalWeight;
		msao.xSampleWeightTable[1].y /= totalWeight;
		msao.xSampleWeightTable[1].z /= totalWeight;
		msao.xSampleWeightTable[1].w /= totalWeight;
		msao.xSampleWeightTable[2].x /= totalWeight;
		msao.xSampleWeightTable[2].y /= totalWeight;
		msao.xSampleWeightTable[2].z /= totalWeight;
		msao.xSampleWeightTable[2].w /= totalWeight;

		msao.xInvSliceDimension.x = 1.0f / desc.width;
		msao.xInvSliceDimension.y = 1.0f / desc.height;
		msao.xRejectFadeoff = 1.0f / -RejectionFalloff;
		msao.xRcpAccentuation = 1.0f / (1.0f + Accentuation);

		device->BindDynamicConstantBuffer(msao, CBSLOT_MSAO, cmd);

		device->BindResource(&read_depth, 0, cmd);

		const GPUResource* uavs[] = {
			&write_result,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&write_result, write_result.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		if (desc.array_size == 1)
		{
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO], cmd);
			device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);
		}
		else
		{
			device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_INTERLEAVE], cmd);
			device->Dispatch((desc.width + 7) / 8, (desc.height + 7) / 8, desc.array_size, cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&write_result, ResourceState::UNORDERED_ACCESS, write_result.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}; // end of lambda: msao_compute

	msao_compute(res.texture_ao_merged4, res.texture_lineardepth_tiled4);
	msao_compute(res.texture_ao_hq4, res.texture_lineardepth_downsize4);

	msao_compute(res.texture_ao_merged3, res.texture_lineardepth_tiled3);
	msao_compute(res.texture_ao_hq3, res.texture_lineardepth_downsize3);

	msao_compute(res.texture_ao_merged2, res.texture_lineardepth_tiled2);
	msao_compute(res.texture_ao_hq2, res.texture_lineardepth_downsize2);

	msao_compute(res.texture_ao_merged1, res.texture_lineardepth_tiled1);
	msao_compute(res.texture_ao_hq1, res.texture_lineardepth_downsize1);

	auto blur_and_upsample = [&](const Texture& Destination, const Texture& HiResDepth, const Texture& LoResDepth,
		const Texture* InterleavedAO, const Texture* HighQualityAO, const Texture* HiResAO)
	{
		const uint32_t LoWidth = LoResDepth.GetDesc().width;
		const uint32_t LoHeight = LoResDepth.GetDesc().height;
		const uint32_t HiWidth = HiResDepth.GetDesc().width;
		const uint32_t HiHeight = HiResDepth.GetDesc().height;

		if (HiResAO == nullptr)
		{
			if (HighQualityAO == nullptr)
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE], cmd);
			}
			else
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN], cmd);
			}
		}
		else
		{
			if (HighQualityAO == nullptr)
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_BLENDOUT], cmd);
			}
			else
			{
				device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MSAO_BLURUPSAMPLE_PREMIN_BLENDOUT], cmd);
			}
		}

		static float g_NoiseFilterTolerance = -3.0f;
		static float g_BlurTolerance = -5.0f;
		static float g_UpsampleTolerance = -7.0f;

		MSAO_UPSAMPLE msao_upsample;
		msao_upsample.InvLowResolution = float2(1.0f / LoWidth, 1.0f / LoHeight);
		msao_upsample.InvHighResolution = float2(1.0f / HiWidth, 1.0f / HiHeight);
		msao_upsample.kBlurTolerance = 1.0f - powf(10.0f, g_BlurTolerance) * 1920.0f / (float)LoWidth;
		msao_upsample.kBlurTolerance *= msao_upsample.kBlurTolerance;
		msao_upsample.kUpsampleTolerance = powf(10.0f, g_UpsampleTolerance);
		msao_upsample.NoiseFilterStrength = 1.0f / (powf(10.0f, g_NoiseFilterTolerance) + msao_upsample.kUpsampleTolerance);
		msao_upsample.StepSize = (float)lineardepth.GetDesc().width / (float)LoWidth;
		device->PushConstants(&msao_upsample, sizeof(msao_upsample), cmd);
		
		device->BindUAV(&Destination, 0, cmd);
		device->BindResource(&LoResDepth, 0, cmd);
		device->BindResource(&HiResDepth, 1, cmd);
		if (InterleavedAO != nullptr)
		{
			device->BindResource(InterleavedAO, 2, cmd);
		}
		if (HighQualityAO != nullptr)
		{
			device->BindResource(HighQualityAO, 3, cmd);
		}
		if (HiResAO != nullptr)
		{
			device->BindResource(HiResAO, 4, cmd);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&Destination, Destination.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((HiWidth + 2 + 15) / 16, (HiHeight + 2 + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&Destination, ResourceState::UNORDERED_ACCESS, Destination.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	};

	blur_and_upsample(
		res.texture_ao_smooth3,
		res.texture_lineardepth_downsize3,
		res.texture_lineardepth_downsize4,
		&res.texture_ao_merged4,
		&res.texture_ao_hq4,
		&res.texture_ao_merged3
	);

	blur_and_upsample(
		res.texture_ao_smooth2,
		res.texture_lineardepth_downsize2,
		res.texture_lineardepth_downsize3,
		&res.texture_ao_smooth3,
		&res.texture_ao_hq3,
		&res.texture_ao_merged2
	);

	blur_and_upsample(
		res.texture_ao_smooth1,
		res.texture_lineardepth_downsize1,
		res.texture_lineardepth_downsize2,
		&res.texture_ao_smooth2,
		&res.texture_ao_hq2,
		&res.texture_ao_merged1
	);

	blur_and_upsample(
		output,
		lineardepth,
		res.texture_lineardepth_downsize1,
		&res.texture_ao_smooth1,
		&res.texture_ao_hq1,
		nullptr
	);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateRTAOResources(RTAOResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.normals);
	device->SetName(&res.normals, "rtao_normals");

	desc.format = Format::R8_UNORM;
	device->CreateTexture(&desc, nullptr, &res.denoised);
	device->SetName(&res.denoised, "denoised");

	GPUBufferDesc bd;
	bd.stride = sizeof(uint);
	bd.size = bd.stride *
		((desc.width + 7) / 8) *
		((desc.height + 3) / 4);
	bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateBuffer(&bd, nullptr, &res.tiles);
	device->SetName(&res.tiles, "rtao_tiles");
	device->CreateBuffer(&bd, nullptr, &res.metadata);
	device->SetName(&res.metadata, "rtao_metadata");

	desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.scratch[0]);
	device->SetName(&res.scratch[0], "rtao_scratch[0]");
	device->CreateTexture(&desc, nullptr, &res.scratch[1]);
	device->SetName(&res.scratch[1], "rtao_scratch[1]");

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.moments[0]);
	device->SetName(&res.moments[0], "rtao_moments[0]");
	device->CreateTexture(&desc, nullptr, &res.moments[1]);
	device->SetName(&res.moments[1], "rtao_moments[1]");
}
void Postprocess_RTAO(
	const RTAOResources& res,
	const Scene& scene,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float range,
	float power
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	device->EventBegin("Postprocess_RTAO", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("RTAO", cmd);

	if (res.frame == 0)
	{
		// Maybe we don't need to clear them all, but it's safer this way:
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1], res.scratch[1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0], res.moments[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1], res.moments[1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&res.normals, 0, cmd);
		device->ClearUAV(&res.scratch[0], 0, cmd);
		device->ClearUAV(&res.scratch[1], 0, cmd);
		device->ClearUAV(&res.moments[0], 0, cmd);
		device->ClearUAV(&res.moments[1], 0, cmd);
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	BindCommonResources(cmd);

	device->EventBegin("Raytrace", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO], cmd);

	const GPUResource* uavs[] = {
		&res.normals,
		&res.tiles
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	PostProcess postprocess = {};
	postprocess.resolution.x = output.desc.width / 2;
	postprocess.resolution.y = output.desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	rtao_range = range;
	rtao_power = power;
	postprocess.params0.w = (float)res.frame;
	uint8_t instanceInclusionMask = 0xFF;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.tiles, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&output, 0, cmd);

	device->Dispatch(
		(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.normals, ResourceState::UNORDERED_ACCESS, res.normals.desc.layout),
			GPUBarrier::Buffer(&res.tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);

	int temporal_output = res.frame % 2;
	int temporal_history = 1 - temporal_output;

	// Denoise - Tile Classification:
	{
		device->EventBegin("Denoise - Tile Classification", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_TILECLASSIFICATION], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.tiles, 1, cmd);
		device->BindResource(&res.moments[temporal_history], 2, cmd);
		device->BindResource(&res.scratch[1], 3, cmd);

		const GPUResource* uavs[] = {
			&res.scratch[0],
			&res.moments[temporal_output],
			&res.metadata
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[temporal_output], res.moments[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Buffer(&res.metadata, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.scratch[0], ResourceState::UNORDERED_ACCESS, res.scratch[0].desc.layout),
				GPUBarrier::Image(&res.moments[temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[temporal_output].desc.layout),
				GPUBarrier::Buffer(&res.metadata, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE)
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Denoise - Spatial filtering:
	{
		device->EventBegin("Denoise - Filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTAO_DENOISE_FILTER], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.metadata, 1, cmd);

		// pass0:
		{
			device->BindResource(&res.scratch[0], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[1],
				&res.denoised,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[1], res.scratch[1].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 0;
			postprocess.params1.y = 1;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		// pass1:
		{
			device->BindResource(&res.scratch[1], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[0],
				&res.denoised,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[1], ResourceState::UNORDERED_ACCESS, res.scratch[1].desc.layout),
					GPUBarrier::Image(&res.scratch[0], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 1;
			postprocess.params1.y = 2;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		// pass2:
		{
			device->BindResource(&res.scratch[0], 2, cmd);
			const GPUResource* uavs[] = {
				&res.scratch[1],
				&res.denoised,
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[0], ResourceState::UNORDERED_ACCESS, res.scratch[0].desc.layout),
					GPUBarrier::Image(&res.scratch[1], res.scratch[0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 2;
			postprocess.params1.y = 4;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.scratch[1], ResourceState::UNORDERED_ACCESS, res.scratch[1].desc.layout),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}
	res.frame++;

	Postprocess_Upsample_Bilateral(
		res.denoised,
		lineardepth,
		output,
		cmd,
		false,
		1.2f
	);

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateRTDiffuseResources(RTDiffuseResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayIndirectDiffuse);

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_spatial);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_spatial_variance);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[1]);
}
void Postprocess_RTDiffuse(
	const RTDiffuseResources& res,
	const Scene& scene,
	const Texture& output,
	CommandList cmd,
	float range
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	device->EventBegin("Postprocess_RTDiffuse", cmd);
	auto profilerRange = wi::profiler::BeginRangeGPU("RTDiffuse", cmd);

	int temporal_output = res.frame % 2;
	int temporal_history = 1 - temporal_output;

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_rayIndirectDiffuse, res.texture_rayIndirectDiffuse.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_spatial, res.texture_spatial.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_spatial_variance, res.texture_spatial_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[temporal_history], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&res.texture_rayIndirectDiffuse, 0, cmd);
	device->ClearUAV(&res.texture_spatial, 0, cmd);
	device->ClearUAV(&res.texture_spatial_variance, 0, cmd);
	device->ClearUAV(&res.texture_temporal[temporal_output], 0, cmd);
	device->ClearUAV(&res.texture_temporal_variance[temporal_output], 0, cmd);
	if (res.frame == 0)
	{
		device->ClearUAV(&res.texture_temporal[temporal_history], 0, cmd);
		device->ClearUAV(&res.texture_temporal_variance[temporal_history], 0, cmd);
	}
	device->ClearUAV(&output, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(&res.texture_rayIndirectDiffuse),
			GPUBarrier::Memory(&res.texture_spatial),
			GPUBarrier::Memory(&res.texture_spatial_variance),
			GPUBarrier::Memory(&res.texture_temporal[temporal_output]),
			GPUBarrier::Memory(&res.texture_temporal_variance[temporal_output]),
			GPUBarrier::Image(&res.texture_temporal[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
			GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
			GPUBarrier::Memory(&output),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	BindCommonResources(cmd);

	const TextureDesc& desc = output.desc;

	// Render half-res:
	PostProcess postprocess;
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	rtdiffuse_range = range;
	rtdiffuse_frame = (float)res.frame;
	uint8_t instanceInclusionMask = 0xFF;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));

	{
		device->EventBegin("RTDiffuse - ray trace", cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTDIFFUSE], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&res.texture_rayIndirectDiffuse,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_rayIndirectDiffuse.GetDesc().width + 7) / 8,
			(res.texture_rayIndirectDiffuse.GetDesc().height + 3) / 4,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_rayIndirectDiffuse, ResourceState::UNORDERED_ACCESS, res.texture_rayIndirectDiffuse.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Spatial pass:
	{
		device->EventBegin("RTDiffuse - spatial resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_SPATIAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_rayIndirectDiffuse,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_spatial,
			&res.texture_spatial_variance,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_spatial.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_spatial.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_spatial, ResourceState::UNORDERED_ACCESS, res.texture_spatial.desc.layout),
				GPUBarrier::Image(&res.texture_spatial_variance, ResourceState::UNORDERED_ACCESS, res.texture_spatial_variance.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Temporal pass:
	{
		device->EventBegin("RTDiffuse - temporal resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_TEMPORAL], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_spatial,
			&res.texture_temporal[temporal_history],
			&res.texture_spatial_variance,
			&res.texture_temporal_variance[temporal_history],
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Full res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Bilateral blur pass:
	{
		device->EventBegin("RTDiffuse - upsample", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTDIFFUSE_UPSAMPLE], cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(profilerRange);
	device->EventEnd(cmd);
}
void CreateSSGIResources(SSGIResources& res, XMUINT2 resolution)
{
	res.cleared = false;

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	desc.format = Format::R11G11B10_FLOAT;

	resolution.x = AlignTo(resolution.x, 64u);
	resolution.y = AlignTo(resolution.y, 64u);

	desc.width = (resolution.x + 7) / 8;
	desc.height = (resolution.y + 7) / 8;
	desc.array_size = 16;
	desc.mip_levels = 4;
	desc.format = Format::R32_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_atlas_depth);
	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_atlas_color);

	desc.array_size = 1;
	desc.mip_levels = 4;
	desc.width = (resolution.x + 1) / 2;
	desc.height = (resolution.y + 1) / 2;
	desc.format = Format::R32_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_depth_mips);
	desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_normal_mips);
	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_diffuse_mips);

	for (uint32_t i = 0; i < 4u; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&res.texture_atlas_depth, SubresourceType::SRV, 0, 16, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_atlas_depth, SubresourceType::UAV, 0, 16, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_atlas_color, SubresourceType::SRV, 0, 16, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_atlas_color, SubresourceType::UAV, 0, 16, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_depth_mips, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_depth_mips, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_normal_mips, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_normal_mips, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_diffuse_mips, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_diffuse_mips, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}
}
void Postprocess_SSGI(
	const SSGIResources& res,
	const Texture& input,
	const Texture& input_depth,
	const Texture& input_normal,
	const Texture& output,
	CommandList cmd,
	float depthRejection
)
{
	device->EventBegin("Postprocess_SSGI", cmd);
	auto profilerRange = wi::profiler::BeginRangeGPU("SSGI", cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_atlas_depth, res.texture_atlas_depth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_atlas_color, res.texture_atlas_color.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_depth_mips, res.texture_depth_mips.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_normal_mips, res.texture_normal_mips.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_diffuse_mips, res.texture_diffuse_mips.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	if (!res.cleared)
	{
		res.cleared = true;
		device->ClearUAV(&res.texture_atlas_depth, 0, cmd);
		device->ClearUAV(&res.texture_atlas_color, 0, cmd);
		device->ClearUAV(&res.texture_depth_mips, 0, cmd);
		device->ClearUAV(&res.texture_normal_mips, 0, cmd);
	}
	device->ClearUAV(&res.texture_diffuse_mips, 0, cmd);
	device->ClearUAV(&output, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	BindCommonResources(cmd);

	PostProcess postprocess = {};

	{
		device->EventBegin("SSGI - deinterleave", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSGI_DEINTERLEAVE], cmd);

		device->BindResource(&input, 0, cmd);

		device->BindUAV(&res.texture_atlas_depth, 0, cmd, 0);
		device->BindUAV(&res.texture_atlas_depth, 1, cmd, 1);
		device->BindUAV(&res.texture_atlas_depth, 2, cmd, 2);
		device->BindUAV(&res.texture_atlas_depth, 3, cmd, 3);

		device->BindUAV(&res.texture_atlas_color, 4, cmd, 0);
		device->BindUAV(&res.texture_atlas_color, 5, cmd, 1);
		device->BindUAV(&res.texture_atlas_color, 6, cmd, 2);
		device->BindUAV(&res.texture_atlas_color, 7, cmd, 3);

		device->BindUAV(&res.texture_depth_mips, 8, cmd, 0);
		device->BindUAV(&res.texture_depth_mips, 9, cmd, 1);
		device->BindUAV(&res.texture_depth_mips, 10, cmd, 2);
		device->BindUAV(&res.texture_depth_mips, 11, cmd, 3);

		device->BindUAV(&res.texture_normal_mips, 12, cmd, 0);
		device->BindUAV(&res.texture_normal_mips, 13, cmd, 1);
		device->BindUAV(&res.texture_normal_mips, 14, cmd, 2);
		device->BindUAV(&res.texture_normal_mips, 15, cmd, 3);

		const TextureDesc& desc = res.texture_atlas_depth.GetDesc();
		device->Dispatch(
			desc.width >> 1,
			desc.height >> 1,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_atlas_depth, ResourceState::UNORDERED_ACCESS, res.texture_atlas_depth.desc.layout),
			GPUBarrier::Image(&res.texture_atlas_color, ResourceState::UNORDERED_ACCESS, res.texture_atlas_color.desc.layout),
			GPUBarrier::Image(&res.texture_depth_mips, ResourceState::UNORDERED_ACCESS, res.texture_depth_mips.desc.layout),
			GPUBarrier::Image(&res.texture_normal_mips, ResourceState::UNORDERED_ACCESS, res.texture_normal_mips.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	{
		device->EventBegin("SSGI - diffuse", cmd);

		postprocess.params0.w = 1.0f / depthRejection;

		// Wide sampling passes:
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSGI_WIDE], cmd);

		// 16x:
		{
			device->BindResource(&res.texture_atlas_depth, 0, cmd, 3);
			device->BindResource(&res.texture_atlas_color, 1, cmd, 3);
			device->BindResource(&res.texture_normal_mips, 2, cmd, 3);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 3);

			const TextureDesc& desc = res.texture_atlas_depth.GetDesc();

			postprocess.resolution.x = desc.width >> 3;
			postprocess.resolution.y = desc.height >> 3;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 8; // range
			postprocess.params0.y = 2; // spread
			postprocess.params0.z = std::pow(1.0f / (postprocess.params0.x * postprocess.params0.y), 2.0f); // rangespread_rcp2
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + 15) / 16,
				(postprocess.resolution.y + 15) / 16,
				16,
				cmd
			);
		}
		// 8x:
		{
			device->BindResource(&res.texture_atlas_depth, 0, cmd, 2);
			device->BindResource(&res.texture_atlas_color, 1, cmd, 2);
			device->BindResource(&res.texture_normal_mips, 2, cmd, 2);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 2);

			const TextureDesc& desc = res.texture_atlas_depth.GetDesc();

			postprocess.resolution.x = desc.width >> 2;
			postprocess.resolution.y = desc.height >> 2;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 4; // range
			postprocess.params0.y = 4; // spread
			postprocess.params0.z = std::pow(1.0f / (postprocess.params0.x * postprocess.params0.y), 2.0f); // rangespread_rcp2
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + 15) / 16,
				(postprocess.resolution.y + 15) / 16,
				16,
				cmd
			);
		}

		// Narrow sampling passes:
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSGI], cmd);

		// 4x:
		{
			device->BindResource(&res.texture_atlas_depth, 0, cmd, 1);
			device->BindResource(&res.texture_atlas_color, 1, cmd, 1);
			device->BindResource(&res.texture_normal_mips, 2, cmd, 1);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 1);

			const TextureDesc& desc = res.texture_atlas_depth.GetDesc();

			postprocess.resolution.x = desc.width >> 1u;
			postprocess.resolution.y = desc.height >> 1u;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 2; // range
			postprocess.params0.y = 2; // spread
			postprocess.params0.z = std::pow(1.0f / (postprocess.params0.x * postprocess.params0.y), 2.0f); // rangespread_rcp2
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + 7) / 8,
				(postprocess.resolution.y + 7) / 8,
				16,
				cmd
			);
		}

		// 2x:
		{
			device->BindResource(&res.texture_atlas_depth, 0, cmd, 0);
			device->BindResource(&res.texture_atlas_color, 1, cmd, 0);
			device->BindResource(&res.texture_normal_mips, 2, cmd, 0);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 0);

			const TextureDesc& desc = res.texture_atlas_depth.GetDesc();

			postprocess.resolution.x = desc.width;
			postprocess.resolution.y = desc.height;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 2; // range
			postprocess.params0.y = 2; // spread
			postprocess.params0.z = std::pow(1.0f / (postprocess.params0.x * postprocess.params0.y), 2.0f); // rangespread_rcp2
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + 7) / 8,
				(postprocess.resolution.y + 7) / 8,
				16,
				cmd
			);
		}

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_diffuse_mips, ResourceState::UNORDERED_ACCESS, res.texture_diffuse_mips.desc.layout, 3),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	{
		device->EventBegin("SSGI - upsample", cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSGI_UPSAMPLE_WIDE], cmd);

		// 16x -> 8x
		{
			device->BindResource(&res.texture_depth_mips, 0, cmd, 3);
			device->BindResource(&res.texture_normal_mips, 1, cmd, 3);
			device->BindResource(&res.texture_diffuse_mips, 2, cmd, 3);
			device->BindResource(&res.texture_depth_mips, 3, cmd, 2);
			device->BindResource(&res.texture_normal_mips, 4, cmd, 2);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 2);

			const TextureDesc& desc = res.texture_diffuse_mips.desc;
			postprocess.resolution.x = desc.width >> 2;
			postprocess.resolution.y = desc.height >> 2;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 3; // range
			postprocess.params0.y = 2; // spread
			postprocess.params1.x = float(desc.width >> 3);
			postprocess.params1.y = float(desc.height >> 3);
			postprocess.params1.z = 1.0f / postprocess.params1.x;
			postprocess.params1.w = 1.0f / postprocess.params1.y;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_diffuse_mips, ResourceState::UNORDERED_ACCESS, res.texture_diffuse_mips.desc.layout, 2),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		// 8x -> 4x
		{
			device->BindResource(&res.texture_depth_mips, 0, cmd, 2);
			device->BindResource(&res.texture_normal_mips, 1, cmd, 2);
			device->BindResource(&res.texture_diffuse_mips, 2, cmd, 2);
			device->BindResource(&res.texture_depth_mips, 3, cmd, 1);
			device->BindResource(&res.texture_normal_mips, 4, cmd, 1);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 1);

			const TextureDesc& desc = res.texture_diffuse_mips.desc;
			postprocess.resolution.x = desc.width >> 1;
			postprocess.resolution.y = desc.height >> 1;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 2; // range
			postprocess.params0.y = 3; // spread
			postprocess.params1.x = float(desc.width >> 2);
			postprocess.params1.y = float(desc.height >> 2);
			postprocess.params1.z = 1.0f / postprocess.params1.x;
			postprocess.params1.w = 1.0f / postprocess.params1.y;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_diffuse_mips, ResourceState::UNORDERED_ACCESS, res.texture_diffuse_mips.desc.layout, 1),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSGI_UPSAMPLE], cmd);

		// 4x -> 2x
		{
			device->BindResource(&res.texture_depth_mips, 0, cmd, 1);
			device->BindResource(&res.texture_normal_mips, 1, cmd, 1);
			device->BindResource(&res.texture_diffuse_mips, 2, cmd, 1);
			device->BindResource(&res.texture_depth_mips, 3, cmd, 0);
			device->BindResource(&res.texture_normal_mips, 4, cmd, 0);
			device->BindUAV(&res.texture_diffuse_mips, 0, cmd, 0);

			const TextureDesc& desc = res.texture_diffuse_mips.desc;
			postprocess.resolution.x = desc.width;
			postprocess.resolution.y = desc.height;
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 1; // range
			postprocess.params0.y = 2; // spread
			postprocess.params1.x = float(desc.width >> 1);
			postprocess.params1.y = float(desc.height >> 1);
			postprocess.params1.z = 1.0f / postprocess.params1.x;
			postprocess.params1.w = 1.0f / postprocess.params1.y;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_diffuse_mips, ResourceState::UNORDERED_ACCESS, res.texture_diffuse_mips.desc.layout, 0),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		// 2x -> output
		{
			device->BindResource(&res.texture_depth_mips, 0, cmd, 0);
			device->BindResource(&res.texture_normal_mips, 1, cmd, 0);
			device->BindResource(&res.texture_diffuse_mips, 2, cmd, 0);
			device->BindResource(&input_depth, 3, cmd);
			device->BindResource(&input_normal, 4, cmd);
			device->BindUAV(&output, 0, cmd);

			const TextureDesc& desc = output.desc;
			postprocess.resolution.x = AlignTo(desc.width, 64u);	// align = uv correction!
			postprocess.resolution.y = AlignTo(desc.height, 64u);	// align = uv correction!
			postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
			postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
			postprocess.params0.x = 1; // range
			postprocess.params0.y = 1; // spread
			const TextureDesc& desc2 = res.texture_diffuse_mips.desc;
			postprocess.params1.x = float(desc2.width);
			postprocess.params1.y = float(desc2.height);
			postprocess.params1.z = 1.0f / postprocess.params1.x;
			postprocess.params1.w = 1.0f / postprocess.params1.y;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,  // dispatch is using desc size (unaligned!)
				(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE, // dispatch is using desc size (unaligned!)
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(profilerRange);
	device->EventEnd(cmd);
}
void CreateRTReflectionResources(RTReflectionResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayIndirectSpecular);
	device->CreateTexture(&desc, nullptr, &res.texture_rayDirectionPDF);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayLengths);
	device->SetName(&res.texture_rayLengths, "ssr_rayLengths");

	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_variance);
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_reprojectionDepth);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[1]);
}
void Postprocess_RTReflection(
	const RTReflectionResources& res,
	const Scene& scene,
	const Texture& output,
	CommandList cmd,
	float range,
	float roughnessCutoff
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	device->EventBegin("Postprocess_RTReflection", cmd);
	auto profilerRange = wi::profiler::BeginRangeGPU("RTReflection", cmd);

	BindCommonResources(cmd);

	const int temporal_output = res.frame % 2;
	const int temporal_history = 1 - temporal_output;

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_resolve, res.texture_resolve.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_resolve_variance, res.texture_resolve_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, res.texture_resolve_reprojectionDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	if (res.frame == 0)
	{
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_history], res.texture_temporal[temporal_history].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], res.texture_temporal_variance[temporal_history].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
		device->ClearUAV(&res.texture_resolve, 0, cmd);
		device->ClearUAV(&res.texture_resolve_variance, 0, cmd);
		device->ClearUAV(&res.texture_resolve_reprojectionDepth, 0, cmd);
		device->ClearUAV(&res.texture_temporal[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal[1], 0, cmd);
		device->ClearUAV(&res.texture_temporal_variance[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal_variance[1], 0, cmd);
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_temporal[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_history].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_history].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}

	device->ClearUAV(&output, 0, cmd);

	const TextureDesc& desc = output.desc;

	// Render half-res:
	PostProcess postprocess;
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	rtreflection_range = range;
	rtreflection_roughness_cutoff = roughnessCutoff;
	rtreflection_frame = (float)res.frame;
	uint8_t instanceInclusionMask = raytracing_inclusion_mask_reflection;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));

	{
		//device->EventBegin("RTReflection Raytrace pass", cmd);

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
		device->BindRaytracingPipelineState(&RTPSO_reflection, cmd);
#else
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTREFLECTION], cmd);
#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, res.texture_rayIndirectSpecular.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, res.texture_rayDirectionPDF.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayLengths, res.texture_rayLengths.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

#ifdef RTREFLECTION_WITH_RAYTRACING_PIPELINE
		size_t shaderIdentifierSize = device->GetShaderIdentifierSize();
		GraphicsDevice::GPUAllocation shadertable_raygen = device->AllocateGPU(shaderIdentifierSize, cmd);
		GraphicsDevice::GPUAllocation shadertable_miss = device->AllocateGPU(shaderIdentifierSize, cmd);
		GraphicsDevice::GPUAllocation shadertable_hitgroup = device->AllocateGPU(shaderIdentifierSize, cmd);

		device->WriteShaderIdentifier(&RTPSO_reflection, 0, shadertable_raygen.data);
		device->WriteShaderIdentifier(&RTPSO_reflection, 1, shadertable_miss.data);
		device->WriteShaderIdentifier(&RTPSO_reflection, 2, shadertable_hitgroup.data);

		DispatchRaysDesc dispatchraysdesc;
		dispatchraysdesc.ray_generation.buffer = &shadertable_raygen.buffer;
		dispatchraysdesc.ray_generation.offset = shadertable_raygen.offset;
		dispatchraysdesc.ray_generation.size = shaderIdentifierSize;

		dispatchraysdesc.miss.buffer = &shadertable_miss.buffer;
		dispatchraysdesc.miss.offset = shadertable_miss.offset;
		dispatchraysdesc.miss.size = shaderIdentifierSize;
		dispatchraysdesc.miss.stride = shaderIdentifierSize;

		dispatchraysdesc.hit_group.buffer = &shadertable_hitgroup.buffer;
		dispatchraysdesc.hit_group.offset = shadertable_hitgroup.offset;
		dispatchraysdesc.hit_group.size = shaderIdentifierSize;
		dispatchraysdesc.hit_group.stride = shaderIdentifierSize;

		dispatchraysdesc.width = desc.width / 2;
		dispatchraysdesc.height = desc.height / 2;

		device->DispatchRays(&dispatchraysdesc, cmd);

#else

		device->Dispatch(
			(res.texture_rayIndirectSpecular.GetDesc().width + 7) / 8,
			(res.texture_rayIndirectSpecular.GetDesc().height + 3) / 4,
			1,
			cmd
		);

#endif // RTREFLECTION_WITH_RAYTRACING_PIPELINE

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, ResourceState::UNORDERED_ACCESS, res.texture_rayIndirectSpecular.desc.layout),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, ResourceState::UNORDERED_ACCESS, res.texture_rayDirectionPDF.desc.layout),
				GPUBarrier::Image(&res.texture_rayLengths, ResourceState::UNORDERED_ACCESS, res.texture_rayLengths.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		//device->EventEnd(cmd);
	}

	// Resolve pass:
	{
		device->EventBegin("RTReflection - spatial resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_resolve,
			&res.texture_resolve_variance,
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_resolve, res.texture_resolve.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_variance, res.texture_resolve_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, res.texture_resolve_reprojectionDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_resolve.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_resolve.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_resolve, ResourceState::UNORDERED_ACCESS, res.texture_resolve.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_variance, ResourceState::UNORDERED_ACCESS, res.texture_resolve_variance.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, ResourceState::UNORDERED_ACCESS, res.texture_resolve_reprojectionDepth.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Temporal pass:
	{
		device->EventBegin("RTReflection - temporal resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_resolve,
			&res.texture_temporal[temporal_history],
			&res.texture_resolve_variance,
			&res.texture_temporal_variance[temporal_history],
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
				GPUBarrier::Memory(&output),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Upscale to full-res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Bilateral blur pass:
	{
		device->EventBegin("RTReflection - upsample", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(profilerRange);
	device->EventEnd(cmd);
}
void CreateSSRResources(SSRResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.width = (resolution.x + SSR_TILESIZE - 1) / SSR_TILESIZE;
	tile_desc.height = (resolution.y + SSR_TILESIZE - 1) / SSR_TILESIZE;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	tile_desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tile_minmax_roughness);

	TextureDesc tile_desc2 = tile_desc;
	tile_desc2.height = resolution.y;
	device->CreateTexture(&tile_desc2, nullptr, &res.texture_tile_minmax_roughness_horizontal);

	GPUBufferDesc bufferdesc;
	bufferdesc.stride = sizeof(PostprocessTileStatistics);
	bufferdesc.size = bufferdesc.stride;
	bufferdesc.bind_flags = BindFlag::UNORDERED_ACCESS;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_tracing_statistics);

	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_desc.width * tile_desc.height * bufferdesc.stride;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_tracing_expensive);

	TextureDesc desc;
	desc.type = TextureDesc::Type::TEXTURE_2D;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.format = Format::R16G16B16A16_FLOAT;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.texture_rayIndirectSpecular);
	device->CreateTexture(&desc, nullptr, &res.texture_rayDirectionPDF);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_rayLengths);
	device->SetName(&res.texture_rayLengths, "ssr_rayLengths");

	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	desc.format = Format::R16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_variance);
	device->CreateTexture(&desc, nullptr, &res.texture_resolve_reprojectionDepth);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[0]);
	device->CreateTexture(&desc, nullptr, &res.texture_temporal_variance[1]);

	desc.width = (uint32_t)std::pow(2.0f, 1.0f + std::floor(std::log2((float)resolution.x / 2)));
	desc.height = (uint32_t)std::pow(2.0f, 1.0f + std::floor(std::log2((float)resolution.y / 2)));
	desc.format = Format::R32G32_FLOAT;
	desc.mip_levels = 1 + (uint32_t)std::floor(std::log2f(std::max((float)desc.width, (float)desc.height)));
	device->CreateTexture(&desc, nullptr, &res.texture_depth_hierarchy);

	for (uint32_t i = 0; i < desc.mip_levels; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&res.texture_depth_hierarchy, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&res.texture_depth_hierarchy, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}
}
void Postprocess_SSR(
	const SSRResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float roughnessCutoff
)
{
	device->EventBegin("Postprocess_SSR", cmd);

	auto range = wi::profiler::BeginRangeGPU("SSR", cmd);

	BindCommonResources(cmd);

	const int temporal_output = res.frame % 2;
	const int temporal_history = 1 - temporal_output;

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_tile_minmax_roughness_horizontal, res.texture_tile_minmax_roughness_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tile_minmax_roughness, res.texture_tile_minmax_roughness.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_resolve, res.texture_resolve.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_resolve_variance, res.texture_resolve_variance.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, res.texture_resolve_reprojectionDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[temporal_output], res.texture_temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], res.texture_temporal_variance[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	if (res.frame == 0)
	{
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_history], res.texture_temporal[temporal_history].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], res.texture_temporal_variance[temporal_history].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
		device->ClearUAV(&res.texture_tile_minmax_roughness_horizontal, 0, cmd);
		device->ClearUAV(&res.texture_tile_minmax_roughness, 0, cmd);
		device->ClearUAV(&res.texture_resolve, 0, cmd);
		device->ClearUAV(&res.texture_resolve_variance, 0, cmd);
		device->ClearUAV(&res.texture_resolve_reprojectionDepth, 0, cmd);
		device->ClearUAV(&res.texture_temporal[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal[1], 0, cmd);
		device->ClearUAV(&res.texture_temporal_variance[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal_variance[1], 0, cmd);
		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_temporal[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_history].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_history], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_history].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}
	}

	device->ClearUAV(&output, 0, cmd);

	PostprocessTileStatistics tile_stats = {};
	tile_stats.dispatch_earlyexit.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_earlyexit.ThreadGroupCountY = 1;
	tile_stats.dispatch_earlyexit.ThreadGroupCountZ = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_cheap.ThreadGroupCountY = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountZ = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_expensive.ThreadGroupCountY = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountZ = 1;
	device->UpdateBuffer(&res.buffer_tile_tracing_statistics, &tile_stats, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Buffer(&res.buffer_tile_tracing_statistics, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	PostProcess postprocess;
	ssr_roughness_cutoff = roughnessCutoff;
	ssr_frame = (float)res.frame;

	// Compute tile classification (horizontal):
	{
		device->EventBegin("SSR - tile classification horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_HORIZONTAL], cmd);

		const GPUResource* uavs[] = {
			&res.texture_tile_minmax_roughness_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_tile_minmax_roughness_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tile_minmax_roughness_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tile_minmax_roughness_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tile_minmax_roughness_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile classification (vertical):
	{
		device->EventBegin("SSR - tile classification vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TILEMAXROUGHNESS_VERTICAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_tile_minmax_roughness_horizontal,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_tracing_statistics,
			&res.buffer_tiles_tracing_earlyexit,
			&res.buffer_tiles_tracing_cheap,
			&res.buffer_tiles_tracing_expensive,
			&res.texture_tile_minmax_roughness,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_tile_minmax_roughness.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tile_minmax_roughness.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tile_minmax_roughness, ResourceState::UNORDERED_ACCESS, res.texture_tile_minmax_roughness.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Depth hierarchy:
	{
		device->EventBegin("SSR - depth hierarchy", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_DEPTHHIERARCHY], cmd);

		TextureDesc hierarchyDesc = res.texture_depth_hierarchy.GetDesc();

		{
			device->BindUAV(&res.texture_depth_hierarchy, 0, cmd, 0);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, res.texture_depth_hierarchy.desc.layout, ResourceState::UNORDERED_ACCESS, 0),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params0.x = (float)hierarchyDesc.width;
			postprocess.params0.y = (float)hierarchyDesc.height;
			postprocess.params0.z = 1.0f;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				std::max(1u, hierarchyDesc.width / POSTPROCESS_BLOCKSIZE),
				std::max(1u, hierarchyDesc.height / POSTPROCESS_BLOCKSIZE),
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, ResourceState::UNORDERED_ACCESS, res.texture_depth_hierarchy.desc.layout, 0),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		for (uint32_t i = 1; i < hierarchyDesc.mip_levels; i++)
		{
			device->BindResource(&res.texture_depth_hierarchy, 0, cmd, i - 1);
			device->BindUAV(&res.texture_depth_hierarchy, 0, cmd, i);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, res.texture_depth_hierarchy.desc.layout, ResourceState::UNORDERED_ACCESS, i),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			hierarchyDesc.width /= 2;
			hierarchyDesc.height /= 2;

			hierarchyDesc.width = std::max(1u, hierarchyDesc.width);
			hierarchyDesc.height = std::max(1u, hierarchyDesc.height);

			postprocess.params0.x = (float)hierarchyDesc.width;
			postprocess.params0.y = (float)hierarchyDesc.height;
			postprocess.params0.z = 0.0f;
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->Dispatch(
				std::max(1u, (hierarchyDesc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE),
				std::max(1u, (hierarchyDesc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE),
				1,
				cmd
			);

			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.texture_depth_hierarchy, ResourceState::UNORDERED_ACCESS, res.texture_depth_hierarchy.desc.layout, i),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}
		}

		device->EventEnd(cmd);
	}

	const TextureDesc& desc = output.GetDesc();

	// Render half-res:
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	ssr_roughness_cutoff = roughnessCutoff;
	ssr_frame = (float)res.frame;

	// Factor to scale ratio between hierarchy and trace pass
	postprocess.params1.x = (float)postprocess.resolution.x / (float)res.texture_depth_hierarchy.GetDesc().width;
	postprocess.params1.y = (float)postprocess.resolution.y / (float)res.texture_depth_hierarchy.GetDesc().height;
	postprocess.params1.z = 1.0f / postprocess.params1.x;
	postprocess.params1.w = 1.0f / postprocess.params1.y;

	// Raytrace pass:
	{
		device->EventBegin("SSR - ray trace", cmd);

		const GPUResource* resarray[] = {
			&res.texture_depth_hierarchy,
			&input,
			&res.buffer_tiles_tracing_earlyexit,
			&res.buffer_tiles_tracing_cheap,
			&res.buffer_tiles_tracing_expensive
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tile_tracing_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_tracing_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, res.texture_rayIndirectSpecular.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, res.texture_rayDirectionPDF.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_rayLengths, res.texture_rayLengths.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_EARLYEXIT], cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, offsetof(PostprocessTileStatistics, dispatch_earlyexit), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE_CHEAP], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, offsetof(PostprocessTileStatistics, dispatch_cheap), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RAYTRACE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_tracing_statistics, offsetof(PostprocessTileStatistics, dispatch_expensive), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_rayIndirectSpecular, ResourceState::UNORDERED_ACCESS, res.texture_rayIndirectSpecular.desc.layout),
				GPUBarrier::Image(&res.texture_rayDirectionPDF, ResourceState::UNORDERED_ACCESS, res.texture_rayDirectionPDF.desc.layout),
				GPUBarrier::Image(&res.texture_rayLengths, ResourceState::UNORDERED_ACCESS, res.texture_rayLengths.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Resolve pass:
	{
		device->EventBegin("SSR - spatial resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_RESOLVE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_rayIndirectSpecular,
			&res.texture_rayDirectionPDF,
			&res.texture_rayLengths,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_resolve,
			&res.texture_resolve_variance,
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_resolve.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_resolve.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_resolve, ResourceState::UNORDERED_ACCESS, res.texture_resolve.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_variance, ResourceState::UNORDERED_ACCESS, res.texture_resolve_variance.desc.layout),
				GPUBarrier::Image(&res.texture_resolve_reprojectionDepth, ResourceState::UNORDERED_ACCESS, res.texture_resolve_reprojectionDepth.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Temporal pass:
	{
		device->EventBegin("SSR - temporal resolve", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_TEMPORAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_resolve,
			&res.texture_temporal[temporal_history],
			&res.texture_resolve_variance,
			&res.texture_temporal_variance[temporal_history],
			&res.texture_resolve_reprojectionDepth,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_temporal[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_temporal[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal[temporal_output].desc.layout),
				GPUBarrier::Image(&res.texture_temporal_variance[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_temporal_variance[temporal_output].desc.layout),
				GPUBarrier::Memory(&output),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Upscale to full-res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Bilateral blur pass:
	{
		device->EventBegin("SSR - upsample", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SSR_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&res.texture_temporal[temporal_output],
			&res.texture_temporal_variance[temporal_output],
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	res.frame++;

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateRTShadowResources(RTShadowResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;

	desc.format = Format::R32G32B32A32_UINT;
	device->CreateTexture(&desc, nullptr, &res.raytraced);
	device->SetName(&res.raytraced, "raytraced");
	device->CreateTexture(&desc, nullptr, &res.temporal[0]);
	device->SetName(&res.temporal[0], "rtshadow_temporal[0]");
	device->CreateTexture(&desc, nullptr, &res.temporal[1]);
	device->SetName(&res.temporal[1], "rtshadow_temporal[1]");

	desc.format = Format::R11G11B10_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.normals);
	device->SetName(&res.normals, "rtshadow_normals");

	GPUBufferDesc bd;
	bd.stride = sizeof(uint4);
	bd.size = bd.stride *
		((desc.width + 7) / 8) *
		((desc.height + 3) / 4);
	bd.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bd.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateBuffer(&bd, nullptr, &res.tiles);
	device->SetName(&res.tiles, "rtshadow_tiles");
	device->CreateBuffer(&bd, nullptr, &res.metadata);
	device->SetName(&res.metadata, "rtshadow_metadata");

	for (int i = 0; i < 4; ++i)
	{
		desc.format = Format::R16G16_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.scratch[i][0]);
		device->SetName(&res.scratch[i][0], "rtshadow_scratch[i][0]");
		device->CreateTexture(&desc, nullptr, &res.scratch[i][1]);
		device->SetName(&res.scratch[i][1], "rtshadow_scratch[i][1]");

		desc.format = Format::R11G11B10_FLOAT;
		device->CreateTexture(&desc, nullptr, &res.moments[i][0]);
		device->SetName(&res.moments[i][0], "rtshadow_moments[i][0]");
		device->CreateTexture(&desc, nullptr, &res.moments[i][1]);
		device->SetName(&res.moments[i][1], "rtshadow_moments[i][1]");
	}

	desc.format = Format::R8G8B8A8_UNORM;
	device->CreateTexture(&desc, nullptr, &res.denoised);
	device->SetName(&res.denoised, "rtshadow_denoised");
}
void Postprocess_RTShadow(
	const RTShadowResources& res,
	const Scene& scene,
	const GPUBuffer& entityTiles_Opaque,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd
)
{
	if (!device->CheckCapability(GraphicsDeviceCapability::RAYTRACING))
		return;

	if (!scene.TLAS.IsValid() && !scene.BVH.IsValid())
		return;

	if (wi::jobsystem::IsBusy(raytracing_ctx))
		return;

	device->EventBegin("Postprocess_RTShadow", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("RTShadow", cmd);

	if (res.frame == 0)
	{
		// Maybe we don't need to clear them all, but it's safer this way:
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.raytraced, res.raytraced.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.temporal[0], res.temporal[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.temporal[1], res.temporal[1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.denoised, res.denoised.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0][0], res.moments[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[0][1], res.moments[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1][0], res.moments[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[1][1], res.moments[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[2][0], res.moments[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[2][1], res.moments[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[3][0], res.moments[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.moments[3][1], res.moments[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
		device->ClearUAV(&res.temporal[0], 0, cmd);
		device->ClearUAV(&res.temporal[1], 0, cmd);
		device->ClearUAV(&res.denoised, 0, cmd);
		device->ClearUAV(&res.scratch[0][0], 0, cmd);
		device->ClearUAV(&res.scratch[0][1], 0, cmd);
		device->ClearUAV(&res.scratch[1][0], 0, cmd);
		device->ClearUAV(&res.scratch[1][1], 0, cmd);
		device->ClearUAV(&res.scratch[2][0], 0, cmd);
		device->ClearUAV(&res.scratch[2][1], 0, cmd);
		device->ClearUAV(&res.scratch[3][0], 0, cmd);
		device->ClearUAV(&res.scratch[3][1], 0, cmd);
		device->ClearUAV(&res.moments[0][0], 0, cmd);
		device->ClearUAV(&res.moments[0][1], 0, cmd);
		device->ClearUAV(&res.moments[1][0], 0, cmd);
		device->ClearUAV(&res.moments[1][1], 0, cmd);
		device->ClearUAV(&res.moments[2][0], 0, cmd);
		device->ClearUAV(&res.moments[2][1], 0, cmd);
		device->ClearUAV(&res.moments[3][0], 0, cmd);
		device->ClearUAV(&res.moments[3][1], 0, cmd);
		for (auto& x : barriers)
		{
			std::swap(x.image.layout_before, x.image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	BindCommonResources(cmd);

	device->EventBegin("Raytrace", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW], cmd);

	PostProcess postprocess = {};
	postprocess.resolution.x = res.raytraced.desc.width;
	postprocess.resolution.y = res.raytraced.desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.w = (float)res.frame;
	uint8_t instanceInclusionMask = raytracing_inclusion_mask_shadow;
	std::memcpy(&postprocess.params1.x, &instanceInclusionMask, sizeof(instanceInclusionMask));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&res.raytraced,
		&res.normals,
		&res.tiles
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.raytraced, res.raytraced.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.normals, res.normals.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.tiles, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&output, 0, cmd);
	device->ClearUAV(&res.raytraced, 0, cmd);
	device->ClearUAV(&res.normals, 0, cmd);
	device->ClearUAV(&res.tiles, 0, cmd);
	device->Barrier(GPUBarrier::Memory(), cmd);

	device->Dispatch(
		(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.raytraced, ResourceState::UNORDERED_ACCESS, res.raytraced.desc.layout),
			GPUBarrier::Image(&res.normals, ResourceState::UNORDERED_ACCESS, res.normals.desc.layout),
			GPUBarrier::Buffer(&res.tiles, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);

	int temporal_output = res.frame % 2;
	int temporal_history = 1 - temporal_output;

	// Denoise - Tile Classification:
	{
		device->EventBegin("Denoise - Tile Classification", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TILECLASSIFICATION], cmd);
		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.tiles, 2, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.metadata, ResourceState::SHADER_RESOURCE_COMPUTE, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[0][temporal_output], res.moments[0][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[1][temporal_output], res.moments[1][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[2][temporal_output], res.moments[2][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.moments[3][temporal_output], res.moments[3][temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		for (int i = 0; i < 4; ++i)
		{
			rtshadow_denoise_lightindex = float(i);
			device->PushConstants(&postprocess, sizeof(postprocess), cmd);

			device->BindResource(&res.moments[i][temporal_history], 3, cmd);
			device->BindResource(&res.scratch[i][1], 4, cmd);

			const GPUResource* uavs[] = {
				&res.metadata,
				&res.scratch[i][0],
				&res.moments[i][temporal_output],
			};
			device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

			device->Dispatch(
				(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
				1,
				cmd
			);
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.metadata, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE_COMPUTE),
				GPUBarrier::Image(&res.scratch[0][0], ResourceState::UNORDERED_ACCESS, res.scratch[0][0].desc.layout),
				GPUBarrier::Image(&res.scratch[1][0], ResourceState::UNORDERED_ACCESS, res.scratch[1][0].desc.layout),
				GPUBarrier::Image(&res.scratch[2][0], ResourceState::UNORDERED_ACCESS, res.scratch[2][0].desc.layout),
				GPUBarrier::Image(&res.scratch[3][0], ResourceState::UNORDERED_ACCESS, res.scratch[3][0].desc.layout),
				GPUBarrier::Image(&res.moments[0][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[0][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[1][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[1][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[2][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[2][temporal_output].desc.layout),
				GPUBarrier::Image(&res.moments[3][temporal_output], ResourceState::UNORDERED_ACCESS, res.moments[3][temporal_output].desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Denoise - Spatial filtering:
	{
		device->EventBegin("Denoise - Filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_FILTER], cmd);

		device->BindResource(&res.normals, 0, cmd);
		device->BindResource(&res.metadata, 1, cmd);

		// pass0:
		{
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][1].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.denoised, res.denoised.desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 0;
			postprocess.params1.y = 1;

			for (int i = 0; i < 4; ++i)
			{
				rtshadow_denoise_lightindex = float(i);
				device->PushConstants(&postprocess, sizeof(postprocess), cmd);

				device->BindResource(&res.scratch[i][0], 2, cmd);
				const GPUResource* uavs[] = {
					&res.scratch[i][1],
					&res.denoised
				};
				device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

				device->Dispatch(
					(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					1,
					cmd
				);
			}
		}

		// pass1:
		{
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[0][1], ResourceState::UNORDERED_ACCESS, res.scratch[0][1].desc.layout),
					GPUBarrier::Image(&res.scratch[1][1], ResourceState::UNORDERED_ACCESS, res.scratch[1][1].desc.layout),
					GPUBarrier::Image(&res.scratch[2][1], ResourceState::UNORDERED_ACCESS, res.scratch[2][1].desc.layout),
					GPUBarrier::Image(&res.scratch[3][1], ResourceState::UNORDERED_ACCESS, res.scratch[3][1].desc.layout),
					GPUBarrier::Image(&res.scratch[0][0], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][0], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][0], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][0], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 1;
			postprocess.params1.y = 2;

			for (int i = 0; i < 4; ++i)
			{
				rtshadow_denoise_lightindex = float(i);
				device->PushConstants(&postprocess, sizeof(postprocess), cmd);

				device->BindResource(&res.scratch[i][1], 2, cmd);
				const GPUResource* uavs[] = {
					&res.scratch[i][0],
					&res.denoised
				};
				device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

				device->Dispatch(
					(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					1,
					cmd
				);
			}
		}

		// pass2:
		{
			{
				GPUBarrier barriers[] = {
					GPUBarrier::Image(&res.scratch[0][0], ResourceState::UNORDERED_ACCESS, res.scratch[0][0].desc.layout),
					GPUBarrier::Image(&res.scratch[1][0], ResourceState::UNORDERED_ACCESS, res.scratch[1][0].desc.layout),
					GPUBarrier::Image(&res.scratch[2][0], ResourceState::UNORDERED_ACCESS, res.scratch[2][0].desc.layout),
					GPUBarrier::Image(&res.scratch[3][0], ResourceState::UNORDERED_ACCESS, res.scratch[3][0].desc.layout),
					GPUBarrier::Image(&res.scratch[0][1], res.scratch[0][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[1][1], res.scratch[1][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[2][1], res.scratch[2][0].desc.layout, ResourceState::UNORDERED_ACCESS),
					GPUBarrier::Image(&res.scratch[3][1], res.scratch[3][0].desc.layout, ResourceState::UNORDERED_ACCESS),
				};
				device->Barrier(barriers, arraysize(barriers), cmd);
			}

			postprocess.params1.x = 2;
			postprocess.params1.y = 4;

			for (int i = 0; i < 4; ++i)
			{
				rtshadow_denoise_lightindex = float(i);
				device->PushConstants(&postprocess, sizeof(postprocess), cmd);

				device->BindResource(&res.scratch[i][0], 2, cmd);
				const GPUResource* uavs[] = {
					&res.scratch[i][1],
					&res.denoised
				};
				device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

				device->Dispatch(
					(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
					1,
					cmd
				);
			}
		}

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.scratch[0][1], ResourceState::UNORDERED_ACCESS, res.scratch[0][1].desc.layout),
				GPUBarrier::Image(&res.scratch[1][1], ResourceState::UNORDERED_ACCESS, res.scratch[1][1].desc.layout),
				GPUBarrier::Image(&res.scratch[2][1], ResourceState::UNORDERED_ACCESS, res.scratch[2][1].desc.layout),
				GPUBarrier::Image(&res.scratch[3][1], ResourceState::UNORDERED_ACCESS, res.scratch[3][1].desc.layout),
				GPUBarrier::Image(&res.denoised, ResourceState::UNORDERED_ACCESS, res.denoised.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}
	res.frame++;

	// Temporal pass:
	{
		device->EventBegin("Temporal Denoise", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_DENOISE_TEMPORAL], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.raytraced, 0, cmd);
		device->BindResource(&res.temporal[temporal_history], 1, cmd);
		device->BindResource(&res.denoised, 3, cmd);

		const GPUResource* uavs[] = {
			&res.temporal[temporal_output],
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.temporal[temporal_output], res.temporal[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.temporal[temporal_output], ResourceState::UNORDERED_ACCESS, res.temporal[temporal_output].desc.layout),
				GPUBarrier::Memory(&output),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	postprocess.resolution.x = output.desc.width;
	postprocess.resolution.y = output.desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = (float)res.raytraced.desc.width;
	postprocess.params0.y = (float)res.raytraced.desc.height;
	postprocess.params0.z = 1.0f / postprocess.params0.x;
	postprocess.params0.w = 1.0f / postprocess.params0.y;

	// Upsample pass:
	{
		device->EventBegin("Upsample", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.temporal[temporal_output], 0, cmd);
		device->BindResource(&lineardepth, 1, cmd, 1);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void CreateScreenSpaceShadowResources(ScreenSpaceShadowResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	desc.format = Format::R32G32B32A32_UINT;
	device->CreateTexture(&desc, nullptr, &res.lowres);
	device->SetName(&res.lowres, "lowres");
}
void Postprocess_ScreenSpaceShadow(
	const ScreenSpaceShadowResources& res,
	const GPUBuffer& entityTiles_Opaque,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	float range,
	uint32_t samplecount
)
{
	device->EventBegin("Postprocess_ScreenSpaceShadow", cmd);
	auto prof_range = wi::profiler::BeginRangeGPU("ScreenSpaceShadow", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SCREENSPACESHADOW], cmd);

	PostProcess postprocess;
	postprocess.resolution.x = res.lowres.desc.width;
	postprocess.resolution.y = res.lowres.desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = range;
	postprocess.params0.y = (float)samplecount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&res.lowres,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.lowres, res.lowres.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->ClearUAV(&output, 0, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.lowres, ResourceState::UNORDERED_ACCESS, res.lowres.desc.layout),
			GPUBarrier::Memory(&output),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	postprocess.resolution.x = output.desc.width;
	postprocess.resolution.y = output.desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = (float)res.lowres.desc.width;
	postprocess.params0.y = (float)res.lowres.desc.height;
	postprocess.params0.z = 1.0f / postprocess.params0.x;
	postprocess.params0.w = 1.0f / postprocess.params0.y;

	// Upsample pass:
	{
		device->EventBegin("Upsample", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_RTSHADOW_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.lowres, 0, cmd);
		device->BindResource(&lineardepth, 1, cmd, 1);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(postprocess.resolution.x + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(postprocess.resolution.y + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(prof_range);
	device->EventEnd(cmd);
}
void Postprocess_LightShafts(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	const XMFLOAT2& center,
	float strength
)
{
	device->EventBegin("Postprocess_LightShafts", cmd);
	auto range = wi::profiler::BeginRangeGPU("LightShafts", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_LIGHTSHAFTS], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = 0.65f;		// density
	postprocess.params0.y = 0.25f;		// weight
	postprocess.params0.z = 0.945f;		// decay
	postprocess.params0.w = strength;	// exposure
	postprocess.params1.x = center.x;
	postprocess.params1.y = center.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateDepthOfFieldResources(DepthOfFieldResources& res, XMUINT2 resolution)
{
	XMUINT2 tile_count = {};
	tile_count.x = (resolution.x + DEPTHOFFIELD_TILESIZE - 1) / DEPTHOFFIELD_TILESIZE;
	tile_count.y = (resolution.y + DEPTHOFFIELD_TILESIZE - 1) / DEPTHOFFIELD_TILESIZE;

	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.width = tile_count.x;
	tile_desc.height = tile_count.y;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_neighborhoodmax);
	tile_desc.format = Format::R16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin);

	tile_desc.height = resolution.y;
	tile_desc.format = Format::R16G16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax_horizontal);
	tile_desc.format = Format::R16_FLOAT;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin_horizontal);

	TextureDesc presort_desc;
	presort_desc.type = TextureDesc::Type::TEXTURE_2D;
	presort_desc.width = resolution.x / 2;
	presort_desc.height = resolution.y / 2;
	presort_desc.format = Format::R11G11B10_FLOAT;
	presort_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateTexture(&presort_desc, nullptr, &res.texture_presort);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_prefilter);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_main);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_postfilter);
	presort_desc.format = Format::R8_UNORM;
	device->CreateTexture(&presort_desc, nullptr, &res.texture_alpha1);
	device->CreateTexture(&presort_desc, nullptr, &res.texture_alpha2);


	GPUBufferDesc bufferdesc;
	bufferdesc.stride = sizeof(PostprocessTileStatistics);
	bufferdesc.size = bufferdesc.stride;
	bufferdesc.bind_flags = BindFlag::UNORDERED_ACCESS;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_statistics);

	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_count.x * tile_count.y * bufferdesc.stride;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_expensive);
}
void Postprocess_DepthOfField(
	const DepthOfFieldResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float coc_scale,
	float max_coc
)
{
	device->EventBegin("Postprocess_DepthOfField", cmd);
	auto range = wi::profiler::BeginRangeGPU("Depth of Field", cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_tilemax_horizontal, res.texture_tilemax_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemin_horizontal, res.texture_tilemin_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemax, res.texture_tilemax.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemin, res.texture_tilemin.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_neighborhoodmax, res.texture_neighborhoodmax.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_presort, res.texture_presort.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_prefilter, res.texture_prefilter.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_main, res.texture_main.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_alpha1, res.texture_alpha1.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_postfilter, res.texture_postfilter.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_alpha2, res.texture_alpha2.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNDEFINED, ResourceState::COPY_DST),
			GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&res.texture_tilemax_horizontal, 0, cmd);
	device->ClearUAV(&res.texture_tilemin_horizontal, 0, cmd);
	device->ClearUAV(&res.texture_tilemax, 0, cmd);
	device->ClearUAV(&res.texture_tilemin, 0, cmd);
	device->ClearUAV(&res.texture_neighborhoodmax, 0, cmd);
	device->ClearUAV(&res.texture_presort, 0, cmd);
	device->ClearUAV(&res.texture_prefilter, 0, cmd);
	device->ClearUAV(&res.texture_main, 0, cmd);
	device->ClearUAV(&res.texture_alpha1, 0, cmd);
	device->ClearUAV(&res.texture_postfilter, 0, cmd);
	device->ClearUAV(&res.texture_alpha2, 0, cmd);
	device->ClearUAV(&output, 0, cmd);

	PostprocessTileStatistics tile_stats = {};
	tile_stats.dispatch_earlyexit.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_earlyexit.ThreadGroupCountY = 1;
	tile_stats.dispatch_earlyexit.ThreadGroupCountZ = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_cheap.ThreadGroupCountY = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountZ = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_expensive.ThreadGroupCountY = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountZ = 1;
	device->UpdateBuffer(&res.buffer_tile_statistics, &tile_stats, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	dof_cocscale = coc_scale;
	dof_maxcoc = max_coc;

	// Compute tile max COC (horizontal):
	{
		device->EventBegin("TileMax - Horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_HORIZONTAL], cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(
			(res.texture_tilemax_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemax_horizontal.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemin_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile max COC (vertical):
	{
		device->EventBegin("TileMax - Vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_TILEMAXCOC_VERTICAL], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax,
			&res.texture_tilemin,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_tilemax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax, ResourceState::UNORDERED_ACCESS, res.texture_tilemax.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin, ResourceState::UNORDERED_ACCESS, res.texture_tilemin.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute max COC for each tiles' neighborhood
	{
		device->EventBegin("NeighborhoodMax", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_NEIGHBORHOODMAXCOC], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax,
			&res.texture_tilemin
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
			&res.texture_neighborhoodmax
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_neighborhoodmax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_neighborhoodmax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
				GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.texture_neighborhoodmax, ResourceState::UNORDERED_ACCESS, res.texture_neighborhoodmax.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Switch to half res:
	postprocess.resolution.x = desc.width / 2;
	postprocess.resolution.y = desc.height / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Prepass:
	{
		device->EventBegin("Prepass", cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_neighborhoodmax,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_presort,
			&res.texture_prefilter
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->BindResource(&res.buffer_tiles_earlyexit, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS_EARLYEXIT], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_earlyexit), cmd);

		device->BindResource(&res.buffer_tiles_cheap, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_cheap), cmd);

		device->BindResource(&res.buffer_tiles_expensive, 2, cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_PREPASS], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_expensive), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_presort, ResourceState::UNORDERED_ACCESS, res.texture_presort.desc.layout),
				GPUBarrier::Image(&res.texture_prefilter, ResourceState::UNORDERED_ACCESS, res.texture_prefilter.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Main pass:
	{
		device->EventBegin("Main pass", cmd);

		const GPUResource* resarray[] = {
			&res.texture_neighborhoodmax,
			&res.texture_presort,
			&res.texture_prefilter,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_main,
			&res.texture_alpha1
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_EARLYEXIT], cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_earlyexit), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN_CHEAP], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_cheap), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_MAIN], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_expensive), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_main, ResourceState::UNORDERED_ACCESS, res.texture_main.desc.layout),
				GPUBarrier::Image(&res.texture_alpha1, ResourceState::UNORDERED_ACCESS, res.texture_alpha1.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Post filter:
	{
		device->EventBegin("Post filter", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_POSTFILTER], cmd);

		const GPUResource* resarray[] = {
			&res.texture_main,
			&res.texture_alpha1
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.texture_postfilter,
			&res.texture_alpha2
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_postfilter.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_postfilter.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_postfilter, ResourceState::UNORDERED_ACCESS, res.texture_postfilter.desc.layout),
				GPUBarrier::Image(&res.texture_alpha2, ResourceState::UNORDERED_ACCESS, res.texture_alpha2.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Switch to full res:
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Upsample pass:
	{
		device->EventBegin("Upsample pass", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DEPTHOFFIELD_UPSAMPLE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_postfilter,
			&res.texture_alpha2,
			&res.texture_neighborhoodmax
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Outline(
	const Texture& input,
	CommandList cmd,
	float threshold,
	float thickness,
	const XMFLOAT4& color
)
{
	device->EventBegin("Postprocess_Outline", cmd);
	auto range = wi::profiler::BeginRangeGPU("Outline", cmd);

	device->BindPipelineState(&PSO_outline, cmd);

	device->BindResource(&input, 0, cmd);

	PostProcess postprocess;
	postprocess.resolution.x = (uint)input.GetDesc().width;
	postprocess.resolution.y = (uint)input.GetDesc().height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = threshold;
	postprocess.params0.y = thickness;
	postprocess.params1.x = color.x;
	postprocess.params1.y = color.y;
	postprocess.params1.z = color.z;
	postprocess.params1.w = color.w;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->Draw(3, 0, cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateMotionBlurResources(MotionBlurResources& res, XMUINT2 resolution)
{
	XMUINT2 tile_count = {};
	tile_count.x = (resolution.x + MOTIONBLUR_TILESIZE - 1) / MOTIONBLUR_TILESIZE;
	tile_count.y = (resolution.y + MOTIONBLUR_TILESIZE - 1) / MOTIONBLUR_TILESIZE;

	TextureDesc tile_desc;
	tile_desc.type = TextureDesc::Type::TEXTURE_2D;
	tile_desc.format = Format::R16G16_FLOAT;
	tile_desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	tile_desc.width = tile_count.x;
	tile_desc.height = tile_count.y;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_neighborhoodmax);

	tile_desc.height = resolution.y;
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemax_horizontal);
	device->CreateTexture(&tile_desc, nullptr, &res.texture_tilemin_horizontal);


	GPUBufferDesc bufferdesc;
	bufferdesc.stride = sizeof(PostprocessTileStatistics);
	bufferdesc.size = bufferdesc.stride;
	bufferdesc.bind_flags = BindFlag::UNORDERED_ACCESS;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED | ResourceMiscFlag::INDIRECT_ARGS;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tile_statistics);

	bufferdesc.stride = sizeof(uint);
	bufferdesc.size = tile_count.x * tile_count.y * bufferdesc.stride;
	bufferdesc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	bufferdesc.misc_flags = ResourceMiscFlag::BUFFER_STRUCTURED;
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_earlyexit);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_cheap);
	device->CreateBuffer(&bufferdesc, nullptr, &res.buffer_tiles_expensive);
}
void Postprocess_MotionBlur(
	float dt,
	const MotionBlurResources& res,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float strength
)
{
	device->EventBegin("Postprocess_MotionBlur", cmd);
	auto range = wi::profiler::BeginRangeGPU("MotionBlur", cmd);

	const TextureDesc& desc = output.GetDesc();

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_tilemax_horizontal, res.texture_tilemax_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemin_horizontal, res.texture_tilemin_horizontal.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemax, res.texture_tilemax.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_tilemin, res.texture_tilemin.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_neighborhoodmax, res.texture_neighborhoodmax.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNDEFINED, ResourceState::COPY_DST),
			GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNDEFINED, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&res.texture_tilemax_horizontal, 0, cmd);
	device->ClearUAV(&res.texture_tilemin_horizontal, 0, cmd);
	device->ClearUAV(&res.texture_tilemax, 0, cmd);
	device->ClearUAV(&res.texture_tilemin, 0, cmd);
	device->ClearUAV(&res.texture_neighborhoodmax, 0, cmd);
	device->ClearUAV(&output, 0, cmd);

	PostprocessTileStatistics tile_stats = {};
	tile_stats.dispatch_earlyexit.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_earlyexit.ThreadGroupCountY = 1;
	tile_stats.dispatch_earlyexit.ThreadGroupCountZ = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_cheap.ThreadGroupCountY = 1;
	tile_stats.dispatch_cheap.ThreadGroupCountZ = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountX = 0; // shader atomic
	tile_stats.dispatch_expensive.ThreadGroupCountY = 1;
	tile_stats.dispatch_expensive.ThreadGroupCountZ = 1;
	device->UpdateBuffer(&res.buffer_tile_statistics, &tile_stats, cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::COPY_DST, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	motionblur_strength = dt > 0 ? (strength / 60.0f / dt) : 0; // align to shutter speed

	// Compute tile max velocities (horizontal):
	{
		device->EventBegin("TileMax - Horizontal", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_HORIZONTAL], cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax_horizontal,
			&res.texture_tilemin_horizontal,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->Dispatch(
			(res.texture_tilemax_horizontal.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax_horizontal.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemax_horizontal.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin_horizontal, ResourceState::UNORDERED_ACCESS, res.texture_tilemin_horizontal.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute tile max velocities (vertical):
	{
		device->EventBegin("TileMax - Vertical", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_TILEMAXVELOCITY_VERTICAL], cmd);

		device->BindResource(&res.texture_tilemax_horizontal, 0, cmd);
		device->BindResource(&res.texture_tilemin_horizontal, 1, cmd);

		const GPUResource* uavs[] = {
			&res.texture_tilemax,
			&res.texture_tilemin,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_tilemax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_tilemax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_tilemax, ResourceState::UNORDERED_ACCESS, res.texture_tilemax.desc.layout),
				GPUBarrier::Image(&res.texture_tilemin, ResourceState::UNORDERED_ACCESS, res.texture_tilemin.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Compute max velocities for each tiles' neighborhood
	{
		device->EventBegin("NeighborhoodMax", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_NEIGHBORHOODMAXVELOCITY], cmd);

		const GPUResource* resarray[] = {
			&res.texture_tilemax,
			&res.texture_tilemin
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&res.buffer_tile_statistics,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
			&res.texture_neighborhoodmax
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_neighborhoodmax.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_neighborhoodmax.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Buffer(&res.buffer_tile_statistics, ResourceState::UNORDERED_ACCESS, ResourceState::INDIRECT_ARGUMENT),
				GPUBarrier::Buffer(&res.buffer_tiles_earlyexit, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_cheap, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Buffer(&res.buffer_tiles_expensive, ResourceState::UNORDERED_ACCESS, ResourceState::SHADER_RESOURCE),
				GPUBarrier::Image(&res.texture_neighborhoodmax, ResourceState::UNORDERED_ACCESS, res.texture_neighborhoodmax.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	// Tile jobs:
	{
		device->EventBegin("MotionBlur Jobs", cmd);

		const GPUResource* resarray[] = {
			&input,
			&res.texture_neighborhoodmax,
			&res.buffer_tiles_earlyexit,
			&res.buffer_tiles_cheap,
			&res.buffer_tiles_expensive,
		};
		device->BindResources(resarray, 0, arraysize(resarray), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_EARLYEXIT], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_earlyexit), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR_CHEAP], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_cheap), cmd);

		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_MOTIONBLUR], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);
		device->DispatchIndirect(&res.buffer_tile_statistics, offsetof(PostprocessTileStatistics, dispatch_expensive), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateAerialPerspectiveResources(AerialPerspectiveResources& res, XMUINT2 resolution)
{
	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.width = resolution.x;
	desc.height = resolution.y;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_output);
	device->SetName(&res.texture_output, "texture_output");
}
void Postprocess_AerialPerspective(
	const AerialPerspectiveResources& res,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_AerialPerspective", cmd);
	auto range = wi::profiler::BeginRangeGPU("Aerial Perspective", cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = res.texture_output.GetDesc();
	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Aerial Perspective render pass:
	{
		device->EventBegin("Aerial Perspective Render", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_AERIALPERSPECTIVE], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&res.texture_output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_output, res.texture_output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_output.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_output.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&res.texture_output, ResourceState::UNORDERED_ACCESS, res.texture_output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->EventEnd(cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateVolumetricCloudResources(VolumetricCloudResources& res, XMUINT2 resolution)
{
	res.ResetFrame();
	res.final_resolution = resolution;

	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.width = resolution.x / 4;
	desc.height = resolution.y / 4;
	desc.format = Format::R16G16B16A16_FLOAT;
	desc.layout = ResourceState::SHADER_RESOURCE_COMPUTE;
	device->CreateTexture(&desc, nullptr, &res.texture_cloudRender);
	device->SetName(&res.texture_cloudRender, "texture_cloudRender");
	desc.format = Format::R32G32_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_cloudDepth);
	device->SetName(&res.texture_cloudDepth, "texture_cloudDepth");

	desc.width = resolution.x / 2;
	desc.height = resolution.y / 2;
	desc.format = Format::R16G16B16A16_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_reproject[0]);
	device->SetName(&res.texture_reproject[0], "texture_reproject[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_reproject[1]);
	device->SetName(&res.texture_reproject[1], "texture_reproject[1]");
	desc.format = Format::R32G32_FLOAT;
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_depth[0]);
	device->SetName(&res.texture_reproject_depth[0], "texture_reproject_depth[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_depth[1]);
	device->SetName(&res.texture_reproject_depth[1], "texture_reproject_depth[1]");
	desc.format = Format::R8_UNORM;
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_additional[0]);
	device->SetName(&res.texture_reproject_additional[0], "texture_reproject_additional[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_reproject_additional[1]);
	device->SetName(&res.texture_reproject_additional[1], "texture_reproject_additional[1]");

	desc.format = Format::R8_UNORM;
	desc.swizzle = SwizzleFromString("rrr1");
	device->CreateTexture(&desc, nullptr, &res.texture_cloudMask);
	device->SetName(&res.texture_cloudMask, "texture_cloudMask");
}
void Postprocess_VolumetricClouds(
	const VolumetricCloudResources& res,
	CommandList cmd,
	const CameraComponent& camera,
	const CameraComponent& camera_previous,
	const CameraComponent& camera_reflection,
	const bool jitterEnabled,
	const Texture* weatherMapFirst,
	const Texture* weatherMapSecond
)
{
	device->EventBegin("Postprocess_VolumetricClouds", cmd);
	auto range = wi::profiler::BeginRangeGPU("Volumetric Clouds", cmd);

	// Disable Temporal AA and FSR2 jitter for clouds
	if (jitterEnabled)
	{
		CameraComponent camera_clouds = camera;
		camera_clouds.jitter = XMFLOAT2(0, 0);
		camera_clouds.UpdateCamera();

		CameraComponent camera_previous_clouds = camera_previous;
		camera_previous_clouds.jitter = XMFLOAT2(0, 0);
		camera_previous_clouds.UpdateCamera();

		BindCameraCB(camera_clouds, camera_previous_clouds, camera_reflection, cmd);
	}

	BindCommonResources(cmd);

	PostProcess postprocess;

	// Render quarter-res: 
	postprocess.resolution.x = res.final_resolution.x / 4;
	postprocess.resolution.y = res.final_resolution.y / 4;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	volumetricclouds_frame = (float)res.frame;

	// Parse reproject info
	postprocess.params0.x = (float)res.texture_reproject[0].GetDesc().width;
	postprocess.params0.y = (float)res.texture_reproject[0].GetDesc().height;
	postprocess.params0.z = 1.0f / postprocess.params0.x;
	postprocess.params0.w = 1.0f / postprocess.params0.y;

	// Cloud render pass:
	{
		device->EventBegin("Volumetric Cloud Render", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_RENDER], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&texture_shapeNoise, 0, cmd);
		device->BindResource(&texture_detailNoise, 1, cmd);
		device->BindResource(&texture_curlNoise, 2, cmd);

		if (weatherMapFirst != nullptr)
		{
			device->BindResource(weatherMapFirst, 3, cmd);
		}
		else
		{
			device->BindResource(&texture_weatherMap, 3, cmd);
		}

		if (weatherMapSecond != nullptr)
		{
			device->BindResource(weatherMapSecond, 4, cmd);
		}
		else
		{
			device->BindResource(&texture_weatherMap, 4, cmd);
		}

		const GPUResource* uavs[] = {
			&res.texture_cloudRender,
			&res.texture_cloudDepth,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&res.texture_cloudRender, res.texture_cloudRender.desc.layout, ResourceState::UNORDERED_ACCESS),
				GPUBarrier::Image(&res.texture_cloudDepth, res.texture_cloudDepth.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch(
			(res.texture_cloudRender.GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_cloudRender.GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	// Render half-res:
	postprocess.resolution.x = res.final_resolution.x / 2;
	postprocess.resolution.y = res.final_resolution.y / 2;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	
	int temporal_output = res.GetTemporalOutputIndex();
	int temporal_history = res.GetTemporalInputIndex();

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_cloudRender, ResourceState::UNORDERED_ACCESS, res.texture_cloudRender.desc.layout),
			GPUBarrier::Image(&res.texture_cloudDepth, ResourceState::UNORDERED_ACCESS, res.texture_cloudDepth.desc.layout),

			GPUBarrier::Image(&res.texture_reproject[temporal_output], res.texture_reproject[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_reproject_depth[temporal_output], res.texture_reproject_depth[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_reproject_additional[temporal_output], res.texture_reproject_additional[temporal_output].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_cloudMask, res.texture_cloudMask.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Cloud reprojection pass:
	{
		device->EventBegin("Volumetric Cloud Reproject", cmd);
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_VOLUMETRICCLOUDS_REPROJECT], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.texture_cloudRender, 0, cmd);
		device->BindResource(&res.texture_cloudDepth, 1, cmd);
		device->BindResource(&res.texture_reproject[temporal_history], 2, cmd);
		device->BindResource(&res.texture_reproject_depth[temporal_history], 3, cmd);
		device->BindResource(&res.texture_reproject_additional[temporal_history], 4, cmd);

		const GPUResource* uavs[] = {
			&res.texture_reproject[temporal_output],
			&res.texture_reproject_depth[temporal_output],
			&res.texture_reproject_additional[temporal_output],
			&res.texture_cloudMask,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Dispatch(
			(res.texture_reproject[temporal_output].GetDesc().width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(res.texture_reproject[temporal_output].GetDesc().height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->EventEnd(cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_reproject[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_reproject[temporal_output].desc.layout),
			GPUBarrier::Image(&res.texture_reproject_depth[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_reproject_depth[temporal_output].desc.layout),
			GPUBarrier::Image(&res.texture_reproject_additional[temporal_output], ResourceState::UNORDERED_ACCESS, res.texture_reproject_additional[temporal_output].desc.layout),
			GPUBarrier::Image(&res.texture_cloudMask, ResourceState::UNORDERED_ACCESS, res.texture_cloudMask.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	// Rebind original cameras for other effects after this:
	BindCameraCB(camera, camera_previous, camera_reflection, cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_VolumetricClouds_Upsample(
	const VolumetricCloudResources& res,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_VolumetricClouds_Upsample", cmd);
	auto range = wi::profiler::BeginRangeGPU("Volumetric Clouds Upsample", cmd);

	BindCommonResources(cmd);

	PostProcess postprocess;
	volumetricclouds_frame = (float)res.frame;

	int temporal_output = res.GetTemporalOutputIndex();

	// Render full-res: (output)
	postprocess.resolution.x = res.final_resolution.x;
	postprocess.resolution.y = res.final_resolution.y;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;

	// Parse reproject info
	postprocess.params0.x = (float)res.texture_reproject[0].GetDesc().width;
	postprocess.params0.y = (float)res.texture_reproject[0].GetDesc().height;
	postprocess.params0.z = 1.0f / postprocess.params0.x;
	postprocess.params0.w = 1.0f / postprocess.params0.y;

	// Cloud upsample pass:
	{
		device->BindPipelineState(&PSO_volumetricclouds_upsample, cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&res.texture_reproject[temporal_output], 0, cmd);
		device->BindResource(&res.texture_reproject_depth[temporal_output], 1, cmd);

		device->Draw(3, 0, cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_FXAA(
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_FXAA", cmd);
	auto range = wi::profiler::BeginRangeGPU("FXAA", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FXAA], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void CreateTemporalAAResources(TemporalAAResources& res, XMUINT2 resolution)
{
	res.frame = 0;

	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;
	desc.format = Format::R11G11B10_FLOAT;
	desc.width = resolution.x;
	desc.height = resolution.y;
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[0]);
	device->SetName(&res.texture_temporal[0], "TemporalAAResources::texture_temporal[0]");
	device->CreateTexture(&desc, nullptr, &res.texture_temporal[1]);
	device->SetName(&res.texture_temporal[1], "TemporalAAResources::texture_temporal[1]");
}
void Postprocess_TemporalAA(
	const TemporalAAResources& res,
	const Texture& input,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_TemporalAA", cmd);
	auto range = wi::profiler::BeginRangeGPU("Temporal AA Resolve", cmd);
	const bool first_frame = res.frame == 0;
	res.frame++;

	if (first_frame)
	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.texture_temporal[0], res.texture_temporal[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.texture_temporal[1], res.texture_temporal[1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->ClearUAV(&res.texture_temporal[0], 0, cmd);
		device->ClearUAV(&res.texture_temporal[1], 0, cmd);

		std::swap(barriers[0].image.layout_before, barriers[0].image.layout_after);
		std::swap(barriers[1].image.layout_before, barriers[1].image.layout_after);
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_TEMPORALAA], cmd);

	device->BindResource(&input, 0, cmd); // input_current

	if (first_frame)
	{
		device->BindResource(&input, 1, cmd); // input_history
	}
	else
	{
		device->BindResource(res.GetHistory(), 1, cmd); // input_history
	}

	const TextureDesc& desc = res.texture_temporal[0].GetDesc();

	PostProcess postprocess = {};
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = first_frame ? 1.0f : 0.0f;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const Texture* output = res.GetCurrent();

	const GPUResource* uavs[] = {
		output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(output, output->GetDesc().layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(output, ResourceState::UNORDERED_ACCESS, output->GetDesc().layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Sharpen(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float amount
)
{
	device->EventBegin("Postprocess_Sharpen", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_SHARPEN], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = amount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	device->EventEnd(cmd);
}
void Postprocess_Tonemap(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float exposure,
	float brightness,
	float contrast,
	float saturation,
	bool dither,
	const Texture* texture_colorgradinglut,
	const Texture* texture_distortion,
	const GPUBuffer* buffer_luminance,
	const Texture* texture_bloom,
	ColorSpace display_colorspace,
	Tonemap tonemap,
	const Texture* texture_distortion_overlay,
	float hdr_calibration
)
{
	if (!input.IsValid() || !output.IsValid())
	{
		assert(0);
		return;
	}

	device->EventBegin("Postprocess_Tonemap", cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_TONEMAP], cmd);

	const TextureDesc& desc = output.GetDesc();

	assert(texture_colorgradinglut == nullptr || texture_colorgradinglut->desc.type == TextureDesc::Type::TEXTURE_3D); // This must be a 3D lut

	XMHALF4 exposure_brightness_contrast_saturation = XMHALF4(exposure, brightness, contrast, saturation);

	PushConstantsTonemap tonemap_push = {};
	tonemap_push.resolution_rcp.x = 1.0f / desc.width;
	tonemap_push.resolution_rcp.y = 1.0f / desc.height;
	tonemap_push.exposure_brightness_contrast_saturation.x = uint(exposure_brightness_contrast_saturation.v);
	tonemap_push.exposure_brightness_contrast_saturation.y = uint(exposure_brightness_contrast_saturation.v >> 32ull);
	tonemap_push.flags_hdrcalibration = 0;
	if (dither)
	{
		tonemap_push.flags_hdrcalibration |= TONEMAP_FLAG_DITHER;
	}
	if (tonemap == Tonemap::ACES)
	{
		tonemap_push.flags_hdrcalibration |= TONEMAP_FLAG_ACES;
	}
	if (display_colorspace == ColorSpace::SRGB)
	{
		tonemap_push.flags_hdrcalibration |= TONEMAP_FLAG_SRGB;
	}
	tonemap_push.flags_hdrcalibration |= XMConvertFloatToHalf(hdr_calibration) << 16u;
	tonemap_push.texture_input = device->GetDescriptorIndex(&input, SubresourceType::SRV);
	tonemap_push.buffer_input_luminance = device->GetDescriptorIndex(buffer_luminance, SubresourceType::SRV);
	tonemap_push.texture_input_distortion = device->GetDescriptorIndex(texture_distortion, SubresourceType::SRV);
	tonemap_push.texture_input_distortion_overlay = device->GetDescriptorIndex(texture_distortion_overlay, SubresourceType::SRV);
	tonemap_push.texture_colorgrade_lookuptable = device->GetDescriptorIndex(texture_colorgradinglut, SubresourceType::SRV);
	tonemap_push.texture_bloom = device->GetDescriptorIndex(texture_bloom, SubresourceType::SRV);
	tonemap_push.texture_output = device->GetDescriptorIndex(&output, SubresourceType::UAV);
	device->PushConstants(&tonemap_push, sizeof(tonemap_push), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	device->EventEnd(cmd);
}

namespace fsr
{
#define A_CPU
#include "shaders/ffx-fsr/ffx_a.h"
#include "shaders/ffx-fsr/ffx_fsr1.h"
}
void Postprocess_FSR(
	const Texture& input,
	const Texture& temp,
	const Texture& output,
	CommandList cmd,
	float sharpness
)
{
	using namespace fsr;
	device->EventBegin("Postprocess_FSR", cmd);
	auto range = wi::profiler::BeginRangeGPU("Postprocess_FSR", cmd);

	const TextureDesc& desc = output.GetDesc();

	struct FSR
	{
		AU1 const0[4];
		AU1 const1[4];
		AU1 const2[4];
		AU1 const3[4];
	} fsr;

	// Upscaling:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR_UPSCALING], cmd);

		FsrEasuCon(
			fsr.const0,
			fsr.const1,
			fsr.const2,
			fsr.const3,

			// current frame render resolution:
			static_cast<AF1>(input.desc.width),
			static_cast<AF1>(input.desc.height),

			// input container resolution:
			static_cast<AF1>(input.desc.width),
			static_cast<AF1>(input.desc.height),

			// upscaled-to-resolution:
			static_cast<AF1>(temp.desc.width),
			static_cast<AF1>(temp.desc.height)

		);
		device->BindDynamicConstantBuffer(fsr, CBSLOT_FSR, cmd);

		device->BindResource(&input, 0, cmd);

		const GPUResource* uavs[] = {
			&temp,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&temp, temp.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&temp, ResourceState::UNORDERED_ACCESS, temp.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	// Sharpen:
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR_SHARPEN], cmd);

		FsrRcasCon(fsr.const0, sharpness);
		device->BindDynamicConstantBuffer(fsr, CBSLOT_FSR, cmd);

		device->BindResource(&temp, 0, cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

		device->Dispatch((desc.width + 15) / 16, (desc.height + 15) / 16, 1, cmd);

		{
			GPUBarrier barriers[] = {
				GPUBarrier::Memory(),
				GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
			};
			device->Barrier(barriers, arraysize(barriers), cmd);
		}

	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}

namespace fsr2
{
#include "shaders/ffx-fsr2/ffx_core.h"
#include "shaders/ffx-fsr2/ffx_fsr1.h"
#include "shaders/ffx-fsr2/ffx_spd.h"
#include "shaders/ffx-fsr2/ffx_fsr2_callbacks_hlsl.h"
#include "shaders/ffx-fsr2/ffx_fsr2_common.h"
int32_t ffxFsr2GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}
static const int FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH = 16;
static const int FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT = 16;
static const float ffxFsr2MaximumBias[] = {
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.876f,	1.809f,	1.772f,	1.753f,	1.748f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.869f,	1.801f,	1.764f,	1.745f,	1.739f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.976f,	1.841f,	1.774f,	1.737f,	1.716f,	1.71f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.914f,	1.784f,	1.716f,	1.673f,	1.649f,	1.641f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.793f,	1.676f,	1.604f,	1.562f,	1.54f,	1.533f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.802f,	1.619f,	1.536f,	1.492f,	1.467f,	1.454f,	1.449f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.812f,	1.575f,	1.496f,	1.456f,	1.432f,	1.416f,	1.408f,	1.405f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.555f,	1.479f,	1.438f,	1.413f,	1.398f,	1.387f,	1.381f,	1.379f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.812f,	1.555f,	1.474f,	1.43f,	1.404f,	1.387f,	1.376f,	1.368f,	1.363f,	1.362f,
	2.0f,	2.0f,	2.0f,	2.0f,	2.0f,	1.802f,	1.575f,	1.479f,	1.43f,	1.401f,	1.382f,	1.369f,	1.36f,	1.354f,	1.351f,	1.35f,
	2.0f,	2.0f,	1.976f,	1.914f,	1.793f,	1.619f,	1.496f,	1.438f,	1.404f,	1.382f,	1.367f,	1.357f,	1.349f,	1.344f,	1.341f,	1.34f,
	1.876f,	1.869f,	1.841f,	1.784f,	1.676f,	1.536f,	1.456f,	1.413f,	1.387f,	1.369f,	1.357f,	1.347f,	1.341f,	1.336f,	1.333f,	1.332f,
	1.809f,	1.801f,	1.774f,	1.716f,	1.604f,	1.492f,	1.432f,	1.398f,	1.376f,	1.36f,	1.349f,	1.341f,	1.335f,	1.33f,	1.328f,	1.327f,
	1.772f,	1.764f,	1.737f,	1.673f,	1.562f,	1.467f,	1.416f,	1.387f,	1.368f,	1.354f,	1.344f,	1.336f,	1.33f,	1.326f,	1.323f,	1.323f,
	1.753f,	1.745f,	1.716f,	1.649f,	1.54f,	1.454f,	1.408f,	1.381f,	1.363f,	1.351f,	1.341f,	1.333f,	1.328f,	1.323f,	1.321f,	1.32f,
	1.748f,	1.739f,	1.71f,	1.641f,	1.533f,	1.449f,	1.405f,	1.379f,	1.362f,	1.35f,	1.34f,	1.332f,	1.327f,	1.323f,	1.32f,	1.319f,

};
/// The value of Pi.
const float FFX_PI = 3.141592653589793f;
/// An epsilon value for floating point numbers.
const float FFX_EPSILON = 1e-06f;
// Lanczos
static float lanczos2(float value)
{
	return abs(value) < FFX_EPSILON ? 1.f : (sinf(FFX_PI * value) / (FFX_PI * value)) * (sinf(0.5f * FFX_PI * value) / (0.5f * FFX_PI * value));
}
// Calculate halton number for index and base.
static float halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {

		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}
}
void CreateFSR2Resources(FSR2Resources& res, XMUINT2 render_resolution, XMUINT2 presentation_resolution)
{
	using namespace fsr2;
	res.fsr2_constants = {};

	res.fsr2_constants.renderSize[0] = render_resolution.x;
	res.fsr2_constants.renderSize[1] = render_resolution.y;
	res.fsr2_constants.displaySize[0] = presentation_resolution.x;
	res.fsr2_constants.displaySize[1] = presentation_resolution.y;
	res.fsr2_constants.displaySizeRcp[0] = 1.0f / presentation_resolution.x;
	res.fsr2_constants.displaySizeRcp[1] = 1.0f / presentation_resolution.y;

	TextureDesc desc;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS;

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R16G16B16A16_UNORM;
	bool success = device->CreateTexture(&desc, nullptr, &res.adjusted_color);
	assert(success);
	device->SetName(&res.adjusted_color, "fsr2::adjusted_color");

	desc.width = 1;
	desc.height = 1;
	desc.format = Format::R32G32_FLOAT;
	success = device->CreateTexture(&desc, nullptr, &res.exposure);
	assert(success);
	device->SetName(&res.exposure, "fsr2::exposure");

	desc.width = render_resolution.x / 2;
	desc.height = render_resolution.y / 2;
	desc.format = Format::R16_FLOAT;
	desc.mip_levels = 0;
	success = device->CreateTexture(&desc, nullptr, &res.luminance_current);
	assert(success);
	device->SetName(&res.luminance_current, "fsr2::luminance_current");
	for (uint32_t mip = 0; mip < res.luminance_current.desc.mip_levels; ++mip)
	{
		int subresource = device->CreateSubresource(&res.luminance_current, SubresourceType::UAV, 0, 1, mip, 1);
		assert(subresource == mip);
	}
	desc.mip_levels = 1;

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R8G8B8A8_UNORM;
	success = device->CreateTexture(&desc, nullptr, &res.luminance_history);
	assert(success);
	device->SetName(&res.luminance_history, "fsr2::luminance_history");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R32_UINT;
	success = device->CreateTexture(&desc, nullptr, &res.previous_depth);
	assert(success);
	device->SetName(&res.previous_depth, "fsr2::previous_depth");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R16_FLOAT;
	success = device->CreateTexture(&desc, nullptr, &res.dilated_depth);
	assert(success);
	device->SetName(&res.dilated_depth, "fsr2::dilated_depth");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R16G16_FLOAT;
	success = device->CreateTexture(&desc, nullptr, &res.dilated_motion);
	assert(success);
	device->SetName(&res.dilated_motion, "fsr2::dilated_motion");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R8G8_UNORM;
	success = device->CreateTexture(&desc, nullptr, &res.dilated_reactive);
	assert(success);
	device->SetName(&res.dilated_reactive, "fsr2::dilated_reactive");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R8_UNORM;
	success = device->CreateTexture(&desc, nullptr, &res.disocclusion_mask);
	assert(success);
	device->SetName(&res.disocclusion_mask, "fsr2::disocclusion_mask");

	desc.width = render_resolution.x;
	desc.height = render_resolution.y;
	desc.format = Format::R8_UNORM;
	success = device->CreateTexture(&desc, nullptr, &res.reactive_mask);
	assert(success);
	device->SetName(&res.reactive_mask, "fsr2::reactive_mask");

	desc.width = presentation_resolution.x;
	desc.height = presentation_resolution.y;
	desc.format = Format::R11G11B10_FLOAT;
	success = device->CreateTexture(&desc, nullptr, &res.lock_status[0]);
	assert(success);
	device->SetName(&res.lock_status[0], "fsr2::lock_status[0]");
	success = device->CreateTexture(&desc, nullptr, &res.lock_status[1]);
	assert(success);
	device->SetName(&res.lock_status[1], "fsr2::lock_status[1]");

	desc.width = presentation_resolution.x;
	desc.height = presentation_resolution.y;
	desc.format = Format::R16G16B16A16_FLOAT;
	success = device->CreateTexture(&desc, nullptr, &res.output_internal[0]);
	assert(success);
	device->SetName(&res.output_internal[0], "fsr2::output_internal[0]");
	success = device->CreateTexture(&desc, nullptr, &res.output_internal[1]);
	assert(success);
	device->SetName(&res.output_internal[1], "fsr2::output_internal[1]");

	// generate the data for the LUT.
	const uint32_t lanczos2LutWidth = 128;
	int16_t lanczos2Weights[lanczos2LutWidth] = { };

	for (uint32_t currentLanczosWidthIndex = 0; currentLanczosWidthIndex < lanczos2LutWidth; currentLanczosWidthIndex++)
	{
		const float x = 2.0f * currentLanczosWidthIndex / float(lanczos2LutWidth - 1);
		const float y = lanczos2(x);
		lanczos2Weights[currentLanczosWidthIndex] = int16_t(roundf(y * 32767.0f));
	}

	desc.width = lanczos2LutWidth;
	desc.height = 1;
	desc.format = Format::R16_SNORM;
	SubresourceData initdata;
	initdata.data_ptr = lanczos2Weights;
	initdata.row_pitch = desc.width * sizeof(int16_t);
	success = device->CreateTexture(&desc, &initdata, &res.lanczos_lut);
	assert(success);
	device->SetName(&res.lanczos_lut, "fsr2::lanczos_lut");

	int16_t maximumBias[FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT];
	for (uint32_t i = 0; i < FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH * FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT; ++i)
	{
		maximumBias[i] = int16_t(roundf(ffxFsr2MaximumBias[i] / 2.0f * 32767.0f));
	}

	desc.width = (uint32_t)FFX_FSR2_MAXIMUM_BIAS_TEXTURE_WIDTH;
	desc.height = (uint32_t)FFX_FSR2_MAXIMUM_BIAS_TEXTURE_HEIGHT;
	desc.format = Format::R16_SNORM;
	initdata.data_ptr = maximumBias;
	initdata.row_pitch = desc.width * sizeof(int16_t);
	success = device->CreateTexture(&desc, &initdata, &res.maximum_bias_lut);
	assert(success);
	device->SetName(&res.maximum_bias_lut, "fsr2::maximum_bias_lut");

	desc.width = 1;
	desc.height = 1;
	desc.format = Format::R32_UINT;
	desc.bind_flags = BindFlag::UNORDERED_ACCESS;
	desc.layout = ResourceState::UNORDERED_ACCESS;
	success = device->CreateTexture(&desc, nullptr, &res.spd_global_atomic);
	assert(success);
	device->SetName(&res.spd_global_atomic, "fsr2::spd_global_atomic");


}
XMFLOAT2 FSR2Resources::GetJitter() const
{
	int32_t phaseCount = fsr2::ffxFsr2GetJitterPhaseCount(fsr2_constants.renderSize[0], fsr2_constants.displaySize[0]);
	float x = fsr2::halton((fsr2_constants.frameIndex % phaseCount) + 1, 2) - 0.5f;
	float y = fsr2::halton((fsr2_constants.frameIndex % phaseCount) + 1, 3) - 0.5f;
	x = 2 * x / (float)fsr2_constants.renderSize[0];
	y = -2 * y / (float)fsr2_constants.renderSize[1];
	return XMFLOAT2(x, y);
}
void Postprocess_FSR2(
	const FSR2Resources& res,
	const CameraComponent& camera,
	const Texture& input_pre_alpha,
	const Texture& input_post_alpha,
	const Texture& input_depth,
	const Texture& input_velocity,
	const Texture& output,
	CommandList cmd,
	float dt,
	float sharpness
)
{
	using namespace fsr2;
	device->EventBegin("Postprocess_FSR2", cmd);
	auto range = wi::profiler::BeginRangeGPU("Postprocess_FSR2", cmd);

	struct Fsr2SpdConstants
	{
		uint32_t                    mips;
		uint32_t                    numworkGroups;
		uint32_t                    workGroupOffset[2];
		uint32_t                    renderSize[2];
	};
	struct Fsr2RcasConstants
	{
		uint32_t                    rcasConfig[4];
	};

	FSR2Resources::Fsr2Constants& fsr2_constants = res.fsr2_constants;
	fsr2_constants.jitterOffset[0] = camera.jitter.x * fsr2_constants.renderSize[0] * 0.5f;
	fsr2_constants.jitterOffset[1] = camera.jitter.y * fsr2_constants.renderSize[1] * -0.5f;

	// compute the horizontal FOV for the shader from the vertical one.
	const float aspectRatio = (float)fsr2_constants.renderSize[0] / (float)fsr2_constants.renderSize[1];
	const float cameraAngleHorizontal = atan(tan(camera.fov / 2) * aspectRatio) * 2;
	fsr2_constants.tanHalfFOV = tanf(cameraAngleHorizontal * 0.5f);

	const bool inverted_depth = true;
	if (inverted_depth)
	{
		const float c = 0.0f;
		fsr2_constants.deviceToViewDepth[0] = c + FLT_EPSILON;
		fsr2_constants.deviceToViewDepth[1] = -1.00000000f;
		fsr2_constants.deviceToViewDepth[2] = 0.100000001f;
		fsr2_constants.deviceToViewDepth[3] = FLT_EPSILON;

	}
	else
	{
		const float c = -1.0f;
		fsr2_constants.deviceToViewDepth[0] = c - FLT_EPSILON;
		fsr2_constants.deviceToViewDepth[1] = -1.00000000f;
		fsr2_constants.deviceToViewDepth[2] = -0.200019985f;
		fsr2_constants.deviceToViewDepth[3] = FLT_EPSILON;
	}

	// To be updated if resource is larger than the actual image size
	fsr2_constants.depthClipUVScale[0] = float(fsr2_constants.renderSize[0]) / res.disocclusion_mask.desc.width;
	fsr2_constants.depthClipUVScale[1] = float(fsr2_constants.renderSize[1]) / res.disocclusion_mask.desc.height;
	fsr2_constants.postLockStatusUVScale[0] = float(fsr2_constants.displaySize[0]) / res.lock_status[0].desc.width;
	fsr2_constants.postLockStatusUVScale[1] = float(fsr2_constants.displaySize[1]) / res.lock_status[0].desc.height;
	fsr2_constants.reactiveMaskDimRcp[0] = 1.0f / float(res.reactive_mask.desc.width);
	fsr2_constants.reactiveMaskDimRcp[1] = 1.0f / float(res.reactive_mask.desc.height);
	fsr2_constants.downscaleFactor[0] = float(fsr2_constants.renderSize[0]) / float(fsr2_constants.displaySize[0]);
	fsr2_constants.downscaleFactor[1] = float(fsr2_constants.renderSize[1]) / float(fsr2_constants.displaySize[1]);
	static float preExposure = 0;
	fsr2_constants.preExposure = (preExposure != 0) ? preExposure : 1.0f;

	// motion vector data
	const bool enable_display_resolution_motion_vectors = false;
	const int32_t* motionVectorsTargetSize = enable_display_resolution_motion_vectors ? fsr2_constants.displaySize : fsr2_constants.renderSize;

	fsr2_constants.motionVectorScale[0] = 1;
	fsr2_constants.motionVectorScale[1] = 1;

	// Jitter cancellation is removed from here because it is baked into the velocity buffer already:

	//// compute jitter cancellation
	//const bool jitterCancellation = true;
	//if (jitterCancellation)
	//{
	//	//fsr2_constants.motionVectorJitterCancellation[0] = (res.jitterPrev.x - fsr2_constants.jitterOffset[0]) / motionVectorsTargetSize[0];
	//	//fsr2_constants.motionVectorJitterCancellation[1] = (res.jitterPrev.y - fsr2_constants.jitterOffset[1]) / motionVectorsTargetSize[1];
	//	fsr2_constants.motionVectorJitterCancellation[0] = res.jitterPrev.x - fsr2_constants.jitterOffset[0];
	//	fsr2_constants.motionVectorJitterCancellation[1] = res.jitterPrev.y - fsr2_constants.jitterOffset[1];

	//	res.jitterPrev.x = fsr2_constants.jitterOffset[0];
	//	res.jitterPrev.y = fsr2_constants.jitterOffset[1];
	//}


	// lock data, assuming jitter sequence length computation for now
	const int32_t jitterPhaseCount = ffxFsr2GetJitterPhaseCount(fsr2_constants.renderSize[0], fsr2_constants.displaySize[0]);

	static const float lockInitialLifetime = 1.0f;
	fsr2_constants.lockInitialLifetime = lockInitialLifetime;

	// init on first frame
	const bool resetAccumulation = fsr2_constants.frameIndex == 0;
	if (resetAccumulation || fsr2_constants.jitterPhaseCount == 0)
	{
		fsr2_constants.jitterPhaseCount = (float)jitterPhaseCount;
	}
	else
	{
		const int32_t jitterPhaseCountDelta = (int32_t)(jitterPhaseCount - fsr2_constants.jitterPhaseCount);
		if (jitterPhaseCountDelta > 0) {
			fsr2_constants.jitterPhaseCount++;
		}
		else if (jitterPhaseCountDelta < 0) {
			fsr2_constants.jitterPhaseCount--;
		}
	}

	const int32_t maxLockFrames = (int32_t)(fsr2_constants.jitterPhaseCount) + 1;
	fsr2_constants.lockTickDelta = lockInitialLifetime / maxLockFrames;

	// convert delta time to seconds and clamp to [0, 1].
	//context->constants.deltaTime = std::max(0.0f, std::min(1.0f, params->frameTimeDelta / 1000.0f));
	fsr2_constants.deltaTime = saturate(dt);

	fsr2_constants.frameIndex++;

	// shading change usage of the SPD mip levels.
	fsr2_constants.lumaMipLevelToUse = uint32_t(FFX_FSR2_SHADING_CHANGE_MIP_LEVEL);

	const float mipDiv = float(2 << fsr2_constants.lumaMipLevelToUse);
	fsr2_constants.lumaMipDimensions[0] = uint32_t(fsr2_constants.renderSize[0] / mipDiv);
	fsr2_constants.lumaMipDimensions[1] = uint32_t(fsr2_constants.renderSize[1] / mipDiv);
	fsr2_constants.lumaMipRcp = float(fsr2_constants.lumaMipDimensions[0] * fsr2_constants.lumaMipDimensions[1]) /
		float(fsr2_constants.renderSize[0] * fsr2_constants.renderSize[1]);

	// reactive mask bias
	const int32_t threadGroupWorkRegionDim = 8;
	const int32_t dispatchSrcX = (fsr2_constants.renderSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int32_t dispatchSrcY = (fsr2_constants.renderSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int32_t dispatchDstX = (fsr2_constants.displaySize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int32_t dispatchDstY = (fsr2_constants.displaySize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	// Auto exposure
	uint32_t dispatchThreadGroupCountXY[2];
	uint32_t workGroupOffset[2];
	uint32_t numWorkGroupsAndMips[2];
	uint32_t rectInfo[4] = { 0, 0, (uint32_t)fsr2_constants.renderSize[0], (uint32_t)fsr2_constants.renderSize[1] };
	SpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);

	// downsample
	Fsr2SpdConstants luminancePyramidConstants;
	luminancePyramidConstants.numworkGroups = numWorkGroupsAndMips[0];
	luminancePyramidConstants.mips = numWorkGroupsAndMips[1];
	luminancePyramidConstants.workGroupOffset[0] = workGroupOffset[0];
	luminancePyramidConstants.workGroupOffset[1] = workGroupOffset[1];
	luminancePyramidConstants.renderSize[0] = fsr2_constants.renderSize[0];
	luminancePyramidConstants.renderSize[1] = fsr2_constants.renderSize[1];

	// compute the constants.
	Fsr2RcasConstants rcasConsts = {};
	const float sharpenessRemapped = (-2.0f * sharpness) + 2.0f;
	FsrRcasCon(rcasConsts.rcasConfig, sharpenessRemapped);

	const int r_idx = fsr2_constants.frameIndex % 2;
	const int rw_idx = (fsr2_constants.frameIndex + 1) % 2;

	const Texture& r_lock = res.lock_status[r_idx];
	const Texture& rw_lock = res.lock_status[rw_idx];
	const Texture& r_output = res.output_internal[r_idx];
	const Texture& rw_output = res.output_internal[rw_idx];

	if (resetAccumulation)
	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.adjusted_color, res.adjusted_color.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.luminance_current, res.luminance_current.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.luminance_history, res.luminance_history.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.exposure, res.exposure.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.previous_depth, res.previous_depth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.dilated_depth, res.dilated_depth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.dilated_motion, res.dilated_motion.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.dilated_reactive, res.dilated_reactive.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.disocclusion_mask, res.disocclusion_mask.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.reactive_mask, res.reactive_mask.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.output_internal[0], res.output_internal[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.output_internal[1], res.output_internal[1].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.lock_status[0], res.lock_status[0].desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.lock_status[1], res.lock_status[1].desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);

		device->ClearUAV(&res.adjusted_color, 0, cmd);
		device->ClearUAV(&res.luminance_current, 0, cmd);
		device->ClearUAV(&res.luminance_history, 0, cmd);
		device->ClearUAV(&res.exposure, 0, cmd);
		device->ClearUAV(&res.previous_depth, 0, cmd);
		device->ClearUAV(&res.dilated_depth, 0, cmd);
		device->ClearUAV(&res.dilated_motion, 0, cmd);
		device->ClearUAV(&res.dilated_reactive, 0, cmd);
		device->ClearUAV(&res.disocclusion_mask, 0, cmd);
		device->ClearUAV(&res.reactive_mask, 0, cmd);
		device->ClearUAV(&res.output_internal[0], 0, cmd);
		device->ClearUAV(&res.output_internal[1], 0, cmd);
		device->ClearUAV(&res.spd_global_atomic, 0, cmd); // this is always in UAV state

		float clearValuesLockStatus[4]{};
		clearValuesLockStatus[LOCK_LIFETIME_REMAINING] = lockInitialLifetime * 2.0f;
		clearValuesLockStatus[LOCK_TEMPORAL_LUMA] = 0.0f;
		clearValuesLockStatus[LOCK_TRUST] = 1.0f;
		uint32_t clear_lock_pk = wi::math::Pack_R11G11B10_FLOAT(XMFLOAT3(clearValuesLockStatus[0], clearValuesLockStatus[1], clearValuesLockStatus[2]));
		device->ClearUAV(&res.lock_status[0], clear_lock_pk, cmd);
		device->ClearUAV(&res.lock_status[1], clear_lock_pk, cmd);

		for (int i = 0; i < arraysize(barriers); ++i)
		{
			std::swap(barriers[i].image.layout_before, barriers[i].image.layout_after);
		}
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&res.reactive_mask, res.reactive_mask.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.luminance_current, res.luminance_current.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.exposure, res.exposure.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Autogen reactive mask", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_AUTOGEN_REACTIVE_PASS], cmd);

		device->BindResource(&input_pre_alpha, 0, cmd);
		device->BindResource(&input_post_alpha, 1, cmd);
		device->BindUAV(&res.reactive_mask, 0, cmd);

		struct Fsr2GenerateReactiveConstants
		{
			float       scale;
			float       threshold;
			float       binaryValue;
			uint32_t    flags;
		};
		Fsr2GenerateReactiveConstants constants = {};
		static float scale = 0.5f;
		static float threshold = 0.2f;
		static float binaryValue = 0.9f;
		constants.scale = scale;
		constants.threshold = threshold;
		constants.binaryValue = binaryValue;
		constants.flags = 0;
		constants.flags |= FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_TONEMAP;
		//constants.flags |= FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_INVERSETONEMAP;
		//constants.flags |= FFX_FSR2_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
		//constants.flags |= FFX_FSR2_AUTOREACTIVEFLAGS_APPLY_THRESHOLD;
		device->BindDynamicConstantBuffer(constants, 0, cmd);

		const int32_t threadGroupWorkRegionDim = 8;
		const int32_t dispatchSrcX = (fsr2_constants.renderSize[0] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
		const int32_t dispatchSrcY = (fsr2_constants.renderSize[1] + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

		device->Dispatch(dispatchSrcX, dispatchSrcY, 1, cmd);
	}
	device->EventEnd(cmd);

	device->BindDynamicConstantBuffer(fsr2_constants, 0, cmd);
	device->BindSampler(&samplers[SAMPLER_POINT_CLAMP], 0, cmd);
	device->BindSampler(&samplers[SAMPLER_LINEAR_CLAMP], 1, cmd);

	device->EventBegin("Luminance pyramid", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_COMPUTE_LUMINANCE_PYRAMID_PASS], cmd);

		device->BindResource(&input_post_alpha, 0, cmd);
		device->BindUAV(&res.spd_global_atomic, 0, cmd);
		device->BindUAV(&res.luminance_current, 1, cmd, fsr2_constants.lumaMipLevelToUse);
		device->BindUAV(&res.luminance_current, 2, cmd, 5);
		device->BindUAV(&res.exposure, 3, cmd);

		device->BindDynamicConstantBuffer(luminancePyramidConstants, 1, cmd);

		device->Dispatch(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], 1, cmd);
	}
	device->EventEnd(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.exposure, ResourceState::UNORDERED_ACCESS, res.exposure.desc.layout),
			GPUBarrier::Image(&res.luminance_current, ResourceState::UNORDERED_ACCESS, res.luminance_current.desc.layout),

			GPUBarrier::Image(&res.previous_depth, res.previous_depth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.adjusted_color, res.adjusted_color.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.luminance_history, res.luminance_history.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Adjust input color", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_PREPARE_INPUT_COLOR_PASS], cmd);

		device->BindResource(&input_post_alpha, 0, cmd);
		device->BindResource(&res.exposure, 1, cmd);
		device->BindUAV(&res.previous_depth, 0, cmd);
		device->BindUAV(&res.adjusted_color, 1, cmd);
		device->BindUAV(&res.luminance_history, 2, cmd);

		device->Dispatch(dispatchSrcX, dispatchSrcY, 1, cmd);
	}
	device->EventEnd(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.reactive_mask, ResourceState::UNORDERED_ACCESS, res.reactive_mask.desc.layout),
			GPUBarrier::Image(&res.adjusted_color, ResourceState::UNORDERED_ACCESS, res.adjusted_color.desc.layout),
			GPUBarrier::Image(&res.luminance_history, ResourceState::UNORDERED_ACCESS, res.luminance_history.desc.layout),

			GPUBarrier::Image(&res.dilated_depth, res.dilated_depth.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.dilated_motion, res.dilated_motion.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.dilated_reactive, res.dilated_reactive.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Reconstruct and dilate", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_RECONSTRUCT_PREVIOUS_DEPTH_PASS], cmd);

		device->BindResource(&input_velocity, 0, cmd);
		device->BindResource(&input_depth, 1, cmd);
		device->BindResource(&res.reactive_mask, 2, cmd);
		device->BindResource(&input_post_alpha, 3, cmd);
		device->BindResource(&res.adjusted_color, 4, cmd);
		device->BindUAV(&res.previous_depth, 0, cmd);
		device->BindUAV(&res.dilated_motion, 1, cmd);
		device->BindUAV(&res.dilated_depth, 2, cmd);
		device->BindUAV(&res.dilated_reactive, 3, cmd);

		device->Dispatch(dispatchSrcX, dispatchSrcY, 1, cmd);
	}
	device->EventEnd(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&res.previous_depth, ResourceState::UNORDERED_ACCESS, res.previous_depth.desc.layout),
			GPUBarrier::Image(&res.dilated_depth, ResourceState::UNORDERED_ACCESS, res.dilated_depth.desc.layout),
			GPUBarrier::Image(&res.dilated_motion, ResourceState::UNORDERED_ACCESS, res.dilated_motion.desc.layout),
			GPUBarrier::Image(&res.dilated_reactive, ResourceState::UNORDERED_ACCESS, res.dilated_reactive.desc.layout),

			GPUBarrier::Image(&rw_lock, rw_lock.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&res.disocclusion_mask, res.disocclusion_mask.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Depth clip", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_DEPTH_CLIP_PASS], cmd);

		device->BindResource(&res.previous_depth, 0, cmd);
		device->BindResource(&res.dilated_motion, 1, cmd);
		device->BindResource(&res.dilated_depth, 2, cmd);
		device->BindUAV(&res.disocclusion_mask, 0, cmd);

		device->Dispatch(dispatchSrcX, dispatchSrcY, 1, cmd);
	}
	device->EventEnd(cmd);

	device->EventBegin("Create locks", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_LOCK_PASS], cmd);

		device->BindResource(&res.reactive_mask, 0, cmd);
		device->BindResource(&r_lock, 1, cmd);
		device->BindResource(&res.adjusted_color, 2, cmd);
		device->BindUAV(&rw_lock, 0, cmd);

		device->Dispatch(dispatchSrcX, dispatchSrcY, 1, cmd);
	}
	device->EventEnd(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(&rw_lock),
			GPUBarrier::Image(&res.disocclusion_mask, ResourceState::UNORDERED_ACCESS, res.disocclusion_mask.desc.layout),
			GPUBarrier::Image(&rw_output, rw_output.desc.layout, ResourceState::UNORDERED_ACCESS),
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventBegin("Reproject and accumulate", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_ACCUMULATE_PASS], cmd);

		device->BindResource(&res.exposure, 0, cmd);
		device->BindResource(&res.dilated_motion, 2, cmd);
		device->BindResource(&r_output, 3, cmd);
		device->BindResource(&r_lock, 4, cmd);
		device->BindResource(&res.disocclusion_mask, 5, cmd);
		device->BindResource(&res.adjusted_color, 6, cmd);
		device->BindResource(&res.luminance_history, 7, cmd);
		device->BindResource(&res.lanczos_lut, 8, cmd);
		device->BindResource(&res.maximum_bias_lut, 9, cmd);
		device->BindResource(&res.dilated_reactive, 10, cmd);
		device->BindResource(&res.luminance_current, 11, cmd);
		device->BindUAV(&rw_output, 0, cmd);
		device->BindUAV(&rw_lock, 1, cmd);
#if !FFX_FSR2_OPTION_APPLY_SHARPENING
		device->BindUAV(&output, 2, cmd);
#endif // !FFX_FSR2_OPTION_APPLY_SHARPENING

		device->Dispatch(dispatchDstX, dispatchDstY, 1, cmd);
	}
	device->EventEnd(cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&rw_lock, ResourceState::UNORDERED_ACCESS, rw_lock.desc.layout),
			GPUBarrier::Image(&rw_output, ResourceState::UNORDERED_ACCESS, rw_output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

#if FFX_FSR2_OPTION_APPLY_SHARPENING
	device->EventBegin("Sharpen (RCAS)", cmd);
	{
		device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_FSR2_RCAS_PASS], cmd);

		device->BindResource(&res.exposure, 0, cmd);
		device->BindResource(&rw_output, 1, cmd);
		device->BindUAV(&output, 0, cmd);

		device->BindDynamicConstantBuffer(rcasConsts, 1, cmd);

		const int32_t threadGroupWorkRegionDimRCAS = 16;
		const int32_t dispatchX = (fsr2_constants.displaySize[0] + (threadGroupWorkRegionDimRCAS - 1)) / threadGroupWorkRegionDimRCAS;
		const int32_t dispatchY = (fsr2_constants.displaySize[1] + (threadGroupWorkRegionDimRCAS - 1)) / threadGroupWorkRegionDimRCAS;

		device->Dispatch(dispatchX, dispatchY, 1, cmd);
	}
	device->EventEnd(cmd);
#endif // FFX_FSR2_OPTION_APPLY_SHARPENING

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Chromatic_Aberration(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	float amount
)
{
	device->EventBegin("Postprocess_Chromatic_Aberration", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_CHROMATIC_ABERRATION], cmd);

	device->BindResource(&input, 0, cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = amount;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	device->EventEnd(cmd);
}
void Postprocess_Upsample_Bilateral(
	const Texture& input,
	const Texture& lineardepth,
	const Texture& output,
	CommandList cmd,
	bool pixelshader,
	float threshold
)
{
	device->EventBegin("Postprocess_Upsample_Bilateral", cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = threshold;
	postprocess.params0.w = float(input.desc.width) / float(output.desc.width);
	postprocess.params1.x = (float)input.desc.width;
	postprocess.params1.y = (float)input.desc.height;
	postprocess.params1.z = 1.0f / postprocess.params1.x;
	postprocess.params1.w = 1.0f / postprocess.params1.y;

	const int mip = (int)std::floor(std::max(1.0f, log2f(std::max((float)desc.width / (float)input.GetDesc().width, (float)desc.height / (float)input.GetDesc().height))));

	device->BindResource(&input, 0, cmd);
	device->BindResource(&lineardepth, 1, cmd);
	device->BindResource(&lineardepth, 2, cmd, std::min((int)lineardepth.desc.mip_levels - 1, mip));

	if (pixelshader)
	{
		device->BindPipelineState(&PSO_upsample_bilateral, cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->Draw(3, 0, cmd);
	}
	else
	{
		SHADERTYPE cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4;
		switch (desc.format)
		{
		case Format::R16_UNORM:
		case Format::R8_UNORM:
		case Format::R16_FLOAT:
		case Format::R32_FLOAT:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT1;
			break;
		case Format::R16G16B16A16_UNORM:
		case Format::R8G8B8A8_UNORM:
		case Format::B8G8R8A8_UNORM:
		case Format::R10G10B10A2_UNORM:
		case Format::R11G11B10_FLOAT:
		case Format::R16G16B16A16_FLOAT:
		case Format::R32G32B32A32_FLOAT:
			cs = CSTYPE_POSTPROCESS_UPSAMPLE_BILATERAL_FLOAT4;
			break;
		default:
			assert(0); // implement format!
			break;
		}
		device->BindComputeShader(&shaders[cs], cmd);
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		const GPUResource* uavs[] = {
			&output,
		};
		device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

		device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

		device->ClearUAV(&output, 0, cmd);
		device->Barrier(GPUBarrier::Memory(&output), cmd);

		device->Dispatch(
			(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
			1,
			cmd
		);

		device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	}

	device->EventEnd(cmd);
}
void Postprocess_Downsample4x(
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	bool hdrToSRGB
)
{
	device->EventBegin("Postprocess_Downsample4x", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_DOWNSAMPLE4X], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = hdrToSRGB ? 1.0f : 0.0f;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	device->EventEnd(cmd);
}
void Postprocess_Lineardepth(
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_Lineardepth", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_LINEARDEPTH], cmd);

	const TextureDesc& desc = output.GetDesc();

	device->BindResource(&input, 0, cmd);

	device->BindUAV(&output, 0, cmd, 0);
	device->BindUAV(&output, 1, cmd, 1);
	device->BindUAV(&output, 2, cmd, 2);
	device->BindUAV(&output, 3, cmd, 3);
	device->BindUAV(&output, 4, cmd, 4);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}
void Postprocess_NormalsFromDepth(
	const Texture& depthbuffer,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_NormalsFromDepth", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_NORMALSFROMDEPTH], cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0.x = std::floor(std::max(1.0f, log2f(std::max((float)desc.width / (float)depthbuffer.GetDesc().width, (float)desc.height / (float)depthbuffer.GetDesc().height))));
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&depthbuffer, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Memory(),
			GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->EventEnd(cmd);
}
void Postprocess_Underwater(
	const Texture& input,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("Postprocess_Underwater", cmd);
	auto range = wi::profiler::BeginRangeGPU("Underwater", cmd);

	device->BindComputeShader(&shaders[CSTYPE_POSTPROCESS_UNDERWATER], cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}
void Postprocess_Custom(
	const Shader& computeshader,
	const Texture& input,
	const Texture& output,
	CommandList cmd,
	const XMFLOAT4& params0,
	const XMFLOAT4& params1,
	const char* debug_name
)
{
	device->EventBegin(debug_name, cmd);
	auto range = wi::profiler::BeginRangeGPU(debug_name, cmd);

	device->BindComputeShader(&computeshader, cmd);

	BindCommonResources(cmd);

	const TextureDesc& desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = desc.width;
	postprocess.resolution.y = desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	postprocess.params0 = params0;
	postprocess.params1 = params1;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->ClearUAV(&output, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->Dispatch(
		(desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	wi::profiler::EndRange(range);
	device->EventEnd(cmd);
}


void YUV_to_RGB(
	const Texture& input,
	int input_subresource_luminance,
	int input_subresource_chrominance,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("YUV_to_RGB", cmd);

	device->BindComputeShader(&shaders[input.desc.array_size > 1 ? CSTYPE_YUV_TO_RGB_ARRAY : CSTYPE_YUV_TO_RGB], cmd);

	const TextureDesc& input_desc = input.GetDesc();
	const TextureDesc& output_desc = output.GetDesc();

	PostProcess postprocess;
	postprocess.resolution.x = input_desc.width;  // taking input desc resolution to avoid sampling padding texels
	postprocess.resolution.y = input_desc.height; // taking input desc resolution to avoid sampling padding texels
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->BindResource(&input, 0, cmd, input_subresource_luminance);
	device->BindResource(&input, 1, cmd, input_subresource_chrominance);

	const GPUResource* uavs[] = {
		&output,
	};
	device->BindUAVs(uavs, 0, arraysize(uavs), cmd);

	device->Barrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::UNORDERED_ACCESS), cmd);

	device->Dispatch(
		(output_desc.width + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		(output_desc.height + POSTPROCESS_BLOCKSIZE - 1) / POSTPROCESS_BLOCKSIZE,
		1,
		cmd
	);

	device->Barrier(GPUBarrier::Image(&output, ResourceState::UNORDERED_ACCESS, output.desc.layout), cmd);

	device->EventEnd(cmd);
}

void CopyDepthStencil(
	const Texture* input_depth,
	const Texture* input_stencil,
	const Texture& output_depth_stencil,
	CommandList cmd,
	uint8_t stencil_bits_to_copy,
	bool depthstencil_already_cleared
)
{
	device->EventBegin("CopyDepthStencil", cmd);

	bool manual_depthstencil_copy_required =
		(stencil_bits_to_copy != 0xFF) ||
		device->CheckCapability(GraphicsDeviceCapability::COPY_BETWEEN_DIFFERENT_IMAGE_ASPECTS_NOT_SUPPORTED)
	;

	if (manual_depthstencil_copy_required)
	{
		// Vulkan workaround:

		RenderPassImage rp[] = {
			RenderPassImage::DepthStencil(
				&output_depth_stencil,
				depthstencil_already_cleared ? RenderPassImage::LoadOp::LOAD : RenderPassImage::LoadOp::CLEAR
			),
		};
		device->RenderPassBegin(rp, arraysize(rp), cmd);

		Viewport vp;
		vp.width = (float)output_depth_stencil.desc.width;
		vp.height = (float)output_depth_stencil.desc.height;
		device->BindViewports(1, &vp, cmd);

		Rect rect;
		rect.left = 0;
		rect.right = output_depth_stencil.desc.width;
		rect.top = 0;
		rect.bottom = output_depth_stencil.desc.height;
		device->BindScissorRects(1, &rect, cmd);

		if (input_depth != nullptr)
		{
			device->EventBegin("CopyDepth", cmd);
			device->BindPipelineState(&PSO_copyDepth, cmd);
			device->BindResource(input_depth, 0, cmd);
			device->Draw(3, 0, cmd);
			device->EventEnd(cmd);
		}

		if (input_stencil != nullptr)
		{
			device->EventBegin("CopyStencilBits", cmd);
			device->BindResource(input_stencil, 0, cmd);

			StencilBitPush push = {};
			push.output_resolution_rcp.x = 1.0f / vp.width;
			push.output_resolution_rcp.y = 1.0f / vp.height;
			push.input_resolution = (input_stencil->desc.width & 0xFFFF) | (input_stencil->desc.height << 16u);

			uint32_t bit_index = 0;
			while (stencil_bits_to_copy != 0)
			{
				if (stencil_bits_to_copy & 0x1)
				{
					if (input_stencil->desc.sample_count > 1)
					{
						device->BindPipelineState(&PSO_copyStencilBit_MSAA[bit_index], cmd);
					}
					else
					{
						device->BindPipelineState(&PSO_copyStencilBit[bit_index], cmd);
					}
					push.bit = 1u << bit_index;
					device->PushConstants(&push, sizeof(push), cmd);
					device->BindStencilRef(push.bit, cmd);
					device->Draw(3, 0, cmd);
				}
				bit_index++;
				stencil_bits_to_copy >>= 1;
			}
			device->EventEnd(cmd);
		}

		device->RenderPassEnd(cmd);

	}
	else
	{
		// Normal copy from color to depth/stencil aspects:
		if (input_depth != nullptr)
		{
			PushBarrier(GPUBarrier::Image(input_depth, input_depth->desc.layout, ResourceState::COPY_SRC));
		}
		if (input_stencil != nullptr)
		{
			PushBarrier(GPUBarrier::Image(input_stencil, input_stencil->desc.layout, ResourceState::COPY_SRC));
		}
		PushBarrier(GPUBarrier::Image(&output_depth_stencil, output_depth_stencil.desc.layout, ResourceState::COPY_DST));
		FlushBarriers(cmd);

		if (input_depth != nullptr)
		{
			device->CopyTexture(
				&output_depth_stencil, 0, 0, 0, 0, 0,
				input_depth, 0, 0,
				cmd,
				nullptr,
				ImageAspect::DEPTH,
				ImageAspect::COLOR
			);
		}
		if (input_stencil != nullptr)
		{
			device->CopyTexture(
				&output_depth_stencil, 0, 0, 0, 0, 0,
				input_stencil, 0, 0,
				cmd,
				nullptr,
				ImageAspect::STENCIL,
				ImageAspect::COLOR
			);
		}

		if (input_depth != nullptr)
		{
			PushBarrier(GPUBarrier::Image(input_depth, ResourceState::COPY_SRC, input_depth->desc.layout));
		}
		if (input_stencil != nullptr)
		{
			PushBarrier(GPUBarrier::Image(input_stencil, ResourceState::COPY_SRC, input_stencil->desc.layout));
		}
		PushBarrier(GPUBarrier::Image(&output_depth_stencil, ResourceState::COPY_DST, output_depth_stencil.desc.layout));
		FlushBarriers(cmd);
	}

	device->EventEnd(cmd);
}

void ScaleStencilMask(
	const Viewport& vp,
	const Texture& input,
	CommandList cmd
)
{
	device->EventBegin("ScaleStencilMask", cmd);

	device->BindResource(&input, 0, cmd);

	RenderPassInfo info = device->GetRenderPassInfo(cmd);
	assert(IsFormatStencilSupport(info.ds_format)); // the current render pass must have stencil

	StencilBitPush push = {};
	push.output_resolution_rcp.x = 1.0f / vp.width;
	push.output_resolution_rcp.y = 1.0f / vp.height;
	push.input_resolution = (input.desc.width & 0xFFFF) | (input.desc.height << 16u);

	uint8_t stencil_bits_to_copy = 0xFF;
	uint32_t bit_index = 0;
	while (stencil_bits_to_copy != 0)
	{
		if (stencil_bits_to_copy & 0x1)
		{
			if (input.desc.sample_count > 1)
			{
				device->BindPipelineState(&PSO_copyStencilBit_MSAA[bit_index], cmd);
			}
			else
			{
				device->BindPipelineState(&PSO_copyStencilBit[bit_index], cmd);
			}
			push.bit = 1u << bit_index;
			device->PushConstants(&push, sizeof(push), cmd);
			device->BindStencilRef(push.bit, cmd);
			device->Draw(3, 0, cmd);
		}
		bit_index++;
		stencil_bits_to_copy >>= 1;
	}

	device->EventEnd(cmd);
}

void ExtractStencil(
	const Texture& input_depthstencil,
	const Texture& output,
	CommandList cmd
)
{
	device->EventBegin("ExtractStencil", cmd);

	if (device->CheckCapability(GraphicsDeviceCapability::COPY_BETWEEN_DIFFERENT_IMAGE_ASPECTS_NOT_SUPPORTED))
	{
		// Vulkan workaround:
		device->EventBegin("ExtractStencilBits", cmd);

		RenderPassImage rp[] = {
			RenderPassImage::RenderTarget(&output,RenderPassImage::LoadOp::CLEAR),
			RenderPassImage::DepthStencil(&input_depthstencil),
		};
		device->RenderPassBegin(rp, arraysize(rp), cmd);

		Viewport vp;
		vp.width = (float)output.desc.width;
		vp.height = (float)output.desc.height;
		device->BindViewports(1, &vp, cmd);

		Rect rect;
		rect.left = 0;
		rect.right = output.desc.width;
		rect.top = 0;
		rect.bottom = output.desc.height;
		device->BindScissorRects(1, &rect, cmd);

		StencilBitPush push = {};
		push.output_resolution_rcp.x = 1.0f / vp.width;
		push.output_resolution_rcp.y = 1.0f / vp.height;
		push.input_resolution = (input_depthstencil.desc.width & 0xFFFF) | (input_depthstencil.desc.height << 16u);

		device->BindStencilRef(0xFFFFFFFF, cmd);

		uint8_t stencil_bits_to_extract = 0xFF;
		uint32_t bit_index = 0;
		while (stencil_bits_to_extract != 0)
		{
			if (stencil_bits_to_extract & 0x1)
			{
				device->BindPipelineState(&PSO_extractStencilBit[bit_index], cmd);
				push.bit = 1u << bit_index;
				device->PushConstants(&push, sizeof(push), cmd);
				device->Draw(3, 0, cmd);
			}
			bit_index++;
			stencil_bits_to_extract >>= 1;
		}

		device->RenderPassEnd(cmd);

		device->EventEnd(cmd);
	}
	else
	{
		// Normal copy from stencil aspect to color:
		PushBarrier(GPUBarrier::Image(&input_depthstencil, input_depthstencil.desc.layout, ResourceState::COPY_SRC));
		PushBarrier(GPUBarrier::Image(&output, output.desc.layout, ResourceState::COPY_DST));
		FlushBarriers(cmd);

		device->CopyTexture(
			&output, 0, 0, 0, 0, 0,
			&input_depthstencil, 0, 0,
			cmd,
			nullptr,
			ImageAspect::COLOR,
			ImageAspect::STENCIL
		);

		PushBarrier(GPUBarrier::Image(&input_depthstencil, ResourceState::COPY_SRC, input_depthstencil.desc.layout));
		PushBarrier(GPUBarrier::Image(&output, ResourceState::COPY_DST, output.desc.layout));
		FlushBarriers(cmd);
	}

	device->EventEnd(cmd);
}

void ComputeReprojectedDepthPyramid(
	const Texture& input_depth,
	const Texture& input_velocity,
	const Texture& output_depth_pyramid,
	CommandList cmd
)
{
	device->EventBegin("ComputeReprojectedDepthPyramid", cmd);

	BindCommonResources(cmd);

	GPUResource empty;
	const GPUResource* unbind[] = {
		&empty,
		&empty,
		&empty,
		&empty,
	};

	device->BindComputeShader(&shaders[CSTYPE_DEPTH_REPROJECT], cmd);

	const TextureDesc& input_desc = input_depth.GetDesc();
	const TextureDesc& output_desc = output_depth_pyramid.GetDesc();

	PostProcess postprocess = {};
	postprocess.resolution.x = output_desc.width;
	postprocess.resolution.y = output_desc.height;
	postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
	postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	{
		GPUBarrier barriers[] = {
			GPUBarrier::Image(&output_depth_pyramid, output_depth_pyramid.desc.layout, ResourceState::UNORDERED_ACCESS),
		};
		device->Barrier(barriers, arraysize(barriers), cmd);
	}

	device->ClearUAV(&output_depth_pyramid, 0, cmd);
	device->Barrier(GPUBarrier::Memory(&output_depth_pyramid), cmd);

	device->BindResource(&input_depth, 0, cmd);
	device->BindResource(&input_velocity, 1, cmd);

	device->BindUAVs(unbind, 0, arraysize(unbind), cmd);

	device->BindUAV(&output_depth_pyramid, 0, cmd, 0);
	PushBarrier(GPUBarrier::Image(&output_depth_pyramid, ResourceState::UNORDERED_ACCESS, output_depth_pyramid.desc.layout, 0));

	if (output_desc.mip_levels > 1)
	{
		device->BindUAV(&output_depth_pyramid, 1, cmd, 1);
		PushBarrier(GPUBarrier::Image(&output_depth_pyramid, ResourceState::UNORDERED_ACCESS, output_depth_pyramid.desc.layout, 1));
	}

	device->Dispatch(
		(postprocess.resolution.x + 7 - 1) / 8,
		(postprocess.resolution.y + 7 - 1) / 8,
		1,
		cmd
	);

	FlushBarriers(cmd);

	device->BindComputeShader(&shaders[CSTYPE_DEPTH_PYRAMID], cmd);

	uint bottom = 2;
	while (output_desc.mip_levels > bottom)
	{
		device->BindUAVs(unbind, 0, arraysize(unbind), cmd);
		postprocess.resolution.x = std::max(1u, output_desc.width >> bottom);
		postprocess.resolution.y = std::max(1u, output_desc.height >> bottom);
		postprocess.resolution_rcp.x = 1.0f / postprocess.resolution.x;
		postprocess.resolution_rcp.y = 1.0f / postprocess.resolution.y;
		device->PushConstants(&postprocess, sizeof(postprocess), cmd);

		device->BindResource(&output_depth_pyramid, 0, cmd, bottom - 1);

		device->BindUAV(&output_depth_pyramid, 0, cmd, bottom);
		PushBarrier(GPUBarrier::Image(&output_depth_pyramid, ResourceState::UNORDERED_ACCESS, output_depth_pyramid.desc.layout, bottom));

		if (output_desc.mip_levels > (bottom + 1))
		{
			device->BindUAV(&output_depth_pyramid, 1, cmd, bottom + 1);
			PushBarrier(GPUBarrier::Image(&output_depth_pyramid, ResourceState::UNORDERED_ACCESS, output_depth_pyramid.desc.layout, bottom + 1));
		}

		device->Dispatch(
			(postprocess.resolution.x + 7 - 1) / 8,
			(postprocess.resolution.y + 7 - 1) / 8,
			1,
			cmd
		);

		FlushBarriers(cmd);

		bottom += 2;
	}

	device->EventEnd(cmd);
}

void DrawWaveEffect(const XMFLOAT4& color, CommandList cmd)
{
	device->EventBegin("DrawWaveEffect", cmd);

	BindCommonResources(cmd);

	device->BindPipelineState(&PSO_waveeffect, cmd);

	PostProcess postprocess = {};
	postprocess.params0 = color;
	device->PushConstants(&postprocess, sizeof(postprocess), cmd);

	device->Draw(3, 0, cmd);

	device->EventEnd(cmd);
}


Ray GetPickRay(long cursorX, long cursorY, const wi::Canvas& canvas, const CameraComponent& camera)
{
	float screenW = canvas.GetLogicalWidth();
	float screenH = canvas.GetLogicalHeight();

	XMMATRIX V = camera.GetView();
	XMMATRIX P = camera.GetProjection();
	XMMATRIX W = XMMatrixIdentity();
	XMVECTOR lineStart = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 1, 1), 0, 0, screenW, screenH, 0.0f, 1.0f, P, V, W);
	XMVECTOR lineEnd = XMVector3Unproject(XMVectorSet((float)cursorX, (float)cursorY, 0, 1), 0, 0, screenW, screenH, 0.0f, 1.0f, P, V, W);
	XMVECTOR rayDirection = XMVector3Normalize(XMVectorSubtract(lineEnd, lineStart));
	return Ray(lineStart, rayDirection);
}

void DrawBox(const wi::primitive::AABB& aabb, const XMFLOAT4& color, bool depth)
{
	DrawBox(aabb.getAsBoxMatrix(), color, depth);
}
void DrawBox(const XMMATRIX& boxMatrix, const XMFLOAT4& color, bool depth)
{
	XMFLOAT4X4 m;
	XMStoreFloat4x4(&m, boxMatrix);
	DrawBox(m, color, depth);
}
void DrawBox(const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color, bool depth)
{
	if(depth)
		renderableBoxes_depth.push_back(std::make_pair(boxMatrix, color));
	else
		renderableBoxes.push_back(std::make_pair(boxMatrix,color));
}
void DrawSphere(const Sphere& sphere, const XMFLOAT4& color, bool depth)
{
	if(depth)
		renderableSpheres_depth.push_back(std::make_pair(sphere, color));
	else
		renderableSpheres.push_back(std::make_pair(sphere, color));
}
void DrawCapsule(const Capsule& capsule, const XMFLOAT4& color, bool depth)
{
	if(depth)
		renderableCapsules_depth.push_back(std::make_pair(capsule, color));
	else
		renderableCapsules.push_back(std::make_pair(capsule, color));
}
void DrawLine(const RenderableLine& line, bool depth)
{
	if(depth)
		renderableLines_depth.push_back(line);
	else
		renderableLines.push_back(line);
}
void DrawLine(const RenderableLine2D& line)
{
	renderableLines2D.push_back(line);
}
void DrawAxis(const XMMATRIX& matrix, float size, bool depth)
{
	RenderableLine line;

	// X
	line.start = XMFLOAT3(0, 0, 0);
	line.end = XMFLOAT3(size, 0, 0);
	line.color_start = line.color_end = XMFLOAT4(1, 0.2f, 0.2f, 1);
	XMStoreFloat3(&line.start, XMVector3Transform(XMLoadFloat3(&line.start), matrix));
	XMStoreFloat3(&line.end, XMVector3Transform(XMLoadFloat3(&line.end), matrix));
	DrawLine(line, depth);

	// Y
	line.start = XMFLOAT3(0, 0, 0);
	line.end = XMFLOAT3(0, size, 0);
	line.color_start = line.color_end = XMFLOAT4(0.2f, 1, 0.2f, 1);
	XMStoreFloat3(&line.start, XMVector3Transform(XMLoadFloat3(&line.start), matrix));
	XMStoreFloat3(&line.end, XMVector3Transform(XMLoadFloat3(&line.end), matrix));
	DrawLine(line, depth);

	// Z
	line.start = XMFLOAT3(0, 0, 0);
	line.end = XMFLOAT3(0, 0, size);
	line.color_start = line.color_end = XMFLOAT4(0.2f, 0.2f, 1, 1);
	XMStoreFloat3(&line.start, XMVector3Transform(XMLoadFloat3(&line.start), matrix));
	XMStoreFloat3(&line.end, XMVector3Transform(XMLoadFloat3(&line.end), matrix));
	DrawLine(line, depth);
}
void DrawPoint(const RenderablePoint& point, bool depth)
{
	if(depth)
		renderablePoints_depth.push_back(point);
	else
		renderablePoints.push_back(point);
}
void DrawTriangle(const RenderableTriangle& triangle, bool wireframe, bool depth)
{
	if(depth)
	{
		if (wireframe)
		{
			renderableTriangles_wireframe_depth.push_back(triangle);
		}
		else
		{
			renderableTriangles_solid_depth.push_back(triangle);
		}
	}
	else
	{
		if (wireframe)
		{
			renderableTriangles_wireframe.push_back(triangle);
		}
		else
		{
			renderableTriangles_solid.push_back(triangle);
		}
	}
}
void DrawDebugText(const char* text, const DebugTextParams& params)
{
	for (size_t i = 0; i < sizeof(DebugTextParams); ++i)
	{
		debugTextStorage.push_back(((uint8_t*)(&params))[i]);
	}
	size_t len = strlen(text) + 1;
	for (size_t i = 0; i < len; ++i)
	{
		debugTextStorage.push_back(uint8_t(text[i]));
	}
}
void DrawPaintRadius(const PaintRadius& paintrad)
{
	paintrads.push_back(paintrad);
}
void PaintIntoTexture(const PaintTextureParams& params)
{
	painttextures.push_back(params);
}
void PaintDecalIntoObjectSpaceTexture(const PaintDecalParams& params)
{
	paintdecals.push_back(params);
}
void DrawVoxelGrid(const wi::VoxelGrid* voxelgrid)
{
	renderableVoxelgrids.push_back(voxelgrid);
}
void DrawPathQuery(const wi::PathQuery* pathquery)
{
	renderablePathqueries.push_back(pathquery);
}
void DrawTrail(const wi::TrailRenderer* trail)
{
	renderableTrails.push_back(trail);
}

void AddDeferredMIPGen(const Texture& texture, bool preserve_coverage)
{
	deferredMIPGenLock.lock();
	deferredMIPGens.push_back(std::make_pair(texture, preserve_coverage));
	deferredMIPGenLock.unlock();
}
void AddDeferredBlockCompression(const wi::graphics::Texture& texture_src, const wi::graphics::Texture& texture_bc)
{
	deferredMIPGenLock.lock();
	deferredBCQueue.push_back(std::make_pair(texture_src, texture_bc));
	deferredMIPGenLock.unlock();
}



void SetWireRender(bool value) { wireRender = value; }
bool IsWireRender() { return wireRender; }
void SetToDrawDebugBoneLines(bool param) { debugBoneLines = param; }
bool GetToDrawDebugBoneLines() { return debugBoneLines; }
void SetToDrawDebugPartitionTree(bool param) { debugPartitionTree = param; }
bool GetToDrawDebugPartitionTree() { return debugPartitionTree; }
bool GetToDrawDebugEnvProbes() { return debugEnvProbes; }
void SetToDrawDebugEnvProbes(bool value) { debugEnvProbes = value; }
void SetToDrawDebugEmitters(bool param) { debugEmitters = param; }
bool GetToDrawDebugEmitters() { return debugEmitters; }
void SetToDrawDebugForceFields(bool param) { debugForceFields = param; }
bool GetToDrawDebugForceFields() { return debugForceFields; }
void SetToDrawDebugCameras(bool param) { debugCameras = param; }
bool GetToDrawDebugCameras() { return debugCameras; }
void SetToDrawDebugColliders(bool param) { debugColliders = param; }
bool GetToDrawDebugColliders() { return debugColliders; }
void SetToDrawDebugSprings(bool param) { debugSprings = param; }
bool GetToDrawDebugSprings() { return debugSprings; }
bool GetToDrawGridHelper() { return gridHelper; }
void SetToDrawGridHelper(bool value) { gridHelper = value; }
bool GetToDrawVoxelHelper() { return VXGI_DEBUG; }
void SetToDrawVoxelHelper(bool value, int clipmap_level) { VXGI_DEBUG = value; VXGI_DEBUG_CLIPMAP = clipmap_level; }
void SetDebugLightCulling(bool enabled) { debugLightCulling = enabled; }
bool GetDebugLightCulling() { return debugLightCulling; }
void SetAdvancedLightCulling(bool enabled) { advancedLightCulling = enabled; }
bool GetAdvancedLightCulling() { return advancedLightCulling; }
void SetVariableRateShadingClassification(bool enabled) { variableRateShadingClassification = enabled; }
bool GetVariableRateShadingClassification() { return variableRateShadingClassification; }
void SetVariableRateShadingClassificationDebug(bool enabled) { variableRateShadingClassificationDebug = enabled; }
bool GetVariableRateShadingClassificationDebug() { return variableRateShadingClassificationDebug; }
void SetOcclusionCullingEnabled(bool value)
{
	occlusionCulling = value;
}
bool GetOcclusionCullingEnabled() { return occlusionCulling; }
void SetTemporalAAEnabled(bool enabled) { temporalAA = enabled; }
bool GetTemporalAAEnabled() { return temporalAA; }
void SetTemporalAADebugEnabled(bool enabled) { temporalAADEBUG = enabled; }
bool GetTemporalAADebugEnabled() { return temporalAADEBUG; }
void SetFreezeCullingCameraEnabled(bool enabled) { freezeCullingCamera = enabled; }
bool GetFreezeCullingCameraEnabled() { return freezeCullingCamera; }
void SetVXGIEnabled(bool enabled)
{
	VXGI_ENABLED = enabled;
}
bool GetVXGIEnabled() { return VXGI_ENABLED; }
void SetVXGIReflectionsEnabled(bool enabled) { VXGI_REFLECTIONS_ENABLED = enabled; }
bool GetVXGIReflectionsEnabled() { return VXGI_REFLECTIONS_ENABLED; }
void SetGameSpeed(float value) { GameSpeed = std::max(0.0f, value); }
float GetGameSpeed() { return GameSpeed; }
void SetShadowsEnabled(bool value)
{
	SHADOWS_ENABLED = value;
}
bool IsShadowsEnabled()
{
	return SHADOWS_ENABLED;
}
void SetRaytraceBounceCount(uint32_t bounces)
{
	raytraceBounceCount = bounces;
}
uint32_t GetRaytraceBounceCount()
{
	return raytraceBounceCount;
}
void SetRaytraceDebugBVHVisualizerEnabled(bool value)
{
	raytraceDebugVisualizer = value;
}
bool GetRaytraceDebugBVHVisualizerEnabled()
{
	return raytraceDebugVisualizer;
}
void SetRaytracedShadowsEnabled(bool value)
{
	raytracedShadows = value;
}
bool GetRaytracedShadowsEnabled()
{
	return raytracedShadows;
}
void SetTessellationEnabled(bool value)
{
	tessellationEnabled = value;
}
bool GetTessellationEnabled()
{
	return tessellationEnabled;
}
void SetDisableAlbedoMaps(bool value)
{
	disableAlbedoMaps = value;
}
bool IsDisableAlbedoMaps()
{
	return disableAlbedoMaps;
}
void SetForceDiffuseLighting(bool value)
{
	forceDiffuseLighting = value;
}
bool IsForceDiffuseLighting()
{
	return forceDiffuseLighting;
}
void SetScreenSpaceShadowsEnabled(bool value)
{
	SCREENSPACESHADOWS = value;
}
bool GetScreenSpaceShadowsEnabled()
{
	return SCREENSPACESHADOWS;
}
void SetSurfelGIEnabled(bool value)
{
	SURFELGI = value;
}
bool GetSurfelGIEnabled()
{
	return SURFELGI;
}
void SetSurfelGIDebugEnabled(SURFEL_DEBUG value)
{
	SURFELGI_DEBUG = value;
}
SURFEL_DEBUG GetSurfelGIDebugEnabled()
{
	return SURFELGI_DEBUG;
}
void SetDDGIEnabled(bool value)
{
	DDGI_ENABLED = value;
}
bool GetDDGIEnabled()
{
	return DDGI_ENABLED;
}
void SetDDGIDebugEnabled(bool value)
{
	DDGI_DEBUG_ENABLED = value;
}
bool GetDDGIDebugEnabled()
{
	return DDGI_DEBUG_ENABLED;
}
void SetDDGIRayCount(uint32_t value)
{
	DDGI_RAYCOUNT = value;
}
uint32_t GetDDGIRayCount()
{
	return DDGI_RAYCOUNT;
}
void SetDDGIBlendSpeed(float value)
{
	DDGI_BLEND_SPEED = value;
}
float GetDDGIBlendSpeed()
{
	return DDGI_BLEND_SPEED;
}
void SetGIBoost(float value)
{
	GI_BOOST = value;
}
float GetGIBoost()
{
	return GI_BOOST;
}
void SetMeshShaderAllowed(bool value)
{
	MESH_SHADER_ALLOWED = value;
}
bool IsMeshShaderAllowed()
{
	return MESH_SHADER_ALLOWED && device->CheckCapability(GraphicsDeviceCapability::MESH_SHADER);
}
void SetMeshletOcclusionCullingEnabled(bool value)
{
	MESHLET_OCCLUSION_CULLING = value;
}
bool IsMeshletOcclusionCullingEnabled()
{
	return MESHLET_OCCLUSION_CULLING;
}
void SetCapsuleShadowEnabled(bool value)
{
	CAPSULE_SHADOW_ENABLED = value;
}
bool IsCapsuleShadowEnabled()
{
	return CAPSULE_SHADOW_ENABLED;
}
void SetCapsuleShadowAngle(float value)
{
	CAPSULE_SHADOW_ANGLE = value;
}
float GetCapsuleShadowAngle()
{
	return CAPSULE_SHADOW_ANGLE;
}
void SetCapsuleShadowFade(float value)
{
	CAPSULE_SHADOW_FADE = value;
}
float GetCapsuleShadowFade()
{
	return CAPSULE_SHADOW_FADE;
}
void SetShadowLODOverrideEnabled(bool value)
{
	SHADOW_LOD_OVERRIDE = value;
}
bool IsShadowLODOverrideEnabled()
{
	return SHADOW_LOD_OVERRIDE;
}

wi::Resource CreatePaintableTexture(uint32_t width, uint32_t height, uint32_t mips, wi::Color initialColor)
{
	TextureDesc desc;
	desc.width = width;
	desc.height = height;
	desc.mip_levels = mips;
	if (desc.mip_levels == 0)
	{
		desc.mip_levels = GetMipCount(desc.width, desc.height);
	}
	desc.format = Format::R8G8B8A8_UNORM;
	desc.bind_flags = BindFlag::SHADER_RESOURCE | BindFlag::UNORDERED_ACCESS | BindFlag::RENDER_TARGET;
	desc.misc_flags = ResourceMiscFlag::TYPED_FORMAT_CASTING;
	wi::vector<wi::Color> data(desc.width * desc.height);
	std::fill(data.begin(), data.end(), initialColor);
	SubresourceData initdata[32] = {};
	for (uint32_t mip = 0; mip < desc.mip_levels; ++mip)
	{
		initdata[mip].data_ptr = data.data();
		initdata[mip].row_pitch = std::max(1u, desc.width >> mip) * GetFormatStride(desc.format);
	}
	Texture texture;
	GraphicsDevice* device = GetDevice();
	device->CreateTexture(&desc, initdata, &texture);

	static std::atomic<uint32_t> cnt = 0;
	device->SetName(&texture, std::string("PaintableTexture" + std::to_string(cnt.fetch_add(1u))).c_str());

	for (uint32_t i = 0; i < texture.GetDesc().mip_levels; ++i)
	{
		int subresource_index;
		subresource_index = device->CreateSubresource(&texture, SubresourceType::SRV, 0, 1, i, 1);
		assert(subresource_index == i);
		subresource_index = device->CreateSubresource(&texture, SubresourceType::UAV, 0, 1, i, 1);
		assert(subresource_index == i);
	}

	// This part must be AFTER mip level subresource creation:
	int srgb_subresource = -1;
	{
		Format srgb_format = GetFormatSRGB(desc.format);
		srgb_subresource = device->CreateSubresource(
			&texture,
			SubresourceType::SRV,
			0, -1,
			0, -1,
			&srgb_format
		);
	}

	wi::Resource resource;
	resource.SetTexture(texture, srgb_subresource);

	return resource;
}

}
