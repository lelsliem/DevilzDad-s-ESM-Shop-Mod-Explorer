	float Bc4Mean(const std::vector<std::uint8_t>& a_bytes, std::uint32_t a_w, std::uint32_t a_h)
	{
		if (a_bytes.size() < 8 || a_w == 0 || a_h == 0) {
			return -1.0f;
		}
		const std::size_t rowPitch = static_cast<std::size_t>((a_w + 3) / 4) * 8;
		const std::size_t blocksX = (a_w + 3) / 4;
		const std::size_t blocksY = (a_h + 3) / 4;
		double total = 0.0;
		std::uint64_t samples = 0;
		for (std::size_t by = 0; by < blocksY; ++by) {
			for (std::size_t bx = 0; bx < blocksX; ++bx) {
				const std::size_t off = by * rowPitch + bx * 8;
				if (off + 8 > a_bytes.size()) {
					return -1.0f;
				}
				const int e0 = a_bytes[off];
				const int e1 = a_bytes[off + 1];
				std::uint64_t idx = 0;
				for (int i = 0; i < 6; ++i) {
					idx |= static_cast<std::uint64_t>(a_bytes[off + 2 + i]) << (8 * i);
				}
				for (int t = 0; t < 16; ++t) {
					const int k = static_cast<int>((idx >> (3 * t)) & 7u);
					int v = 0;
					if (k == 0) {
						v = e0;
					} else if (k == 1) {
						v = e1;
					} else {
						v = ((8 - k) * e0 + k * e1 + 4) / 8;
					}
					total += v;
					++samples;
				}
			}
		}
		if (samples == 0) {
			return -1.0f;
		}
		return static_cast<float>(total / static_cast<double>(samples) / 255.0);
	}

	// `textures/a/b_color.dds` -> `textures/a/b_metal.dds`. The companions sit
	// beside the colour map with the same stem in every shipped weapon set
	// (`receiver_2_color/metal/rough`, `grip_color/metal/rough`), and the
	// material database names only the colour and opacity slots, so pairing by
	// name is the only route to them.
	std::string CompanionMapKey(const std::string& a_colorKey, const char* a_suffix)
	{
		const auto at = a_colorKey.rfind("_color.dds");
		if (at == std::string::npos) {
			return {};
		}
		return a_colorKey.substr(0, at) + a_suffix;
	}
