// WIC-based image conversion to PNG 96 DPI

#include "image_convert.h"

#include <wincodec.h>
#include <Windows.h>
#include <winrt/base.h>

namespace platf::img {

  struct CoInitGuard {
    bool inited = false;

    CoInitGuard() {
      inited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    }

    ~CoInitGuard() {
      if (inited) {
        CoUninitialize();
      }
    }
  };

  bool convert_to_png_96dpi(const std::wstring &src_path, const std::wstring &dst_path) {
    CoInitGuard co;
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(src_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) {
      return false;
    }

    // Convert to a well-supported pixel format if needed
    GUID pf = GUID_WICPixelFormat32bppPBGRA;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) {
      return false;
    }
    if (FAILED(converter->Initialize(frame.get(), pf, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return false;
    }

    // Create output stream and encoder
    winrt::com_ptr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) {
      return false;
    }
    if (FAILED(stream->InitializeFromFilename(dst_path.c_str(), GENERIC_WRITE))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))) {
      return false;
    }
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
      return false;
    }

    winrt::com_ptr<IWICBitmapFrameEncode> fenc;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(fenc.put(), props.put()))) {
      return false;
    }
    if (FAILED(fenc->Initialize(props.get()))) {
      return false;
    }

    // Set 96 DPI to avoid scaling issues
    if (FAILED(fenc->SetResolution(96.0, 96.0))) {
      return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h))) {
      return false;
    }
    if (FAILED(fenc->SetSize(w, h))) {
      return false;
    }
    WICPixelFormatGUID outFmt = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(fenc->SetPixelFormat(&outFmt))) {
      return false;
    }

    // Write pixels with no resampling (preserve pixel dimensions)
    const UINT stride = w * 4;
    const UINT bufSize = stride * h;
    std::unique_ptr<BYTE[]> buffer = std::make_unique<BYTE[]>(bufSize);
    if (FAILED(converter->CopyPixels(nullptr, stride, bufSize, buffer.get()))) {
      return false;
    }
    if (FAILED(fenc->WritePixels(h, stride, bufSize, buffer.get()))) {
      return false;
    }
    if (FAILED(fenc->Commit())) {
      return false;
    }
    if (FAILED(encoder->Commit())) {
      return false;
    }
    return true;
  }
}  // namespace platf::img
