#pragma once

#include "FSR41Api.h"

#include <array>
#include <cstdint>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace FidelityFX
{
   // Provides the explicit D3D11 resource and synchronization bridge needed by
   // a D3D12-only FSR provider. It uses shared textures and shared fences, so
   // the game keeps its native D3D11 device and immediate context.
   class D3D11On12Bridge
   {
   public:
      D3D11On12Bridge() = default;
      ~D3D11On12Bridge();

      D3D11On12Bridge(const D3D11On12Bridge&) = delete;
      D3D11On12Bridge& operator=(const D3D11On12Bridge&) = delete;

      bool Initialize(ID3D11Device* native_device, IDXGIAdapter* native_adapter);
      void Shutdown();

      bool IsInitialized() const { return m_device12.Get() != nullptr && m_queue.Get() != nullptr; }
      bool BeginFrame(
         ID3D11DeviceContext* native_context,
         ID3D11Resource* source_color,
         ID3D11Resource* depth_buffer,
         ID3D11Resource* motion_vectors,
         ID3D11Resource* exposure,
         ID3D11Resource* output_color);
      bool TransitionForDispatch();
      bool FinishFrame();
      void AbortFrame();
      bool WaitIdle();

      ID3D12Device* GetD3D12Device() const { return m_device12.Get(); }
      ID3D12GraphicsCommandList* GetCommandList() const { return m_frame_slots[m_frame_slot_index].command_list.Get(); }
      const FSR41Api::Resource& GetColorResource() const { return m_color.api; }
      const FSR41Api::Resource& GetDepthResource() const { return m_depth.api; }
      const FSR41Api::Resource& GetMotionVectorResource() const { return m_motion_vectors.api; }
      const FSR41Api::Resource& GetExposureResource() const { return m_exposure.api; }
      const FSR41Api::Resource& GetOutputResource() const { return m_output.api; }
      bool HasExposureResource() const { return m_exposure.valid; }

      const char* GetLastError() const { return m_last_error; }

   private:
      struct ResourceSlot
      {
         Microsoft::WRL::ComPtr<ID3D11Texture2D> source_texture;
         Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_texture;
         Microsoft::WRL::ComPtr<ID3D12Resource> d3d12_resource;
         D3D11_TEXTURE2D_DESC desc = {};
         FSR41Api::Resource api = {};
         ID3D11Resource* source_identity = nullptr;
         bool copy_required = false;
         bool valid = false;

         void Reset()
         {
            source_texture.Reset();
            shared_texture.Reset();
            d3d12_resource.Reset();
            desc = {};
            api = {};
            source_identity = nullptr;
            copy_required = false;
            valid = false;
         }
      };

      struct FrameSlot
      {
         Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
         Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
         std::uint64_t fence_value = 0;
      };

      enum class ResourceKind
      {
         Color,
         Depth,
         MotionVectors,
         Exposure,
         Output,
      };

      bool CreateD3D12Objects(IDXGIAdapter* native_adapter);
      bool CreateCrossApiFence();
      bool PrepareInput(ResourceSlot& slot, ID3D11Resource* resource, ResourceKind kind);
      bool PrepareOutput(ID3D11Resource* resource);
      bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& source_desc, ResourceSlot& slot, bool output);
      bool OpenD3D12Resource(ResourceSlot& slot, HANDLE shared_handle, bool close_handle);
      bool TryOpenNativeSharedResource(ResourceSlot& slot);
      bool ResetFrameSlot();
      bool SignalD3D11ToD3D12();
      bool SignalD3D12ToD3D11();
      bool WaitForFence(ID3D12Fence* fence, std::uint64_t value);
      bool WaitForD3D11Fence(std::uint64_t value);
      void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
      void SetError(const char* message);
      void ResetResources();

      static std::uint32_t GetApiFormat(DXGI_FORMAT format);
      static std::uint32_t GetApiUsage(DXGI_FORMAT format, ResourceKind kind, bool output);
      static bool AreDescriptorsEqual(const D3D11_TEXTURE2D_DESC& left, const D3D11_TEXTURE2D_DESC& right);

      Microsoft::WRL::ComPtr<ID3D11Device> m_native_device;
      Microsoft::WRL::ComPtr<ID3D11Device5> m_native_device5;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_native_context;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_native_context4;
      Microsoft::WRL::ComPtr<IDXGIAdapter> m_native_adapter;

      Microsoft::WRL::ComPtr<ID3D12Device> m_device12;
      Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
      Microsoft::WRL::ComPtr<ID3D12Fence> m_frame_fence;
      Microsoft::WRL::ComPtr<ID3D11Fence> m_cross_fence11;
      Microsoft::WRL::ComPtr<ID3D12Fence> m_cross_fence12;
      HANDLE m_wait_event = nullptr;

      std::array<FrameSlot, 2> m_frame_slots = {};
      std::uint32_t m_frame_slot_index = 0;
      std::uint64_t m_next_frame_fence = 1;
      std::uint64_t m_next_cross_fence = 1;
      bool m_frame_active = false;

      ResourceSlot m_color;
      ResourceSlot m_depth;
      ResourceSlot m_motion_vectors;
      ResourceSlot m_exposure;
      ResourceSlot m_output;
      Microsoft::WRL::ComPtr<ID3D11Resource> m_output_target;

      const char* m_last_error = "not initialized";
   };
}
