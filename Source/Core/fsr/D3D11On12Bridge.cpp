#if defined(ENABLE_FSR41) && ENABLE_FSR41 && defined(ENABLE_FIDELITY_SK) && ENABLE_FIDELITY_SK

#include "D3D11On12Bridge.h"

namespace FidelityFX
{
   using Microsoft::WRL::ComPtr;
   using namespace FSR41Api;

   namespace
   {
      constexpr DWORD shared_resource_access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;

      bool IsDepthFormat(DXGI_FORMAT format)
      {
         switch (format)
         {
         case DXGI_FORMAT_D32_FLOAT:
         case DXGI_FORMAT_R32_TYPELESS:
         case DXGI_FORMAT_D24_UNORM_S8_UINT:
         case DXGI_FORMAT_R24G8_TYPELESS:
         case DXGI_FORMAT_R32G8X24_TYPELESS:
         case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
         default:
            return false;
         }
      }

      bool IsStencilFormat(DXGI_FORMAT format)
      {
         switch (format)
         {
         case DXGI_FORMAT_D24_UNORM_S8_UINT:
         case DXGI_FORMAT_R24G8_TYPELESS:
         case DXGI_FORMAT_R32G8X24_TYPELESS:
         case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
         default:
            return false;
         }
      }
   }

   D3D11On12Bridge::~D3D11On12Bridge()
   {
      Shutdown();
   }

   void D3D11On12Bridge::SetError(const char* message)
   {
      m_last_error = message ? message : "unknown bridge error";
   }

   bool D3D11On12Bridge::Initialize(ID3D11Device* native_device, IDXGIAdapter* native_adapter)
   {
      Shutdown();

      if (!native_device)
      {
         SetError("native D3D11 device is null");
         return false;
      }

      m_native_device = native_device;
      if (FAILED(native_device->QueryInterface(IID_PPV_ARGS(&m_native_device5))))
      {
         SetError("ID3D11Device5 is unavailable");
         Shutdown();
         return false;
      }

      native_device->GetImmediateContext(m_native_context.GetAddressOf());
      if (!m_native_context || FAILED(m_native_context->QueryInterface(IID_PPV_ARGS(&m_native_context4))))
      {
         SetError("ID3D11DeviceContext4 is unavailable");
         Shutdown();
         return false;
      }

      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> device_adapter;
      if (FAILED(native_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
          FAILED(dxgi_device->GetAdapter(device_adapter.GetAddressOf())) ||
          !device_adapter)
      {
         SetError("native DXGI adapter is unavailable");
         Shutdown();
         return false;
      }

      if (native_adapter)
      {
         DXGI_ADAPTER_DESC device_adapter_desc = {};
         DXGI_ADAPTER_DESC supplied_adapter_desc = {};
         if (FAILED(device_adapter->GetDesc(&device_adapter_desc)) ||
             FAILED(native_adapter->GetDesc(&supplied_adapter_desc)) ||
             device_adapter_desc.AdapterLuid.LowPart != supplied_adapter_desc.AdapterLuid.LowPart ||
             device_adapter_desc.AdapterLuid.HighPart != supplied_adapter_desc.AdapterLuid.HighPart)
         {
            SetError("native D3D11 adapter does not match the supplied adapter");
            Shutdown();
            return false;
         }
         m_native_adapter = native_adapter;
      }
      else
      {
         m_native_adapter = device_adapter;
      }

      if (!CreateD3D12Objects(m_native_adapter.Get()))
      {
         Shutdown();
         return false;
      }

      m_wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!m_wait_event)
      {
         SetError("D3D12 fence wait event could not be created");
         Shutdown();
         return false;
      }

      if (!CreateCrossApiFence())
      {
         Shutdown();
         return false;
      }

      m_last_error = "";
      return true;
   }

