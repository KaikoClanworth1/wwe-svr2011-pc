// A minimal zip writer (stored entries, no compression): what .svrmod files
// are. The launcher's unzip (launcher/unzip.c) reads stored and deflated.
#pragma once

#include <string>
#include <vector>

#include "bytes.h"

namespace svrfmt {

struct ZipEntry {
  std::string name;  // path inside the zip, '/' separated
  Bytes data;
};

Bytes ZipWrite(const std::vector<ZipEntry>& entries);
// Reads a zip of stored entries (what ZipWrite makes); false on anything else.
bool ZipRead(const Bytes& zip, std::vector<ZipEntry>& out);

}  // namespace svrfmt
