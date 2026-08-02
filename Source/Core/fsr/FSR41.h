#pragma once

#include "../includes/super_resolution.h"

#if defined(_WIN64) && defined(ENABLE_FSR41) && ENABLE_FSR41 && defined(ENABLE_FIDELITY_SK) && ENABLE_FIDELITY_SK

#include "FSR.h"

namespace FidelityFX
{
   // Optional FSR 4.1 backend for D3D11 games. The provider and its D3D12
   // backend are loaded at runtime; FSR3 remains the in-process fallback.
   class FSR41 : public SR::SuperResolutionImpl
   {
   public:
      virtual bool HasInit(const SR::InstanceData* data) const override;
      virtual bool IsSupported(const SR::InstanceData* data) const override;

      virtual bool Init(SR::InstanceData*& data, ID3D11Device* device, IDXGIAdapter* adapter = nullptr) override;
      virtual void Deinit(SR::InstanceData*& data, ID3D11Device* optional_device = nullptr) override;

      virtual bool UpdateSettings(SR::InstanceData* data, ID3D11DeviceContext* command_list, const SR::SettingsData& settings_data) override;
      virtual bool Draw(const SR::InstanceData* data, ID3D11DeviceContext* command_list, const DrawData& draw_data) override;

      virtual int GetJitterPhases(const SR::InstanceData* data) const override;
      virtual bool NeedsStateRestoration() const override { return true; }
      virtual SR::Type GetFallbackType(const SR::InstanceData* data) const override;
      virtual const char* GetBackendName(const SR::InstanceData* data) const override;
   };
}

#endif
