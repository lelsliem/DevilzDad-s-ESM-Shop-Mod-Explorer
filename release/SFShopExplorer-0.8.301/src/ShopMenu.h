#pragma once

#include "RE/I/IMenu.h"

namespace SFE
{
	class ShopMenu
	{
	public:
		static void OnMenu(RE::IMenu* a_menu);
		static void OpenExisting();
	};
}
