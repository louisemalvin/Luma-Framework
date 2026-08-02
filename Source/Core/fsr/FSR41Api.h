#pragma once

// This is the small ABI surface used by the FSR 4.1 provider. It intentionally
// avoids linking the provider or copying its implementation into the addon.
// The layout mirrors the public FidelityFX API headers.

#include <cstdint>

namespace FidelityFX::FSR41Api
{
   using StructType = std::uint64_t;
   using ReturnCode = std::uint32_t;
   using Context = void*;

   constexpr ReturnCode ReturnOk = 0;
   constexpr ReturnCode ReturnError = 1;
   constexpr ReturnCode ReturnErrorUnknownDescType = 2;
   constexpr ReturnCode ReturnErrorRuntime = 3;
   constexpr ReturnCode ReturnNoProvider = 4;
   constexpr ReturnCode ReturnErrorMemory = 5;
   constexpr ReturnCode ReturnErrorParameter = 6;
   constexpr ReturnCode ReturnProviderNoSupportNewDescType = 7;

   constexpr StructType EffectIdUpscale = 0x00010000u;
   constexpr StructType BackendIdD3D12 = 0x00000000u;
   constexpr std::uint32_t UpscalerVersion = (4u << 22) | (1u << 12) | 1u;

   constexpr StructType MakeEffectSubId(StructType effect_id, StructType subversion)
   {
      return (effect_id & 0x00ff0000u) | (subversion & ~0x00ff0000u);
   }

   constexpr StructType MakeBackendSubId(StructType backend_id, StructType subversion)
   {
      return (backend_id & 0xff000000u) | (subversion & ~0xff000000u);
   }

   constexpr StructType CreateContextDescTypeUpscale = MakeEffectSubId(EffectIdUpscale, 0x00);
   constexpr StructType DispatchDescTypeUpscale = MakeEffectSubId(EffectIdUpscale, 0x01);
   constexpr StructType CreateContextDescTypeUpscaleVersion = MakeEffectSubId(EffectIdUpscale, 0x0b);
   constexpr StructType CreateContextDescTypeBackendD3D12 = MakeBackendSubId(BackendIdD3D12, 0x02);

   enum CreateContextUpscaleFlags : std::uint32_t
   {
      EnableHighDynamicRange = 1u << 0,
      EnableDisplayResolutionMotionVectors = 1u << 1,
      EnableMotionVectorsJitterCancellation = 1u << 2,
      EnableDepthInverted = 1u << 3,
      EnableDepthInfinite = 1u << 4,
      EnableAutoExposure = 1u << 5,
      EnableDynamicResolution = 1u << 6,
      EnableDebugChecking = 1u << 7,
      EnableNonLinearColorspace = 1u << 8,
      EnableDebugVisualization = 1u << 9,
   };

   enum DispatchUpscaleFlags : std::uint32_t
   {
      DrawDebugView = 1u << 0,
      NonLinearColorSrgb = 1u << 1,
      NonLinearColorPq = 1u << 2,
   };

   enum ResourceFormat : std::uint32_t
   {
      FormatUnknown = 0,
      FormatR32G32B32A32Typeless = 1,
      FormatR32G32B32A32Uint = 2,
      FormatR32G32B32A32Float = 3,
      FormatR16G16B16A16Float = 4,
      FormatR32G32B32Float = 5,
      FormatR32G32Float = 6,
      FormatR8Uint = 7,
      FormatR32Uint = 8,
      FormatR8G8B8A8Typeless = 9,
      FormatR8G8B8A8Unorm = 10,
      FormatR8G8B8A8Snorm = 11,
      FormatR8G8B8A8Srgb = 12,
      FormatB8G8R8A8Typeless = 13,
      FormatB8G8R8A8Unorm = 14,
      FormatB8G8R8A8Srgb = 15,
      FormatR11G11B10Float = 16,
      FormatR10G10B10A2Unorm = 17,
      FormatR16G16Float = 18,
      FormatR16G16Uint = 19,
      FormatR16G16Sint = 20,
      FormatR16Float = 21,
      FormatR16Uint = 22,
      FormatR16Unorm = 23,
      FormatR16Snorm = 24,
      FormatR8Unorm = 25,
      FormatR8G8Unorm = 26,
      FormatR8G8Uint = 27,
      FormatR32Float = 28,
      FormatR9G9B9E5SharedExp = 29,
      FormatR16G16B16A16Typeless = 30,
      FormatR32G32Typeless = 31,
      FormatR10G10B10A2Typeless = 32,
      FormatR16G16Typeless = 33,
      FormatR16Typeless = 34,
      FormatR8Typeless = 35,
      FormatR8G8Typeless = 36,
      FormatR32Typeless = 37,
      FormatR32G32Uint = 38,
      FormatR8Snorm = 39,
   };