   void D3D11On12Bridge::Shutdown()
   {
      AbortFrame();
      WaitIdle();
      ResetResources();

      for (auto& frame_slot : m_frame_slots)
      {
         frame_slot.command_list.Reset();
         frame_slot.allocator.Reset();
         frame_slot.fence_value = 0;
      }

      if (m_wait_event)
      {
         CloseHandle(m_wait_event);
         m_wait_event = nullptr;
      }

      m_cross_fence12.Reset();
      m_cross_fence11.Reset();
      m_frame_fence.Reset();
      m_queue.Reset();
      m_device12.Reset();
      m_native_context4.Reset();
      m_native_context.Reset();
      m_native_adapter.Reset();
      m_native_device5.Reset();
      m_native_device.Reset();
      m_frame_slot_index = 0;
      m_next_frame_fence = 1;
      m_next_cross_fence = 1;
      m_frame_active = false;
   }

   bool D3D11On12Bridge::CreateD3D12Objects(IDXGIAdapter* native_adapter)
   {
      HRESULT hr = D3D12CreateDevice(native_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device12));
      if (FAILED(hr) || !m_device12)
      {
         SetError("D3D12CreateDevice failed");
         return false;
      }

      D3D12_FEATURE_DATA_SHADER_MODEL shader_model = { D3D_SHADER_MODEL_6_6 };
      if (FAILED(m_device12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) ||
          shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
      {
         SetError("D3D12 shader model 6.6 is unavailable");
         return false;
      }

      DXGI_ADAPTER_DESC native_adapter_desc = {};
      if (FAILED(native_adapter->GetDesc(&native_adapter_desc)))
      {
         SetError("native adapter description is unavailable");
         return false;
      }

      const LUID d3d12_luid = m_device12->GetAdapterLuid();
      if (native_adapter_desc.AdapterLuid.LowPart != d3d12_luid.LowPart ||
          native_adapter_desc.AdapterLuid.HighPart != d3d12_luid.HighPart)
      {
         SetError("D3D11 and D3D12 adapters do not match");
         return false;
      }

      D3D12_COMMAND_QUEUE_DESC queue_desc = {};
      queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
      queue_desc.NodeMask = 0;
      if (FAILED(m_device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_queue))))
      {
         SetError("D3D12 command queue creation failed");
         return false;
      }

      if (FAILED(m_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frame_fence))))
      {
         SetError("D3D12 frame fence creation failed");
         return false;
      }

      for (auto& frame_slot : m_frame_slots)
      {
         if (FAILED(m_device12->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&frame_slot.allocator))) ||
             FAILED(m_device12->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                frame_slot.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&frame_slot.command_list))) ||
             FAILED(frame_slot.command_list->Close()))
         {
            SetError("D3D12 command list creation failed");
            return false;
         }
      }

      return true;
   }

   bool D3D11On12Bridge::CreateCrossApiFence()
   {
      if (FAILED(m_native_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_cross_fence11))))
      {
         SetError("D3D11 shared fence creation failed");
         return false;
      }

      HANDLE shared_handle = nullptr;
      if (FAILED(m_cross_fence11->CreateSharedHandle(nullptr, shared_resource_access, nullptr, &shared_handle)) || !shared_handle)
      {
         SetError("D3D11 shared fence handle creation failed");
         return false;
      }

      const HRESULT hr = m_device12->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&m_cross_fence12));
      CloseHandle(shared_handle);
      if (FAILED(hr) || !m_cross_fence12)
      {
         SetError("D3D12 shared fence import failed");
         return false;
      }

      return true;
   }

   bool D3D11On12Bridge::WaitForFence(ID3D12Fence* fence, std::uint64_t value)
   {
      if (!fence || !m_wait_event)
      {
         SetError("D3D12 fence wait prerequisites are unavailable");
         return false;
      }

      const std::uint64_t completed_value = fence->GetCompletedValue();
      if (completed_value == ~std::uint64_t(0))
      {
         SetError("D3D12 fence reports a removed device");
         return false;
      }
      if (completed_value >= value)
      {
         return true;
      }

      if (FAILED(fence->SetEventOnCompletion(value, m_wait_event)))
      {
         SetError("D3D12 fence event registration failed");
         return false;
      }

      if (WaitForSingleObject(m_wait_event, INFINITE) != WAIT_OBJECT_0)
      {
         SetError("D3D12 fence wait failed");
         return false;
      }
      return true;
   }

   bool D3D11On12Bridge::WaitForD3D11Fence(std::uint64_t value)
   {
      if (!m_cross_fence11 || !m_wait_event)
      {
         SetError("D3D11 fence wait prerequisites are unavailable");
         return false;
      }

      const std::uint64_t completed_value = m_cross_fence11->GetCompletedValue();
      if (completed_value == ~std::uint64_t(0))
      {
         SetError("D3D11 fence reports a removed device");
         return false;
      }
      if (completed_value >= value)
      {
         return true;
      }

      if (FAILED(m_cross_fence11->SetEventOnCompletion(value, m_wait_event)))
      {
         SetError("D3D11 fence event registration failed");
         return false;
      }

      if (WaitForSingleObject(m_wait_event, INFINITE) != WAIT_OBJECT_0)
      {
         SetError("D3D11 fence wait failed");
         return false;
      }
      return true;
   }

   bool D3D11On12Bridge::WaitIdle()
   {
      if (!m_queue || !m_frame_fence)
      {
         return true;
      }

      if (!m_wait_event)
      {
         return true;
      }

      const std::uint64_t value = m_next_frame_fence++;
      if (FAILED(m_queue->Signal(m_frame_fence.Get(), value)))
      {
         SetError("D3D12 queue idle signal failed");
         return false;
      }

      if (!WaitForFence(m_frame_fence.Get(), value))
      {
         return false;
      }

      if (!m_native_context || !m_native_context4 || !m_cross_fence11)
      {
         return true;
      }

      m_native_context->Flush();
      const std::uint64_t d3d11_value = m_next_cross_fence++;
      if (FAILED(m_native_context4->Signal(m_cross_fence11.Get(), d3d11_value)))
      {
         SetError("D3D11 idle fence signal failed");
         return false;
      }
      m_native_context->Flush();
      return WaitForD3D11Fence(d3d11_value);
   }

   bool D3D11On12Bridge::ResetFrameSlot()
   {
      if (!m_queue || !m_device12)
      {
         SetError("D3D12 frame objects are unavailable");
         return false;
      }

      m_frame_slot_index = (m_frame_slot_index + 1) % static_cast<std::uint32_t>(m_frame_slots.size());
      FrameSlot& frame_slot = m_frame_slots[m_frame_slot_index];
      if (!frame_slot.allocator || !frame_slot.command_list)
      {
         SetError("D3D12 frame slot is unavailable");
         return false;
      }

      if (frame_slot.fence_value != 0 && !WaitForFence(m_frame_fence.Get(), frame_slot.fence_value))
      {
         return false;
      }

      if (FAILED(frame_slot.allocator->Reset()) || FAILED(frame_slot.command_list->Reset(frame_slot.allocator.Get(), nullptr)))
      {
         SetError("D3D12 command list reset failed");
         return false;
      }

      return true;
   }

   bool D3D11On12Bridge::BeginFrame(
      ID3D11DeviceContext* native_context,
      ID3D11Resource* source_color,
      ID3D11Resource* depth_buffer,
      ID3D11Resource* motion_vectors,
      ID3D11Resource* exposure,
      ID3D11Resource* output_color)
   {
      if (!IsInitialized() || !native_context || !source_color || !depth_buffer || !motion_vectors || !output_color)
      {
         SetError("D3D11On12 frame inputs are incomplete");
         return false;
      }

      if (native_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
      {
         SetError("D3D11On12 requires the immediate D3D11 context");
         return false;
      }

      ComPtr<ID3D11Device> context_device;
      native_context->GetDevice(context_device.GetAddressOf());
      if (context_device.Get() != m_native_device.Get())
      {
         SetError("D3D11 context belongs to a different device");
         return false;
      }

      AbortFrame();
      m_native_context = native_context;
      if (FAILED(native_context->QueryInterface(IID_PPV_ARGS(&m_native_context4))))
      {
         SetError("active D3D11 context has no fence synchronization interface");
         return false;
      }

      m_frame_active = true;
      if (!ResetFrameSlot() ||
          !PrepareInput(m_color, source_color, ResourceKind::Color) ||
          !PrepareInput(m_depth, depth_buffer, ResourceKind::Depth) ||
          !PrepareInput(m_motion_vectors, motion_vectors, ResourceKind::MotionVectors) ||
          !PrepareOutput(output_color))
      {
         AbortFrame();
         return false;
      }

      if (exposure)
      {
         D3D11_TEXTURE2D_DESC exposure_desc = {};
         ComPtr<ID3D11Texture2D> exposure_texture;
         if (SUCCEEDED(exposure->QueryInterface(IID_PPV_ARGS(&exposure_texture))) && exposure_texture)
         {
            exposure_texture->GetDesc(&exposure_desc);
         }

         if (exposure_desc.Width == 1 && exposure_desc.Height == 1 && exposure_desc.Format == DXGI_FORMAT_R32_FLOAT)
         {
            if (!PrepareInput(m_exposure, exposure, ResourceKind::Exposure))
            {
               AbortFrame();
               return false;
            }
         }
         else
         {
            if (m_exposure.valid && !WaitIdle())
            {
               AbortFrame();
               return false;
            }
            m_exposure.Reset();
         }
      }
      else
      {
         if (m_exposure.valid && !WaitIdle())
         {
            AbortFrame();
            return false;
         }
         m_exposure.Reset();
      }

      if (!SignalD3D11ToD3D12())
      {
         AbortFrame();
         return false;
      }

      return true;
   }

   bool D3D11On12Bridge::SignalD3D11ToD3D12()
   {
      if (!m_native_context4 || !m_cross_fence11 || !m_cross_fence12 || !m_queue)
      {
         SetError("D3D11 to D3D12 synchronization objects are unavailable");
         return false;
      }

      m_native_context->Flush();
      const std::uint64_t value = m_next_cross_fence++;
      if (FAILED(m_native_context4->Signal(m_cross_fence11.Get(), value)))
      {
         SetError("D3D11 to D3D12 fence signal failed");
         return false;
      }
      m_native_context->Flush();

      if (FAILED(m_queue->Wait(m_cross_fence12.Get(), value)))
      {
         SetError("D3D12 queue wait for D3D11 failed");
         return false;
      }

      return true;
   }

   bool D3D11On12Bridge::SignalD3D12ToD3D11()
   {
      if (!m_native_context4 || !m_cross_fence11 || !m_cross_fence12 || !m_queue)
      {
         SetError("D3D12 to D3D11 synchronization objects are unavailable");
         return false;
      }

      const std::uint64_t value = m_next_cross_fence++;
      if (FAILED(m_queue->Signal(m_cross_fence12.Get(), value)))
      {
         SetError("D3D12 to D3D11 fence signal failed");
         return false;
      }

      if (FAILED(m_native_context4->Wait(m_cross_fence11.Get(), value)))
      {
         SetError("D3D11 wait for D3D12 failed");
         return false;
      }

      return true;
   }

   bool D3D11On12Bridge::TransitionForDispatch()
   {
      if (!m_frame_active ||
          !m_color.valid ||
          !m_depth.valid ||
          !m_motion_vectors.valid ||
          !m_output.valid ||
          !GetCommandList())
      {
         SetError("D3D11On12 frame resources are incomplete");
         return false;
      }

      Transition(m_color.d3d12_resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Transition(m_depth.d3d12_resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Transition(m_motion_vectors.d3d12_resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      if (m_exposure.valid)
      {
         Transition(m_exposure.d3d12_resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      Transition(m_output.d3d12_resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      m_color.api.state = ResourceStateComputeRead;
      m_depth.api.state = ResourceStateComputeRead;
      m_motion_vectors.api.state = ResourceStateComputeRead;
      m_exposure.api.state = m_exposure.valid ? ResourceStateComputeRead : ResourceStateCommon;
      m_output.api.state = ResourceStateUnorderedAccess;
      return true;
   }

   void D3D11On12Bridge::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
   {
      if (!resource || before == after)
      {
         return;
      }

      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter = after;
      m_frame_slots[m_frame_slot_index].command_list->ResourceBarrier(1, &barrier);
   }

   bool D3D11On12Bridge::FinishFrame()
   {
      if (!m_frame_active ||
          !m_native_context4 ||
          !m_output.valid ||
          !m_output_target ||
          !m_output.shared_texture ||
          !GetCommandList())
      {
         SetError("D3D11On12 output frame is incomplete");
         AbortFrame();
         return false;
      }

      Transition(m_color.d3d12_resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
      Transition(m_depth.d3d12_resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
      Transition(m_motion_vectors.d3d12_resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
      if (m_exposure.valid)
      {
         Transition(m_exposure.d3d12_resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
      }
      Transition(m_output.d3d12_resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

      FrameSlot& frame_slot = m_frame_slots[m_frame_slot_index];
      if (FAILED(frame_slot.command_list->Close()))
      {
         SetError("D3D12 command list close failed");
         AbortFrame();
         return false;
      }

      ID3D12CommandList* command_lists[] = { frame_slot.command_list.Get() };
      m_queue->ExecuteCommandLists(1, command_lists);

      frame_slot.fence_value = m_next_frame_fence++;
      if (FAILED(m_queue->Signal(m_frame_fence.Get(), frame_slot.fence_value)))
      {
         SetError("D3D12 frame submission failed");
         m_frame_active = false;
         return false;
      }
      if (!SignalD3D12ToD3D11())
      {
         m_frame_active = false;
         return false;
      }

      m_native_context->CopyResource(m_output_target.Get(), m_output.shared_texture.Get());
      m_native_context->Flush();

      m_color.api.state = ResourceStateCommon;
      m_depth.api.state = ResourceStateCommon;
      m_motion_vectors.api.state = ResourceStateCommon;
      m_exposure.api.state = ResourceStateCommon;
      m_output.api.state = ResourceStateCommon;
      m_frame_active = false;
      return true;
   }

   void D3D11On12Bridge::AbortFrame()
   {
      if (!m_frame_active)
      {
         return;
      }

      if (m_frame_slots[m_frame_slot_index].command_list)
      {
         m_frame_slots[m_frame_slot_index].command_list->Close();
      }
      m_frame_active = false;
   }

   bool D3D11On12Bridge::PrepareInput(ResourceSlot& slot, ID3D11Resource* resource, ResourceKind kind)
   {
      if (!resource || !m_native_context)
      {
         SetError("D3D11 input resource is unavailable");
         return false;
      }

      ComPtr<ID3D11Device> resource_device;
      resource->GetDevice(resource_device.GetAddressOf());
      if (resource_device.Get() != m_native_device.Get())
      {
         SetError("D3D11 input resource belongs to a different device");
         return false;
      }

      ComPtr<ID3D11Texture2D> texture;
      if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
      {
         SetError("D3D11 resource is not a 2D texture");
         return false;
      }

      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      if (desc.Width == 0 || desc.Height == 0 || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
      {
         SetError("D3D11On12 only supports single-sample 2D textures");
         return false;
      }

      if (GetApiFormat(desc.Format) == FormatUnknown)
      {
         SetError("D3D11 texture format is not supported by the FSR API");
         return false;
      }

      if (slot.valid && slot.source_identity == resource && AreDescriptorsEqual(slot.desc, desc))
      {
         if (slot.copy_required)
         {
            m_native_context->CopyResource(slot.shared_texture.Get(), texture.Get());
         }
         return true;
      }

      // A resource can be referenced by the other command-list slot from the
      // previous frame. Do not release it until the D3D12 queue is idle.
      if (slot.valid && !WaitIdle())
      {
         return false;
      }
      slot.Reset();
      slot.source_texture = texture;
      slot.source_identity = resource;
      slot.desc = desc;

      if (!TryOpenNativeSharedResource(slot) && !CreateSharedTexture(desc, slot, false))
      {
         return false;
      }

      slot.api = {};
      slot.api.resource = slot.d3d12_resource.Get();
      slot.api.description.type = ResourceTypeTexture2D;
      slot.api.description.format = GetApiFormat(desc.Format);
      slot.api.description.width = desc.Width;
      slot.api.description.height = desc.Height;
      slot.api.description.depth = 1;
      slot.api.description.mip_count = desc.MipLevels;
      slot.api.description.flags = 0;
      slot.api.description.usage = GetApiUsage(desc.Format, kind, false);
      slot.api.state = ResourceStateCommon;
      slot.valid = true;

      if (slot.copy_required)
      {
         m_native_context->CopyResource(slot.shared_texture.Get(), texture.Get());
      }
      return true;
   }

   bool D3D11On12Bridge::PrepareOutput(ID3D11Resource* resource)
   {
      if (!resource)
      {
         SetError("D3D11 output resource is unavailable");
         return false;
      }

      ComPtr<ID3D11Device> resource_device;
      resource->GetDevice(resource_device.GetAddressOf());
      if (resource_device.Get() != m_native_device.Get())
      {
         SetError("D3D11 output resource belongs to a different device");
         return false;
      }

      ComPtr<ID3D11Texture2D> texture;
      if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
      {
         SetError("D3D11 output is not a 2D texture");
         return false;
      }

      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      if (desc.Width == 0 || desc.Height == 0 || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
      {
         SetError("D3D11On12 output must be a single-sample 2D texture");
         return false;
      }

      if (GetApiFormat(desc.Format) == FormatUnknown)
      {
         SetError("D3D11 output format is not supported by the FSR API");
         return false;
      }

      if (!m_output.valid || m_output.source_identity != resource || !AreDescriptorsEqual(m_output.desc, desc))
      {
         if (m_output.valid && !WaitIdle())
         {
            return false;
         }
         m_output.Reset();
         m_output.source_texture = texture;
         m_output.source_identity = resource;
         m_output.desc = desc;
         if (!CreateSharedTexture(desc, m_output, true))
         {
            return false;
         }

         m_output.api = {};
         m_output.api.resource = m_output.d3d12_resource.Get();
         m_output.api.description.type = ResourceTypeTexture2D;
         m_output.api.description.format = GetApiFormat(desc.Format);
         m_output.api.description.width = desc.Width;
         m_output.api.description.height = desc.Height;
         m_output.api.description.depth = 1;
         m_output.api.description.mip_count = desc.MipLevels;
         m_output.api.description.flags = 0;
         m_output.api.description.usage = ResourceUsageUav;
         m_output.api.state = ResourceStateCommon;
         m_output.valid = true;
      }

      m_output_target = resource;
      return true;
   }

   bool D3D11On12Bridge::TryOpenNativeSharedResource(ResourceSlot& slot)
   {
      if (!slot.source_texture || !m_device12)
      {
         return false;
      }

      ComPtr<IDXGIResource1> resource1;
      if ((slot.desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0 &&
          SUCCEEDED(slot.source_texture->QueryInterface(IID_PPV_ARGS(resource1.GetAddressOf()))) && resource1)
      {
         HANDLE shared_handle = nullptr;
         if (SUCCEEDED(resource1->CreateSharedHandle(nullptr, shared_resource_access, nullptr, &shared_handle)) && shared_handle)
         {
            if (OpenD3D12Resource(slot, shared_handle, true))
            {
               slot.shared_texture = slot.source_texture;
               slot.copy_required = false;
               return true;
            }
         }
      }

      if ((slot.desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0)
      {
         ComPtr<IDXGIResource> legacy_resource;
         if (SUCCEEDED(slot.source_texture->QueryInterface(IID_PPV_ARGS(legacy_resource.GetAddressOf()))) && legacy_resource)
         {
            HANDLE shared_handle = nullptr;
            if (SUCCEEDED(legacy_resource->GetSharedHandle(&shared_handle)) && shared_handle && OpenD3D12Resource(slot, shared_handle, false))
            {
               slot.shared_texture = slot.source_texture;
               slot.copy_required = false;
               return true;
            }
         }
      }

      slot.d3d12_resource.Reset();
      return false;
   }

   bool D3D11On12Bridge::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& source_desc, ResourceSlot& slot, bool output)
   {
      D3D11_TEXTURE2D_DESC shared_desc = source_desc;
      shared_desc.Usage = D3D11_USAGE_DEFAULT;
      shared_desc.CPUAccessFlags = 0;
      if (output)
      {
         shared_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      }

      const std::array<UINT, 2> sharing_flags = {
         D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
         D3D11_RESOURCE_MISC_SHARED,
      };
      for (const UINT sharing_flag : sharing_flags)
      {
         slot.shared_texture.Reset();
         slot.d3d12_resource.Reset();
         shared_desc.MiscFlags = sharing_flag;
         if (FAILED(m_native_device->CreateTexture2D(&shared_desc, nullptr, slot.shared_texture.GetAddressOf())))
         {
            continue;
         }

         HANDLE shared_handle = nullptr;
         bool close_handle = sharing_flag == D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
         if (close_handle)
         {
            ComPtr<IDXGIResource1> resource1;
            if (SUCCEEDED(slot.shared_texture->QueryInterface(IID_PPV_ARGS(resource1.GetAddressOf()))) && resource1)
            {
               resource1->CreateSharedHandle(nullptr, shared_resource_access, nullptr, &shared_handle);
            }
         }
         else
         {
            ComPtr<IDXGIResource> legacy_resource;
            if (SUCCEEDED(slot.shared_texture->QueryInterface(IID_PPV_ARGS(legacy_resource.GetAddressOf()))) && legacy_resource)
            {
               legacy_resource->GetSharedHandle(&shared_handle);
            }
         }

         if (shared_handle && OpenD3D12Resource(slot, shared_handle, close_handle))
         {
            slot.copy_required = true;
            slot.valid = true;
            return true;
         }
      }

      SetError("shared D3D11 texture creation or import failed");
      slot.Reset();
      return false;
   }

   bool D3D11On12Bridge::OpenD3D12Resource(ResourceSlot& slot, HANDLE shared_handle, bool close_handle)
   {
      if (!shared_handle || !m_device12)
      {
         if (close_handle && shared_handle)
         {
            CloseHandle(shared_handle);
         }
         return false;
      }

      const HRESULT hr = m_device12->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&slot.d3d12_resource));
      if (close_handle)
      {
         CloseHandle(shared_handle);
      }
      if (FAILED(hr) || !slot.d3d12_resource)
      {
         slot.d3d12_resource.Reset();
         return false;
      }
      return true;
   }

   void D3D11On12Bridge::ResetResources()
   {
      m_color.Reset();
      m_depth.Reset();
      m_motion_vectors.Reset();
      m_exposure.Reset();
      m_output.Reset();
      m_output_target.Reset();
   }

   bool D3D11On12Bridge::AreDescriptorsEqual(const D3D11_TEXTURE2D_DESC& left, const D3D11_TEXTURE2D_DESC& right)
   {
      return left.Width == right.Width &&
             left.Height == right.Height &&
             left.MipLevels == right.MipLevels &&
             left.ArraySize == right.ArraySize &&
             left.Format == right.Format &&
             left.SampleDesc.Count == right.SampleDesc.Count &&
             left.SampleDesc.Quality == right.SampleDesc.Quality &&
             left.Usage == right.Usage &&
             left.BindFlags == right.BindFlags &&
             left.CPUAccessFlags == right.CPUAccessFlags &&
             left.MiscFlags == right.MiscFlags;
   }

   std::uint32_t D3D11On12Bridge::GetApiUsage(DXGI_FORMAT format, ResourceKind kind, bool output)
   {
      if (output || kind == ResourceKind::Output)
      {
         return ResourceUsageUav;
      }

      if (kind == ResourceKind::Depth || IsDepthFormat(format))
      {
         std::uint32_t usage = ResourceUsageDepthTarget;
         if (IsStencilFormat(format))
         {
            usage |= ResourceUsageStencilTarget;
         }
         return usage;
      }

      return ResourceUsageReadOnly;
   }

   std::uint32_t D3D11On12Bridge::GetApiFormat(DXGI_FORMAT format)
   {
      switch (format)
      {
      case DXGI_FORMAT_R32G32B32A32_TYPELESS: return FormatR32G32B32A32Typeless;
      case DXGI_FORMAT_R32G32B32A32_UINT: return FormatR32G32B32A32Uint;
      case DXGI_FORMAT_R32G32B32A32_FLOAT: return FormatR32G32B32A32Float;
      case DXGI_FORMAT_R32G32B32_FLOAT: return FormatR32G32B32Float;
      case DXGI_FORMAT_R16G16B16A16_TYPELESS: return FormatR16G16B16A16Typeless;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: return FormatR16G16B16A16Float;
      case DXGI_FORMAT_R32G32_TYPELESS: return FormatR32G32Typeless;
      case DXGI_FORMAT_R32G32_FLOAT: return FormatR32G32Float;
      case DXGI_FORMAT_R32G32_UINT: return FormatR32G32Uint;
      case DXGI_FORMAT_R32G8X24_TYPELESS:
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
      case DXGI_FORMAT_D32_FLOAT:
      case DXGI_FORMAT_R32_FLOAT:
         return FormatR32Float;
      case DXGI_FORMAT_R32_TYPELESS: return FormatR32Typeless;
      case DXGI_FORMAT_R24G8_TYPELESS:
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
      case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return FormatR32Uint;
      case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
      case DXGI_FORMAT_R8_UINT: return FormatR8Uint;
      case DXGI_FORMAT_R10G10B10A2_TYPELESS: return FormatR10G10B10A2Typeless;
      case DXGI_FORMAT_R10G10B10A2_UNORM: return FormatR10G10B10A2Unorm;
      case DXGI_FORMAT_R11G11B10_FLOAT: return FormatR11G11B10Float;
      case DXGI_FORMAT_R8G8B8A8_TYPELESS: return FormatR8G8B8A8Typeless;
      case DXGI_FORMAT_R8G8B8A8_UNORM: return FormatR8G8B8A8Unorm;
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return FormatR8G8B8A8Srgb;
      case DXGI_FORMAT_R8G8B8A8_SNORM: return FormatR8G8B8A8Snorm;
      case DXGI_FORMAT_B8G8R8A8_TYPELESS: return FormatB8G8R8A8Typeless;
      case DXGI_FORMAT_B8G8R8A8_UNORM: return FormatB8G8R8A8Unorm;
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FormatB8G8R8A8Srgb;
      case DXGI_FORMAT_R16G16_TYPELESS: return FormatR16G16Typeless;
      case DXGI_FORMAT_R16G16_FLOAT: return FormatR16G16Float;
      case DXGI_FORMAT_R16G16_UINT: return FormatR16G16Uint;
      case DXGI_FORMAT_R16G16_SINT: return FormatR16G16Sint;
      case DXGI_FORMAT_R16_FLOAT: return FormatR16Float;
      case DXGI_FORMAT_R16_UINT: return FormatR16Uint;
      case DXGI_FORMAT_R16_UNORM: return FormatR16Unorm;
      case DXGI_FORMAT_R16_SNORM: return FormatR16Snorm;
      case DXGI_FORMAT_R8_UNORM: return FormatR8Unorm;
      case DXGI_FORMAT_R8G8_UNORM: return FormatR8G8Unorm;
      case DXGI_FORMAT_R8G8_UINT: return FormatR8G8Uint;
      case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return FormatR9G9B9E5SharedExp;
      case DXGI_FORMAT_R16_TYPELESS: return FormatR16Typeless;
      case DXGI_FORMAT_R8_TYPELESS: return FormatR8Typeless;
      case DXGI_FORMAT_R8G8_TYPELESS: return FormatR8G8Typeless;
      case DXGI_FORMAT_R8_SNORM: return FormatR8Snorm;
      default: return FormatUnknown;
      }
   }
}

#endif
