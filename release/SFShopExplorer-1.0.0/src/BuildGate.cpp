// ============================================================================
// BuildGate.cpp - one answer to "is this build verified?"
//
// Two features reach the engine through addresses the Address Library does not
// list: the pause counter and the purchase spike. Both were verified against
// 1.16.244, so both ask here first and refuse themselves on any other build
// instead of writing to an address that now means something else.
// ============================================================================

#include "PCH.h"
#include "BuildGate.h"

namespace SFE::BuildGate
{
	namespace
	{
		// The build the unlisted internals were verified against.
		//
		// ShopPause.cpp's UI+0x4B4 pause counter and the spike's .text
		// addresses (0x42C7B0 / 0x5DDBD0 / 0x61DD4FF) were read out of
		// Starfield 1.16.244 and cross-checked against versionlib-1-16-244-0.
		// Raise this only after re-reading those addresses on a new build and
		// re-running the pause and mod-application checks in game.
		constexpr std::uint16_t kVerifiedMajor = 1;
		constexpr std::uint16_t kVerifiedMinor = 16;
		constexpr std::uint16_t kVerifiedPatch = 244;
		constexpr std::uint16_t kVerifiedBuild = 0;

		std::mutex  g_mutex;
		std::string g_runtime{ "unknown" };
		bool        g_captured{ false };
		bool        g_verified{ false };

		// One log line per refused feature, not one per frame: the freeze is
		// re-asserted every frame and would otherwise flood the log.
		std::mutex              g_refusedMutex;
		std::vector<std::string> g_refused;
	}

	void Capture(std::uint16_t a_major, std::uint16_t a_minor,
		std::uint16_t a_patch, std::uint16_t a_build)
	{
		std::scoped_lock lock(g_mutex);
		g_runtime = std::format("{}.{}.{}.{}", a_major, a_minor, a_patch, a_build);
		g_captured = true;
		g_verified = a_major == kVerifiedMajor && a_minor == kVerifiedMinor &&
			a_patch == kVerifiedPatch && a_build == kVerifiedBuild;
		REX::INFO("SFShopExplorer: game runtime {}; unlisted internals {} (verified against {}.{}.{})",
			g_runtime, g_verified ? "match" : "DO NOT match - freeze and mod delivery disabled",
			kVerifiedMajor, kVerifiedMinor, kVerifiedPatch);
	}

	bool Verified()
	{
		std::scoped_lock lock(g_mutex);
		return g_captured && g_verified;
	}

	std::string RuntimeString()
	{
		std::scoped_lock lock(g_mutex);
		return g_runtime;
	}

	const char* VerifiedVersionString()
	{
		return "1.16.244";
	}

	void Refuse(const char* a_feature)
	{
		if (!a_feature) {
			return;
		}
		{
			std::scoped_lock lock(g_refusedMutex);
			for (const auto& seen : g_refused) {
				if (seen == a_feature) {
					return;
				}
			}
			g_refused.emplace_back(a_feature);
		}
		REX::WARN("SFShopExplorer: '{}' disabled - this plugin's addresses for it were verified "
			"against Starfield {} and you are on {}",
			a_feature, VerifiedVersionString(), RuntimeString());
	}
}
