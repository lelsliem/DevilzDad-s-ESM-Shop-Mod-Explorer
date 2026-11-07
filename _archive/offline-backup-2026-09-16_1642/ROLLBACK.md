# Rollback — offline renderer (pre-native vendor window)
Archive: D:\Projects\Address Library\SF-Shop\_archive\offline-backup-2026-09-16_1642
MCM stash: stash@{0} (SF Starfield MCM)
Date: 2026-09-16 16:43

Offline path = own .sfmodel cache + CPU/GPU rasterizer (OfflineModel.cpp:167) + live NIF/.mesh (LiveModel.cpp:211) + BA2 v2/v3 + FindDiffuse/FindMatAlbedo. All 732 caches V-flipped: build_model_cache.py:133.

To roll back:
  1. Copy D:\Projects\Address Library\SF-Shop\_archive\offline-backup-2026-09-16_1642\src\  over SF Shop Explorer\src\
  2. git -C 'SF Starfield MCM' stash pop
  3. xmake build SFShopExplorer ; deploy to B:\MO2\Starfield\mods\Shop Test Render\

Native attempt lives on: ShopExplorer.cpp vendor-sized PREVIEW child + BarterMenu LoadModel dispatch (crash-allowed, Test profile isolated).
