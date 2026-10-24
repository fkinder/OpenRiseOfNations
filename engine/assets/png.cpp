#include "assets/png.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>

namespace ron {

namespace {

uint32_t be32(const Bytes& b, size_t p) {
	if (p + 4 > b.size()) throw FormatError("png: truncated");
	return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) | (uint32_t(b[p + 2]) << 8) | uint32_t(b[p + 3]);
}

Bytes inflate_all(const Bytes& in, size_t expect) {
	Bytes out(expect);
	z_stream z{};
	if (inflateInit(&z) != Z_OK) throw FormatError("png: inflateInit failed");
	z.next_in = const_cast<Bytef*>(in.data());
	z.avail_in = uInt(in.size());
	z.next_out = out.data();
	z.avail_out = uInt(out.size());
	const int rc = inflate(&z, Z_FINISH);
	const size_t got = out.size() - z.avail_out;
	inflateEnd(&z);
	if ((rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) || got != expect) throw FormatError("png: bad image data");
	return out;
}

int paeth(int a, int b, int c) {
	const int p = a + b - c;
	const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

// Undoes the filters of one (sub)image in place: `rows` scanlines of `stride` bytes each preceded by a filter byte.
void unfilter(uint8_t* data, int rows, size_t stride, int bpp) {
	std::vector<uint8_t> zero(stride, 0);
	const uint8_t* prev = zero.data();
	for (int y = 0; y < rows; y++) {
		uint8_t* line = data + size_t(y) * (stride + 1);
		const int f = line[0];
		uint8_t* cur = line + 1;
		switch (f) {
		case 0: break;
		case 1:
			for (size_t i = size_t(bpp); i < stride; i++) cur[i] = uint8_t(cur[i] + cur[i - size_t(bpp)]);
			break;
		case 2:
			for (size_t i = 0; i < stride; i++) cur[i] = uint8_t(cur[i] + prev[i]);
			break;
		case 3:
			for (size_t i = 0; i < stride; i++) {
				const int left = i >= size_t(bpp) ? cur[i - size_t(bpp)] : 0;
				cur[i] = uint8_t(cur[i] + ((left + prev[i]) >> 1));
			}
			break;
		case 4:
			for (size_t i = 0; i < stride; i++) {
				const int left = i >= size_t(bpp) ? cur[i - size_t(bpp)] : 0;
				const int ul = i >= size_t(bpp) ? prev[i - size_t(bpp)] : 0;
				cur[i] = uint8_t(cur[i] + paeth(left, prev[i], ul));
			}
			break;
		default: throw FormatError("png: bad filter type");
		}
		prev = cur;
	}
}

}  // namespace

Image parse_png(const Bytes& b) {
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	if (b.size() < 8 || !std::equal(sig, sig + 8, b.begin())) throw FormatError("png: bad signature");
	uint32_t W = 0, H = 0;
	int depth = 0, ctype = -1, interlace = 0;
	std::vector<uint8_t> palette, trns;
	Bytes idat;
	size_t p = 8;
	bool have_ihdr = false, have_end = false;
	while (p + 8 <= b.size() && !have_end) {
		const uint32_t len = be32(b, p);
		const uint32_t type = be32(b, p + 4);
		const size_t d = p + 8;
		if (d + len + 4 > b.size()) throw FormatError("png: truncated chunk");
		if (type == 0x49484452) {  // IHDR
			if (len < 13) throw FormatError("png: bad IHDR");
			W = be32(b, d);
			H = be32(b, d + 4);
			depth = b[d + 8];
			ctype = b[d + 9];
			interlace = b[d + 12];
			have_ihdr = true;
		} else if (type == 0x504c5445) {  // PLTE
			palette.assign(b.begin() + long(d), b.begin() + long(d + len));
		} else if (type == 0x74524e53) {  // tRNS
			trns.assign(b.begin() + long(d), b.begin() + long(d + len));
		} else if (type == 0x49444154) {  // IDAT
			idat.insert(idat.end(), b.begin() + long(d), b.begin() + long(d + len));
		} else if (type == 0x49454e44) {
			have_end = true;
		}
		p = d + len + 4;
	}
	if (!have_ihdr || W == 0 || H == 0 || W > 65535 || H > 65535) throw FormatError("png: bad header");
	int channels;
	switch (ctype) {
	case 0: channels = 1; break;
	case 2: channels = 3; break;
	case 3: channels = 1; break;
	case 4: channels = 2; break;
	case 6: channels = 4; break;
	default: throw FormatError("png: bad colour type");
	}
	if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) throw FormatError("png: bad bit depth");
	if (ctype == 3 && (palette.empty() || depth == 16)) throw FormatError("png: palette image without PLTE");
	if ((ctype == 2 || ctype == 4 || ctype == 6) && depth < 8) throw FormatError("png: bad depth for colour type");
	if (interlace != 0 && interlace != 1) throw FormatError("png: bad interlace method");
	const int bits_pp = channels * depth;
	const int bpp = std::max(1, bits_pp / 8);

