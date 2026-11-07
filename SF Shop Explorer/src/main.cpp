// ============================================================================
// main.cpp - plugin entry point and the per-frame task
//
// Load: capture the game build, configure logging, register for SFSE messages.
// The renderer and audio cannot be touched until the game has one, so everything
// else waits for kPostPostLoad and then runs from a permanent task: install the
// overlay, pump the shop, tell the overlay whether to draw.
//
// The OSF build takes the same path minus the overlay: its page is the interface,
// so the task only ticks the bridge (see OsfFrontend.cpp).
// ============================================================================

#include "PCH.h"
#include "ShopExplorer.h"
#include "ImguiOverlay.h"
#include "ItemCapture.h"

#include "SFSE/SFSE.h"

#include <d3d12sdklayers.h>
#include <filesystem>
#include <spdlog/spdlog.h>

#include "BuildGate.h"
#include "ShopAudio.h"
#ifdef SFE_OSF_FRONTEND
#include "OsfFrontend.h"
#endif

namespace
{
	class BrowserTask : public SFSE::ITaskDelegate
	{
	public:
		void Run() override
		{
#ifndef SFE_OSF_FRONTEND
			SFE::Overlay::Install();
			SFE::ItemCapture::FramePoll();
#endif
			SFE::ShopBrowser::GetSingleton()->FrameUpdate();
#ifdef SFE_OSF_FRONTEND
			// The OSF build draws nothing of its own: the page is the interface,
			// so all this task has to do beyond the engine tick is attach the
			// bridge once OSF UI is loadable.
			SFE::Osf::Tick();
#else
			SFE::Overlay::SetDrawingEnabled(SFE::ShopBrowser::GetSingleton()->IsOpen());
#endif
		}
		void Destroy() override { delete this; }
	};

	std::filesystem::path PluginDirectory()
	{
		wchar_t path[MAX_PATH]{};
		const auto module = GetModuleHandleW(SFE::kDllFileName);
		const auto length = module ? GetModuleFileNameW(module, path, MAX_PATH) : 0;
		if (length == 0 || length >= MAX_PATH) {
			return {};
		}
		return std::filesystem::path(path).parent_path();
	}

	// Log budget. The preview's RTTI field-scan and node-walk diagnostics are
	// REX::DEBUG now: one item could write hundreds of lines, and the
	// 2026-10-24 sitting produced 35k lines. The default level therefore keeps
	// the probes off disk entirely; [logging] verbose=1 in SFShopExplorer.ini
	// raises the level for a diagnostic session. flush_on(warn) keeps the
	// per-frame INFO chatter out of the file write path.
	void ConfigureLogging()
	{
		const auto ini = PluginDirectory() / SFE::kIniFileName;
		const bool verbose = GetPrivateProfileIntW(L"logging", L"verbose", 0, ini.c_str()) != 0;
		auto* logger = spdlog::default_logger_raw();
		if (!logger) {
			return;
		}
		logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
		logger->flush_on(logger->level() <= spdlog::level::debug ? spdlog::level::info : spdlog::level::warn);
		REX::INFO("SFShopExplorer: logging level={} (probes are DEBUG; [logging] verbose=1 shows them)",
			verbose ? "debug" : "info");
	}

	void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SFSE::MessagingInterface::kPostLoad) {
			REX::INFO("SFShopExplorer: kPostLoad received; renderer installation deferred to task");
		} else if (a_msg->type == SFSE::MessagingInterface::kPostPostLoad) {
			if (const auto tasks = SFSE::GetTaskInterface()) {
				// Audio must come first: the task's very first tick can already
				// call PlaySfx, so registering the task before the device exists
				// would drop the opening sting.
				SFE::ShopAudio::Init();
				tasks->AddPermanentTask(new BrowserTask());
				REX::INFO("SFShopExplorer: browser task registered");
			} else {
				REX::ERROR("SFShopExplorer: failed to get task interface");
			}
		}
	}
}

namespace SFE
{
	// One read of [logging] verbose for the process. The probes that write
	// outside the logger - the DRED block below and the %TEMP% prefetch trace -
	// need the same switch the log level uses, and the trace would otherwise
	// hit the profile API once per texture it resolves.
	bool VerboseLogging()
	{
		static const bool s_verbose = []() {
			const auto ini = PluginDirectory() / SFE::kIniFileName;
			return GetPrivateProfileIntW(L"logging", L"verbose", 0, ini.c_str()) != 0;
		}();
		return s_verbose;
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	ConfigureLogging();
	// Diagnostic build 0.8.324: DRED breadcrumbs, NOT the debug layer. The
	// debug layer validated the game's own command lists too and made it
	// unlaunchable; DRED only records operation breadcrumbs and leaves
	// validation alone. Settings must be set before the game creates its
	// device; SFSE::Init does not create it (the 0.8.323 info queue attached
	// on the first Present, after this point). Logged after SFSE::Init because
	// REX has no sink before it. Gated on [logging] verbose: forcing both
	// enablements on every launch is a developer probe, and it wrote a WARN
	// into every player's log on every start.
	if (SFE::VerboseLogging())
	{
		ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
		const auto hr = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
		if (SUCCEEDED(hr) && dred) {
			dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->Release();
			REX::WARN("SFShopExplorer diagnostics: DRED breadcrumbs enabled at plugin load");
		} else {
			REX::WARN("SFShopExplorer diagnostics: DRED settings unavailable (hr=0x{:08X})",
				static_cast<unsigned>(hr));
		}
	}
	// Before anything else can use them: record the running build so the paths
	// that rely on unlisted addresses (the pause counter, the mod-application
	// spike) can refuse themselves on a build they were not verified against.
	if (a_sfse) {
		const auto runtime = a_sfse->RuntimeVersion();
		SFE::BuildGate::Capture(runtime.major(), runtime.minor(), runtime.patch(), runtime.build());
	} else {
		REX::WARN("SFShopExplorer: no SFSE interface at load; unlisted internals stay disabled");
	}
	const auto messaging = SFSE::GetMessagingInterface();
	if (!messaging) {
		REX::ERROR("SFShopExplorer: failed to get messaging interface");
		return false;
	}
	messaging->RegisterListener(MessageHandler);
	REX::INFO("SFShopExplorer v{}: plugin loaded; {} front end", SFE::kVersion,
#ifdef SFE_OSF_FRONTEND
		"OSF UI view"
#else
		"standalone overlay (no game menu host)"
#endif
	);
	return true;
}