   enum ResourceUsage : std::uint32_t
   {
      ResourceUsageReadOnly = 0,
      ResourceUsageRenderTarget = 1u << 0,
      ResourceUsageUav = 1u << 1,
      ResourceUsageDepthTarget = 1u << 2,
      ResourceUsageArrayView = 1u << 4,
      ResourceUsageStencilTarget = 1u << 5,
   };

   enum ResourceState : std::uint32_t
   {
      ResourceStateCommon = 1u << 0,
      ResourceStateUnorderedAccess = 1u << 1,
      ResourceStateComputeRead = 1u << 2,
      ResourceStatePixelRead = 1u << 3,
   };

   enum ResourceType : std::uint32_t
   {
      ResourceTypeBuffer = 0,
      ResourceTypeTexture1D = 1,
      ResourceTypeTexture2D = 2,
      ResourceTypeTextureCube = 3,
      ResourceTypeTexture3D = 4,
   };

   struct Dimensions2D
   {
      std::uint32_t width;
      std::uint32_t height;
   };

   struct FloatCoords2D
   {
      float x;
      float y;
   };

   struct ApiHeader
   {
      StructType type;
      ApiHeader* pNext;
   };

   using ApiMessage = void (*)(std::uint32_t type, const wchar_t* message);

   struct ResourceDescription
   {
      std::uint32_t type;
      std::uint32_t format;
      union
      {
         std::uint32_t width;
         std::uint32_t size;
      };
      union
      {
         std::uint32_t height;
         std::uint32_t stride;
      };
      union
      {
         std::uint32_t depth;
         std::uint32_t alignment;
      };
      std::uint32_t mip_count;
      std::uint32_t flags;
      std::uint32_t usage;
   };

   struct Resource
   {
      void* resource;
      ResourceDescription description;
      std::uint32_t state;
   };

   struct CreateContextDescUpscale
   {
      ApiHeader header;
      std::uint32_t flags;
      Dimensions2D max_render_size;
      Dimensions2D max_upscale_size;
      ApiMessage fp_message;
   };

   struct CreateContextDescUpscaleVersion
   {
      ApiHeader header;
      std::uint32_t version;
   };

   struct CreateBackendD3D12Desc
   {
      ApiHeader header;
      void* device;
   };

   struct DispatchDescUpscale
   {
      ApiHeader header;
      void* command_list;
      Resource color;
      Resource depth;
      Resource motion_vectors;
      Resource exposure;
      Resource reactive;
      Resource transparency_and_composition;
      Resource output;
      FloatCoords2D jitter_offset;
      FloatCoords2D motion_vector_scale;
      Dimensions2D render_size;
      Dimensions2D upscale_size;
      bool enable_sharpening;
      float sharpness;
      float frame_time_delta;
      float pre_exposure;
      bool reset;
      float camera_near;
      float camera_far;
      float camera_fov_angle_vertical;
      float view_space_to_meters_factor;
      std::uint32_t flags;
   };

   static_assert(sizeof(ApiHeader) == 16);
   static_assert(sizeof(ResourceDescription) == 32);
   static_assert(sizeof(Resource) == 48);
   static_assert(sizeof(CreateContextDescUpscale) == 48);
   static_assert(sizeof(CreateContextDescUpscaleVersion) == 24);
#if defined(_WIN64)
   static_assert(sizeof(DispatchDescUpscale) == 432);
#endif

   using CreateContextFn = ReturnCode (*)(Context* context, ApiHeader* descriptor, const void* allocation_callbacks);
   using DestroyContextFn = ReturnCode (*)(Context* context, const void* allocation_callbacks);
   using ConfigureFn = ReturnCode (*)(Context* context, const ApiHeader* descriptor);
   using QueryFn = ReturnCode (*)(Context* context, ApiHeader* descriptor);
   using DispatchFn = ReturnCode (*)(Context* context, const ApiHeader* descriptor);
}
