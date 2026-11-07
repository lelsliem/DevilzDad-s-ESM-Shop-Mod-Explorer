#include "PCH.h"
#include "ShopExplorer.h"
#include "ShopMenu.h"
#include "ImguiOverlay.h"
#include "ItemCapture.h"

#include "SFSE/SFSE.h"

#include "BuildGate.h"
#include "ShopAudio.h"

namespace
{
	class BrowserTask : public SFSE::ITaskDelegate
	{
	public:
		void Run() override
		{
			SFE::Overlay::Install();
			SFE::ItemCapture::FramePoll();
			SFE::ShopBrowser::GetSingleton()->FrameUpdate();
			SFE::Overlay::SetDrawingEnabled(SFE::ShopBrowser::GetSingleton()->IsOpen());
		}
		void Destroy() override { delete this; }
	};

	void MenuCallback(RE::IMenu* a_menu)
	{
		SFE::ShopMenu::OnMenu(a_menu);
	}

	void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SFSE::MessagingInterface::kPostLoad) {
			REX::INFO("SFShopExplorer: kPostLoad received; renderer installation deferred to task");
		} else if (a_msg->type == SFSE::MessagingInterface::kPostPostLoad) {
			if (const auto menus = SFSE::GetMenuInterface())
				menus->Register(MenuCallback);
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

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
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
	REX::INFO("SFShopExplorer v{}: plugin loaded; existing-menu host mode", SFE::kVersion);
	return true;
}
