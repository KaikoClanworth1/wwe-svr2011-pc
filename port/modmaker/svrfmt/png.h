// PNG/JPG/TGA load (stb_image) and PNG save (Windows Imaging Component).
#pragma once

#include <string>

#include "texture.h"

namespace svrfmt {

bool LoadImageFile(const std::string& path, Image& out);
bool SavePng(const std::string& path, const Image& img);

}  // namespace svrfmt
