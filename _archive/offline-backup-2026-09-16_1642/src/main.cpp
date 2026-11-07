#include "PCH.h"
#include "ShopExplorer.h"
#include "ShopMenu.h"
#include "ImguiOverlay.h"
#include "ItemCapture.h"

#include "SFSE/SFSE.h"

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
	const auto messaging = SFSE::GetMessagingInterface();
	if (!messaging) {
		REX::ERROR("SFShopExplorer: failed to get messaging interface");
		return false;
	}
	messaging->RegisterListener(MessageHandler);
	// Build tag: bump on EVERY deployed build so a stale game session is
	// provable from the log (DLLs load once at game start; copying a new
	// DLL to disk mid-session changes nothing until relaunch).
	REX::INFO("SFShopExplorer: plugin loaded; build 55-matchbatch; existing-menu host mode");
	return true;
}
