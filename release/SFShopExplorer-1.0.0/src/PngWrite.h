#pragma once

// A minimal PNG writer for the offscreen preview frames.
//
// It lives in its own header because it is the one part of the preview path
// that can be exercised without Starfield or a GPU: the encoder is the step
// that used to fail silently inside WIC, so it is worth being able to compile
// and check it on its own.
//
// 8-bit RGBA, filter 0 per scanline, deflate level 6. That is exactly what a
// rendered frame needs and nothing a screenshot tool would want (no palettes,
// no interlacing, no ancillary chunks).

#include <cstdint>
#include <cstddef>
#include <vector>

#include <zlib.h>

namespace SFE::Png
{
	namespace Detail
	{
		inline void PutBigEndian(std::vector<std::uint8_t>& a_out, const std::uint32_t a_value)
		{
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 24) & 0xFF));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 16) & 0xFF));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 8) & 0xFF));
			a_out.push_back(static_cast<std::uint8_t>(a_value & 0xFF));
		}

		inline void PutChunk(std::vector<std::uint8_t>& a_out, const char* a_type, const std::uint8_t* a_data,
			const std::size_t a_size)
		{
			PutBigEndian(a_out, static_cast<std::uint32_t>(a_size));
			const auto typeAt = a_out.size();
			a_out.insert(a_out.end(), a_type, a_type + 4);
			if (a_size) {
				a_out.insert(a_out.end(), a_data, a_data + a_size);
			}
			const auto crc = ::crc32(0L, a_out.data() + typeAt, static_cast<uInt>(4 + a_size));
			PutBigEndian(a_out, static_cast<std::uint32_t>(crc));
		}
	}

	// a_rgba is top-down, tightly packed, width * height * 4 bytes.
	[[nodiscard]] inline bool EncodeRgba(const std::vector<std::uint8_t>& a_rgba, const std::uint32_t a_width,
		const std::uint32_t a_height, std::vector<std::uint8_t>& a_out)
	{
		if (a_width == 0 || a_height == 0 ||
			a_rgba.size() < static_cast<std::size_t>(a_width) * a_height * 4u) {
			return false;
		}
		const std::size_t stride = static_cast<std::size_t>(a_width) * 4u;
		std::vector<std::uint8_t> raw;
		raw.reserve(static_cast<std::size_t>(a_height) * (stride + 1u));
		for (std::uint32_t row = 0; row < a_height; ++row) {
			raw.push_back(0);  // filter: none
			const auto* line = a_rgba.data() + static_cast<std::size_t>(row) * stride;
			raw.insert(raw.end(), line, line + stride);
		}
		uLongf packed = ::compressBound(static_cast<uLong>(raw.size()));
		std::vector<std::uint8_t> deflated(packed);
		if (::compress2(deflated.data(), &packed, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
			return false;
		}
		deflated.resize(packed);

		a_out.clear();
		a_out.reserve(deflated.size() + 64);
		static constexpr std::uint8_t kSignature[8]{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		a_out.insert(a_out.end(), kSignature, kSignature + 8);
		std::vector<std::uint8_t> header;
		Detail::PutBigEndian(header, a_width);
		Detail::PutBigEndian(header, a_height);
		header.push_back(8);   // bit depth
		header.push_back(6);   // colour type: truecolour with alpha
		header.push_back(0);   // deflate
		header.push_back(0);   // adaptive filtering
		header.push_back(0);   // no interlace
		Detail::PutChunk(a_out, "IHDR", header.data(), header.size());
		Detail::PutChunk(a_out, "IDAT", deflated.data(), deflated.size());
		Detail::PutChunk(a_out, "IEND", nullptr, 0);
		return true;
	}
}
