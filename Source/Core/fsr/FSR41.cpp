#include "FSR41.h"

#if defined(_WIN64) && defined(ENABLE_FSR41) && ENABLE_FSR41 && defined(ENABLE_FIDELITY_SK) && ENABLE_FIDELITY_SK

#include "D3D11On12Bridge.h"

#include <Windows.h>
#include <include/reshade.hpp>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace FidelityFX
{
   using namespace FSR41Api;

   namespace
   {
      void LogFsr41(const char* format, ...)
      {
         char message[1024] = {};
         va_list args;
         va_start(args, format);
         vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
         va_end(args);

         OutputDebugStringA(message);
         OutputDebugStringA("\n");
         printf_s("%s\n", message);
         reshade::log::message(reshade::log::level::warning, message);
      }

      void FSR41ApiMessage(std::uint32_t type, const wchar_t* wide_message)
      {
         char message[768] = {};
         if (wide_message)
         {
            WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<LPCWSTR>(wide_message), -1, message, static_cast<int>(sizeof(message)), nullptr, nullptr);
         }

         LogFsr41("FSR4.1 provider %s: %s", type == 0 ? "error" : "warning", message);
      }

      class Provider
      {
      public:
         ~Provider()
         {
            Unload();
         }

         bool Load()
         {
            Unload();

            std::vector<std::wstring> candidates;
            wchar_t environment_buffer[2048] = {};
            const DWORD environment_length = GetEnvironmentVariableW(reinterpret_cast<LPCWSTR>(L"LUMA_FSR41_PROVIDER"), reinterpret_cast<LPWSTR>(environment_buffer), static_cast<DWORD>(std::size(environment_buffer)));
            if (environment_length != 0 && environment_length < std::size(environment_buffer))
            {
               candidates.emplace_back(std::wstring(environment_buffer, environment_length));
            }
            else
            {
               const std::array<const wchar_t*, 3> names = {
                  L"amd_fidelityfx_loader_dx12.dll",
                  // GBFR's current distribution ships the same loader under
                  // this shorter filename; its PE internal name is the one
                  // above.
                  L"amd_fidelityfx_dx12.dll",
                  L"amd_fidelityfx_upscaler_dx12.dll",
               };

               wchar_t module_path_buffer[4096] = {};
               const DWORD module_path_length = GetModuleFileNameW(nullptr, reinterpret_cast<LPWSTR>(module_path_buffer), static_cast<DWORD>(std::size(module_path_buffer)));
               if (module_path_length != 0 && module_path_length < std::size(module_path_buffer))
               {
                  std::wstring module_directory(module_path_buffer, module_path_length);
                  const std::size_t separator = module_directory.find_last_of(L"\\/");
                  if (separator == std::wstring::npos)
                  {
                     module_directory.clear();
                  }
                  else
                  {
                     module_directory.resize(separator);
                  }

                  auto add_module_candidate = [&candidates](const std::wstring& directory, const wchar_t* subdirectory, const wchar_t* name)
                  {
                     std::wstring candidate = directory;
                     if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/')
                     {
                        candidate += L'\\';
                     }
                     if (subdirectory && subdirectory[0] != L'\0')
                     {
                        candidate += subdirectory;
                        candidate += L'\\';
                     }
                     candidate += name;
                     candidates.emplace_back(std::move(candidate));
                  };

                  for (const wchar_t* name : names)
                  {
                     add_module_candidate(module_directory, nullptr, name);
                     add_module_candidate(module_directory, L"Luma", name);
                  }
               }
               for (const wchar_t* name : names)
               {
                  candidates.emplace_back(name);
               }
            }

            for (const auto& candidate : candidates)
            {
               HMODULE module = nullptr;
               if (candidate.find_first_of(L"\\/") != std::wstring::npos)
               {
                  module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(candidate.c_str()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
               }
               else
               {
                  module = LoadLibraryW(reinterpret_cast<LPCWSTR>(candidate.c_str()));
               }
               if (!module)
               {
                  continue;
               }

               auto get_proc = [module](const char* name) -> FARPROC
               {
                  return GetProcAddress(module, name);
               };
               create_context = reinterpret_cast<CreateContextFn>(get_proc("ffxCreateContext"));
               destroy_context = reinterpret_cast<DestroyContextFn>(get_proc("ffxDestroyContext"));
               configure = reinterpret_cast<ConfigureFn>(get_proc("ffxConfigure"));
               query = reinterpret_cast<QueryFn>(get_proc("ffxQuery"));
               dispatch = reinterpret_cast<DispatchFn>(get_proc("ffxDispatch"));
               if (!create_context || !destroy_context || !configure || !query || !dispatch)
               {
                  FreeLibrary(module);
                  create_context = nullptr;
                  destroy_context = nullptr;
                  configure = nullptr;
                  query = nullptr;
                  dispatch = nullptr;
                  continue;
               }

               module_handle = module;
               loaded_path = candidate;
               return true;
            }

            last_error = "signed FSR4.1 provider DLL was not found or has an incompatible API";
            return false;
         }

         void Unload()
         {
            if (module_handle)
            {
               FreeLibrary(module_handle);
               module_handle = nullptr;
            }
            create_context = nullptr;
            destroy_context = nullptr;
            configure = nullptr;
            query = nullptr;
            dispatch = nullptr;
            loaded_path.clear();
         }

         HMODULE module_handle = nullptr;
         CreateContextFn create_context = nullptr;
         DestroyContextFn destroy_context = nullptr;
         ConfigureFn configure = nullptr;
         QueryFn query = nullptr;
         DispatchFn dispatch = nullptr;
         std::wstring loaded_path;
         const char* last_error = "provider is not loaded";
      };

      struct FSR41InstanceData final : SR::InstanceData
      {
         Provider provider;
         D3D11On12Bridge bridge;
         Microsoft::WRL::ComPtr<ID3D11Device> native_device;
         Microsoft::WRL::ComPtr<IDXGIAdapter> native_adapter;
         FSR fallback;
         SR::InstanceData* fallback_data = nullptr;

         Context context = nullptr;
         bool has_context = false;
         bool fallback_active = false;
         bool fallback_logged = false;
         // GBFR's patched camera table and frame hook use the framework's
         // eight-phase default. Keep the FSR4.1 period aligned with FSR3.
         int phase_count = SR::GetDefaultJitterPhases();

         CreateContextDescUpscale context_desc = {};
         CreateContextDescUpscaleVersion version_desc = {};
         CreateBackendD3D12Desc backend_desc = {};
      };

      FSR41InstanceData* GetData(SR::InstanceData* data)
      {
         return reinterpret_cast<FSR41InstanceData*>(data);
      }

      const FSR41InstanceData* GetData(const SR::InstanceData* data)
      {
         return reinterpret_cast<const FSR41InstanceData*>(data);
      }

      void DestroyContext(FSR41InstanceData& data)
      {
         if (!data.has_context || !data.provider.destroy_context)
         {
            return;
         }

         if (!data.bridge.WaitIdle())
         {
            LogFsr41("FSR4.1 bridge idle wait failed before context destruction: %s", data.bridge.GetLastError());
         }
         const ReturnCode result = data.provider.destroy_context(&data.context, nullptr);
         if (result != ReturnOk)
         {
            LogFsr41("FSR4.1 context destruction failed, error = %u", result);
         }
         data.context = nullptr;
         data.has_context = false;
      }

      bool ActivateFallback(FSR41InstanceData& data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data)
      {
         DestroyContext(data);
         data.fallback_active = true;
         data.provider.Unload();
         data.bridge.Shutdown();

         if (!data.fallback_data || !data.fallback_data->is_supported)
         {
            if (!data.fallback_logged)
            {
               LogFsr41("FSR4.1 disabled and FSR3 fallback is unavailable");
               data.fallback_logged = true;
            }
            return false;
         }

         if (!data.fallback_logged)
         {
            LogFsr41("FSR4.1 failed; using the existing FSR3 backend for this device");
            data.fallback_logged = true;
         }
         return data.fallback.UpdateSettings(data.fallback_data, command_list, settings_data);
      }

      bool DispatchFallback(const FSR41InstanceData& data, ID3D11DeviceContext* command_list, const SR::SuperResolutionImpl::DrawData& draw_data)
      {
         if (!data.fallback_data || !data.fallback_data->is_supported)
         {
            return false;
         }
         return const_cast<FSR&>(data.fallback).Draw(data.fallback_data, command_list, draw_data);
      }
   }

   bool FSR41::Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter)
   {
      if (data)
      {
         Deinit(data);
      }

      auto*& custom_data = reinterpret_cast<FSR41InstanceData*&>(data);
      custom_data = new FSR41InstanceData();
      custom_data->supports_sdr = true;
      custom_data->supports_scrgb_hdr = true;
      custom_data->supports_dynamic_resolution = false;
      custom_data->supports_arbitrary_jitter_phases = true;
      custom_data->automatically_restores_pipeline_state = false;
      custom_data->min_resolution = 32;

      if (!device)
      {
         LogFsr41("FSR4.1 initialization skipped: native D3D11 device is null");
         return false;
      }

      custom_data->native_device = device;
      custom_data->native_adapter = adapter;

      if (!custom_data->provider.Load())
      {
         LogFsr41("FSR4.1 initialization skipped: %s", custom_data->provider.last_error);
         return false;
      }

      custom_data->fallback.Init(custom_data->fallback_data, device, adapter);
      custom_data->is_supported = true;
      return true;
   }

   void FSR41::Deinit(SR::InstanceData*& data, ID3D11Device* optional_device)
   {
      auto*& custom_data = reinterpret_cast<FSR41InstanceData*&>(data);
      if (!custom_data)
      {
         return;
      }

      DestroyContext(*custom_data);
      if (custom_data->fallback_data)
      {
         custom_data->fallback.Deinit(custom_data->fallback_data, optional_device);
      }
      custom_data->provider.Unload();
      custom_data->bridge.Shutdown();

      delete custom_data;
      custom_data = nullptr;
   }

   bool FSR41::HasInit(const SR::InstanceData* data) const
   {
      return data != nullptr;
   }

   bool FSR41::IsSupported(const SR::InstanceData* data) const
   {
      return data != nullptr && data->is_supported;
   }

   bool FSR41::UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data)
   {
      auto* custom_data = GetData(data);
      if (!custom_data || !custom_data->is_supported || !command_list)
      {
         return false;
      }

      if (custom_data->fallback_active)
      {
         custom_data->settings_data = settings_data;
         return custom_data->fallback.UpdateSettings(custom_data->fallback_data, command_list, settings_data);
      }

      if (custom_data->has_context && custom_data->settings_data == settings_data)
      {
         return true;
      }

      if (!custom_data->bridge.IsInitialized() &&
          !custom_data->bridge.Initialize(custom_data->native_device.Get(), custom_data->native_adapter.Get()))
      {
         LogFsr41("FSR4.1 bridge initialization failed: %s", custom_data->bridge.GetLastError());
         custom_data->settings_data = settings_data;
         return ActivateFallback(*custom_data, command_list, settings_data);
      }

      DestroyContext(*custom_data);

      custom_data->context_desc = {};
      custom_data->version_desc = {};
      custom_data->backend_desc = {};
      custom_data->context_desc.header.type = CreateContextDescTypeUpscale;
      custom_data->context_desc.header.pNext = &custom_data->version_desc.header;
      custom_data->context_desc.flags = 0;
      if (settings_data.hdr)
      {
         custom_data->context_desc.flags |= EnableHighDynamicRange;
      }
      else
      {
         custom_data->context_desc.flags |= EnableNonLinearColorspace;
      }
      if (settings_data.auto_exposure)
      {
         custom_data->context_desc.flags |= EnableAutoExposure;
      }
      if (settings_data.dynamic_resolution)
      {
         custom_data->context_desc.flags |= EnableDynamicResolution;
      }
      if (settings_data.inverted_depth)
      {
         custom_data->context_desc.flags |= EnableDepthInverted | EnableDepthInfinite;
      }
      if (settings_data.mvs_jittered)
      {
         custom_data->context_desc.flags |= EnableMotionVectorsJitterCancellation;
      }
#if DEVELOPMENT || TEST
      custom_data->context_desc.flags |= EnableDebugChecking;
      custom_data->context_desc.fp_message = FSR41ApiMessage;
#endif
      custom_data->context_desc.max_render_size = {
         settings_data.dynamic_resolution ? settings_data.output_width : settings_data.render_width,
         settings_data.dynamic_resolution ? settings_data.output_height : settings_data.render_height,
      };
      custom_data->context_desc.max_upscale_size = {
         settings_data.output_width,
         settings_data.output_height,
      };

      custom_data->version_desc.header.type = CreateContextDescTypeUpscaleVersion;
      custom_data->version_desc.header.pNext = &custom_data->backend_desc.header;
      custom_data->version_desc.version = UpscalerVersion;

      custom_data->backend_desc.header.type = CreateContextDescTypeBackendD3D12;
      custom_data->backend_desc.header.pNext = nullptr;
      custom_data->backend_desc.device = custom_data->bridge.GetD3D12Device();

      LogFsr41(
         "FSR4.1 context request: render=%ux%u output=%ux%u flags=0x%08x",
         custom_data->context_desc.max_render_size.width,
         custom_data->context_desc.max_render_size.height,
         custom_data->context_desc.max_upscale_size.width,
         custom_data->context_desc.max_upscale_size.height,
         custom_data->context_desc.flags);

      const ReturnCode result = custom_data->provider.create_context(
         &custom_data->context,
         &custom_data->context_desc.header,
         nullptr);
      if (result != ReturnOk || !custom_data->context)
      {
         LogFsr41("FSR4.1 context creation failed, error = %u", result);
         custom_data->context = nullptr;
         custom_data->has_context = false;
         custom_data->settings_data = settings_data;
         return ActivateFallback(*custom_data, command_list, settings_data);
      }

      custom_data->settings_data = settings_data;
      custom_data->has_context = true;
      custom_data->fallback_logged = false;
      return true;
   }

   int FSR41::GetJitterPhases(const SR::InstanceData* data) const
   {
      const auto* custom_data = GetData(data);
      if (!custom_data || custom_data->fallback_active)
      {
         return custom_data && custom_data->fallback_data ? const_cast<FSR&>(custom_data->fallback).GetJitterPhases(custom_data->fallback_data) : SR::GetDefaultJitterPhases();
      }
      return custom_data->phase_count;
   }

   SR::Type FSR41::GetFallbackType(const SR::InstanceData* data) const
   {
      const auto* custom_data = GetData(data);
      return custom_data && custom_data->fallback_active ? SR::Type::FSR : SR::Type::None;
   }

   bool FSR41::Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data)
   {
      const auto* custom_data = GetData(data);
      if (!custom_data || !custom_data->is_supported || !command_list)
      {
         return false;
      }

      if (custom_data->fallback_active)
      {
         return DispatchFallback(*custom_data, command_list, draw_data);
      }

      auto* mutable_data = const_cast<FSR41InstanceData*>(custom_data);
      if (!custom_data->has_context ||
          !mutable_data->bridge.BeginFrame(
             command_list,
             draw_data.source_color,
             draw_data.depth_buffer,
             draw_data.motion_vectors,
             draw_data.exposure,
             draw_data.output_color))
      {
         if (custom_data->has_context)
         {
            mutable_data->bridge.AbortFrame();
         }
         return ActivateFallback(*mutable_data, command_list, custom_data->settings_data) &&
                DispatchFallback(*mutable_data, command_list, draw_data);
      }

      if (!mutable_data->bridge.TransitionForDispatch())
      {
         mutable_data->bridge.AbortFrame();
         return ActivateFallback(*mutable_data, command_list, custom_data->settings_data) &&
                DispatchFallback(*mutable_data, command_list, draw_data);
      }

      DispatchDescUpscale dispatch = {};
      dispatch.header.type = DispatchDescTypeUpscale;
      dispatch.header.pNext = nullptr;
      dispatch.command_list = mutable_data->bridge.GetCommandList();
      dispatch.color = mutable_data->bridge.GetColorResource();
      dispatch.depth = mutable_data->bridge.GetDepthResource();
      dispatch.motion_vectors = mutable_data->bridge.GetMotionVectorResource();
      if (mutable_data->bridge.HasExposureResource())
      {
         dispatch.exposure = mutable_data->bridge.GetExposureResource();
      }
      dispatch.output = mutable_data->bridge.GetOutputResource();
      dispatch.jitter_offset = { draw_data.jitter_x, draw_data.jitter_y };
      dispatch.motion_vector_scale = { custom_data->settings_data.mvs_x_scale, custom_data->settings_data.mvs_y_scale };
      dispatch.render_size = { draw_data.render_width, draw_data.render_height };
      dispatch.upscale_size = { custom_data->settings_data.output_width, custom_data->settings_data.output_height };
      dispatch.enable_sharpening = draw_data.user_sharpness >= 0.f;
      dispatch.sharpness = dispatch.enable_sharpening ? draw_data.user_sharpness : 0.f;
      dispatch.frame_time_delta = (draw_data.time_delta > 0.f ? draw_data.time_delta : 1.f / 60.f) * 1000.f;
      dispatch.pre_exposure = draw_data.pre_exposure > 0.f ? draw_data.pre_exposure : 1.f;
      dispatch.reset = draw_data.reset;
      if (custom_data->settings_data.inverted_depth)
      {
         dispatch.camera_near = draw_data.far_plane;
         dispatch.camera_far = draw_data.near_plane;
      }
      else
      {
         dispatch.camera_near = draw_data.near_plane;
         dispatch.camera_far = draw_data.far_plane;
      }
      dispatch.camera_fov_angle_vertical = draw_data.vert_fov;
      dispatch.view_space_to_meters_factor = 1.f;
      dispatch.flags = custom_data->settings_data.hdr ? 0u : NonLinearColorSrgb;
#if DEVELOPMENT && !defined(NDEBUG)
      dispatch.flags |= DrawDebugView;
#endif

      const ReturnCode result = mutable_data->provider.dispatch(&mutable_data->context, &dispatch.header);
      if (result != ReturnOk)
      {
         LogFsr41("FSR4.1 dispatch failed, error = %u", result);
         mutable_data->bridge.AbortFrame();
         return ActivateFallback(*mutable_data, command_list, custom_data->settings_data) &&
                DispatchFallback(*mutable_data, command_list, draw_data);
      }

      if (!mutable_data->bridge.FinishFrame())
      {
         LogFsr41("FSR4.1 D3D11On12 frame completion failed: %s", mutable_data->bridge.GetLastError());
         if (!mutable_data->bridge.WaitIdle())
         {
            LogFsr41("FSR4.1 bridge idle wait after frame failure also failed: %s", mutable_data->bridge.GetLastError());
         }
         return ActivateFallback(*mutable_data, command_list, custom_data->settings_data) &&
                DispatchFallback(*mutable_data, command_list, draw_data);
      }

      return true;
   }

   const char* FSR41::GetBackendName(const SR::InstanceData* data) const
   {
      const auto* custom_data = GetData(data);
      return custom_data && custom_data->fallback_active ? "FSR3 fallback" : "FSR4.1";
   }
}

#endif