	struct Pass {
		int x0, y0, dx, dy;
	};
	static const Pass adam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
	static const Pass whole[1] = {{0, 0, 1, 1}};
	const Pass* passes = interlace ? adam7 : whole;
	const int npass = interlace ? 7 : 1;
	struct Dim {
		int w, h;
		size_t stride, offset;
	};
	Dim dim[7];
	size_t total = 0;
	for (int i = 0; i < npass; i++) {
		const Pass& ps = passes[i];
		const int w = int((W - uint32_t(ps.x0) + uint32_t(ps.dx) - 1) / uint32_t(ps.dx));
		const int h = int((H - uint32_t(ps.y0) + uint32_t(ps.dy) - 1) / uint32_t(ps.dy));
		const int ww = ps.x0 >= int(W) ? 0 : w, hh = ps.y0 >= int(H) ? 0 : h;
		dim[i].w = ww;
		dim[i].h = hh;
		dim[i].stride = (size_t(ww) * size_t(bits_pp) + 7) / 8;
		dim[i].offset = total;
		if (ww > 0 && hh > 0) total += size_t(hh) * (dim[i].stride + 1);
	}
	Bytes raw = inflate_all(idat, total);

	Image img;
	img.width = int(W);
	img.height = int(H);
	img.rgba.assign(size_t(W) * size_t(H) * 4, 0);
	for (int i = 0; i < npass; i++) {
		const Dim& dm = dim[i];
		if (dm.w <= 0 || dm.h <= 0) continue;
		uint8_t* base = raw.data() + dm.offset;
		unfilter(base, dm.h, dm.stride, bpp);
		for (int y = 0; y < dm.h; y++) {
			const uint8_t* row = base + size_t(y) * (dm.stride + 1) + 1;
			const int oy = passes[i].y0 + y * passes[i].dy;
			for (int x = 0; x < dm.w; x++) {
				const int ox = passes[i].x0 + x * passes[i].dx;
				uint8_t* o = &img.rgba[(size_t(oy) * W + size_t(ox)) * 4];
				// sample k of the pixel, as an 8-bit value (16-bit: high byte)
				auto sample = [&](int k) -> int {
					if (depth == 8) return row[size_t(x) * size_t(channels) + size_t(k)];
					if (depth == 16) return row[(size_t(x) * size_t(channels) + size_t(k)) * 2];
					const size_t bit = size_t(x) * size_t(depth);
					const int v = (row[bit >> 3] >> (8 - depth - int(bit & 7))) & ((1 << depth) - 1);
					return v;
				};
				auto sample16 = [&](int k) -> int {
					return depth == 16 ? (row[(size_t(x) * size_t(channels) + size_t(k)) * 2] << 8) | row[(size_t(x) * size_t(channels) + size_t(k)) * 2 + 1] : sample(k);
				};
				switch (ctype) {
				case 0: {
					int v = sample(0);
					const int raw_v = v;
					if (depth < 8) v = v * 255 / ((1 << depth) - 1);
					o[0] = o[1] = o[2] = uint8_t(v);
					o[3] = 255;
					if (trns.size() >= 2) {
						const int key = (trns[0] << 8) | trns[1];
						if (sample16(0) == key || (depth < 8 && raw_v == key)) o[3] = 0;
					}
					break;
				}
				case 2:
					o[0] = uint8_t(sample(0));
					o[1] = uint8_t(sample(1));
					o[2] = uint8_t(sample(2));
					o[3] = 255;
					if (trns.size() >= 6 && sample16(0) == ((trns[0] << 8) | trns[1]) && sample16(1) == ((trns[2] << 8) | trns[3]) &&
					    sample16(2) == ((trns[4] << 8) | trns[5]))
						o[3] = 0;
					break;
				case 3: {
					const size_t idx = size_t(sample(0));
					if (idx * 3 + 2 >= palette.size()) throw FormatError("png: palette index out of range");
					o[0] = palette[idx * 3];
					o[1] = palette[idx * 3 + 1];
					o[2] = palette[idx * 3 + 2];
					o[3] = idx < trns.size() ? trns[idx] : 255;
					break;
				}
				case 4:
					o[0] = o[1] = o[2] = uint8_t(sample(0));
					o[3] = uint8_t(sample(1));
					break;
				default:
					o[0] = uint8_t(sample(0));
					o[1] = uint8_t(sample(1));
					o[2] = uint8_t(sample(2));
					o[3] = uint8_t(sample(3));
					break;
				}
			}
		}
	}
	return img;
}

bool load_png(const std::string& path, Image& out, std::string* error) {
	try {
		out = parse_png(read_file(path));
		return true;
	} catch (const std::exception& e) {
		if (error) *error = path + ": " + e.what();
		return false;
	}
}

}  // namespace ron
