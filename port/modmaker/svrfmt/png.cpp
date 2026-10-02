#include "png.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#include "stb_image.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include <vector>

namespace svrfmt {

bool LoadImageFile(const std::string& path, Image& out) {
  int w, h, n;
  stbi_uc* p = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!p) return false;
  out.w = w;
  out.h = h;
  out.rgba.assign(p, p + size_t(w) * h * 4);
  stbi_image_free(p);
  return true;
}

namespace {
std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
}  // namespace

bool SavePng(const std::string& path, const Image& img) {
  static bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
  (void)com;
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  bool ok = false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  std::vector<uint8_t> bgra(img.rgba.size());
  for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
    bgra[i] = img.rgba[i + 2];
    bgra[i + 1] = img.rgba[i + 1];
    bgra[i + 2] = img.rgba[i];
    bgra[i + 3] = img.rgba[i + 3];
  }
  if (SUCCEEDED(f->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(Wide(path).c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->SetSize(UINT(img.w), UINT(img.h)))) {
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(frame->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppBGRA &&
        SUCCEEDED(frame->WritePixels(UINT(img.h), UINT(img.w * 4), UINT(bgra.size()), bgra.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
      ok = true;
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  f->Release();
  return ok;
}

}  // namespace svrfmt
