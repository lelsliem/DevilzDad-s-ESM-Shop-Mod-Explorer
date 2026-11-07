#pragma once

#include <cstdint>
#include <string>

// One place that answers "is this a game build whose internals this plugin was
// verified against?".
//
// Two of this plugin's features reach the engine through addresses that are
// not in the Address Library at all: the world-freeze counter at
// UI+0x4B4 and the mod-application spike's code addresses (see
// ShopPause.cpp and the [spike] block in ShopExplorer.cpp). Both were verified
// against Starfield 1.16.244 by reading the running process and cross-checking
// the official versionlib. On any other build those constants name different
// memory: the freeze writes a counter into whatever object now lives at that
// offset, and the spike calls into an arbitrary .text address.
//
// Every such path asks this gate first. Fail-closed on purpose: until a
// runtime version has been captured, nothing is verified, so nothing runs.
namespace SFE::BuildGate
{
	// Called once from plugin load with the SFSE-reported runtime version.
	void Capture(std::uint16_t a_major, std::uint16_t a_minor,
		std::uint16_t a_patch, std::uint16_t a_build);

	// True only on a build whose internals the hardcoded addresses match.
	[[nodiscard]] bool Verified();

	// The captured runtime version, for logs and the refusal line. "unknown"
	// until Capture.
	[[nodiscard]] std::string RuntimeString();

	// The version this build's internals were verified against, "1.16.244".
	[[nodiscard]] const char* VerifiedVersionString();

	// Log, once per feature name, that a feature was refused for this build.
	// One line per feature rather than per frame.
	void Refuse(const char* a_feature);
}
