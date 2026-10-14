// PNG/JPG/TGA/BMP/PSD load (stb_image) and PNG save (Windows Imaging Component).
#pragma once

#include <string>

#include "texture.h"

namespace svrfmt {

bool LoadImageFile(const std::string& path, Image& out);  // (PSD too: its composited image)
bool LoadImageMemory(const uint8_t* data, size_t size, Image& out);
bool SavePng(const std::string& path, const Image& img);

}  // namespace svrfmt
