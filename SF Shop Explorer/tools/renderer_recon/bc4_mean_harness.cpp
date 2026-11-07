// Harness for the BC4 mean used as per-range metalness/roughness.
//
// The block under test is EXTRACTED verbatim from src/OfflineModel.cpp by
// run_bc4_mean_harness.sh (from `float Bc4Mean(` through the end of
// CompanionMapKey). Nothing is copied, so the harness cannot drift from the
// shipped code.
//
// Why this needs a test at all: the number this returns decides how metallic
// every range renders, and it is read from a compressed mask whose block layout
// is easy to get subtly wrong (a 3-bit index read out of the wrong byte gives a
// plausible-looking answer). The expectations below are hand-computed from the
// BC4 specification, so the assertions are independent of the implementation.

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "bc4_mean_block.inc"

namespace
{
	int g_fail = 0;

	void check(bool a_ok, const std::string& a_what)
	{
		std::cout << (a_ok ? "  ok   " : "  FAIL ") << a_what << "\n";
		if (!a_ok) {
			++g_fail;
		}
	}

	void checkNear(float a_got, float a_want, const std::string& a_what)
	{
		const bool ok = a_got >= a_want - 1e-4f && a_got <= a_want + 1e-4f;
		std::cout << (ok ? "  ok   " : "  FAIL ") << a_what << " (want " << a_want << ", got " << a_got << ")\n";
		if (!ok) {
			++g_fail;
		}
	}

	// One BC4 block: endpoints plus sixteen 3-bit indices packing 48 bits.
	std::vector<std::uint8_t> Block(std::uint8_t a_e0, std::uint8_t a_e1, const int a_indices[16])
	{
		std::vector<std::uint8_t> out{ a_e0, a_e1, 0, 0, 0, 0, 0, 0 };
		std::uint64_t packed = 0;
		for (int i = 0; i < 16; ++i) {
			packed |= static_cast<std::uint64_t>(a_indices[i] & 7) << (3 * i);
		}
		for (int i = 0; i < 6; ++i) {
			out[2 + i] = static_cast<std::uint8_t>((packed >> (8 * i)) & 0xFF);
		}
		return out;
	}
}  // namespace

int main()
{
	std::cout << "BC4 mean (metalness/roughness) harness\n";

	// --- endpoint-only blocks ---------------------------------------------
	{
		const int zero[16]{};
		const auto white = Block(255, 255, zero);
		checkNear(Bc4Mean(white, 4, 4), 1.0f, "e0 = e1 = 255 with index 0 is a mean of 1.0");
		const auto black = Block(0, 0, zero);
		checkNear(Bc4Mean(black, 4, 4), 0.0f, "e0 = e1 = 0 is a mean of 0.0");
	}
	{
		// Every texel on the second endpoint: the mean is that endpoint's value.
		int ones[16];
		for (int& v : ones) {
			v = 1;
		}
		const auto mid = Block(0, 128, ones);
		checkNear(Bc4Mean(mid, 4, 4), 128.0f / 255.0f, "all-index-1 block is the second endpoint");
	}

	// --- interpolation ----------------------------------------------------
	{
		// index 2 -> ((8 - 2) * 0 + 2 * 255 + 4) / 8 = 64
		int idx[16];
		for (int& v : idx) {
			v = 2;
		}
		checkNear(Bc4Mean(Block(0, 255, idx), 4, 4), 64.0f / 255.0f, "index 2 interpolates to 64/255");
		int idx7[16];
		for (int& v : idx7) {
			v = 7;
		}
		// ((8 - 7) * 0 + 7 * 255 + 4) / 8 = 223
		checkNear(Bc4Mean(Block(0, 255, idx7), 4, 4), 223.0f / 255.0f, "index 7 interpolates to 223/255");
	}

	// --- index bit packing ------------------------------------------------
	{
		// Texel 15 sits in the last three bits of the sixth index byte; a
		// decoder that reads it from the wrong place gets 0 here.
		int idx[16]{};
		idx[15] = 1;
		checkNear(Bc4Mean(Block(0, 255, idx), 4, 4), 255.0f / 255.0f / 16.0f,
			"a single endpoint-1 texel in slot 15 averages 1/16 of the endpoint");
	}

	// --- multi-block images ----------------------------------------------
	{
		const int zero[16]{};
		// 8x4 = two blocks side by side: 255 then 0 -> mean 127.5/255.
		std::vector<std::uint8_t> img = Block(255, 255, zero);
		const auto dark = Block(0, 0, zero);
		img.insert(img.end(), dark.begin(), dark.end());
		checkNear(Bc4Mean(img, 8, 4), 127.5f / 255.0f, "two blocks average together");
	}

	// --- unavailable, never mistaken for metalness 0 ----------------------
	{
		const int zero[16]{};
		const auto one = Block(255, 255, zero);
		checkNear(Bc4Mean(one, 0, 4), -1.0f, "zero width is unavailable, not 0.0");
		checkNear(Bc4Mean(one, 4, 0), -1.0f, "zero height is unavailable, not 0.0");
		checkNear(Bc4Mean(std::vector<std::uint8_t>{ 1, 2, 3 }, 4, 4), -1.0f, "short buffer is unavailable");
		std::vector<std::uint8_t> truncated = one;
		truncated.resize(truncated.size() - 1);
		checkNear(Bc4Mean(truncated, 4, 4), -1.0f, "truncated block is unavailable");
		checkNear(Bc4Mean({}, 4, 4), -1.0f, "empty buffer is unavailable");
	}

	// --- companion key derivation ----------------------------------------
	{
		check(CompanionMapKey("textures/weapons/sfbgs050/breechblock/receiver_2_color.dds", "_metal.dds") ==
				"textures/weapons/sfbgs050/breechblock/receiver_2_metal.dds",
			"_metal.dds pairs with the colour stem");
		check(CompanionMapKey("textures/weapons/dlc001/vortextgrenade/lambert1_color.dds", "_rough.dds") ==
				"textures/weapons/dlc001/vortextgrenade/lambert1_rough.dds",
			"_rough.dds pairs with the colour stem");
		check(CompanionMapKey("textures/weapons/dummy_atlas.dds", "_metal.dds").empty(),
			"a key with no _color suffix has no companion");
		check(CompanionMapKey("", "_metal.dds").empty(), "an empty key has no companion");
	}

	std::cout << (g_fail == 0 ? "ALL PASS" : "FAILURES") << " (" << g_fail << ")\n";
	return g_fail == 0 ? 0 : 1;
}
