#pragma once

// OSF front-end entry points. Built only into the OSF target
// (src/main.cpp includes this header under SFE_OSF_FRONTEND), because the OSF
// build presents the shop through an OSF UI view instead of the ImGui overlay.
namespace SFE::Osf
{
	// Attaches to OSF UI once its DLL is loadable and registers the view.
	// Safe to call repeatedly: a miss is retried on the next tick, because OSF
	// UI can finish loading after this plugin.
	void Tick();

	// True once the view and its request endpoints are registered.
	[[nodiscard]] bool IsAttached();
}
