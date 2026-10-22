#pragma once
#include "frame_push_transport.h"
#ifdef _WIN32
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace platf::frame_push {
  class D3D11PrivateFrame final: public vdd::FrameRingResources {
  public:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  };
  // Host ring allocation/import counterpart used by the opt-in display adapter.
  // The caller supplies its selected adapter/device and an owner-authenticated
  // live display generation; the factory never guesses a monitor or adapter.
  std::shared_ptr<ConsumerResources> create_d3d11_consumer_ring(
    ID3D11Device1 *device, ID3D11DeviceContext *context, vdd::FrameRingRegisterRequest target);
}
#endif
