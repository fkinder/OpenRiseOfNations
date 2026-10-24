// A minimal PNG decoder (zlib's inflate + the five scanline filters): 8-bit grey, grey+alpha, RGB, RGBA and palette
// images (tRNS honoured), 1/2/4-bit palette and grey, 16-bit channels reduced to their high byte, Adam7 interlace.
// The output is always RGBA8 with a top-left origin, the same layout as parse_tga's Image. Used for the Conquer the
// World silhouette pieces (conquest/RenderMaps/MapPieces/<map>/xpack_temp_country<N><map>.png).
#pragma once

#include <string>

#include "assets/chunk.hpp"
#include "assets/tga.hpp"

namespace ron {

// Throws FormatError on a malformed or unsupported file (bad signature, CRC is not checked, truncated data).
Image parse_png(const Bytes& bytes);

// Reads and decodes a file; returns false (with `error`) instead of throwing.
bool load_png(const std::string& path, Image& out, std::string* error = nullptr);

}  // namespace ron
