#include "ShopMenu.h"

#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

namespace SFE
{
	void ShopMenu::OnMenu(RE::IMenu* a_menu)
	{
		if (!a_menu)
			return;
		REX::INFO("SFShopExplorer: existing menu callback name={}", a_menu->GetName());
	}

	void ShopMenu::OpenExisting()
	{
		// The explorer currently uses the existing Console menu as its host. The
		// host is opened by the user with the normal tilde key; no custom menu or
		// engine constructor is invoked here.
		REX::INFO("SFShopExplorer: use the existing Console menu as host");
	}
}
