"""Favourites on the OSF page: one list, two front ends.

The overlay's star column and the page's star buttons write the same
`favourites` vector and the same SFShopExplorerFavourites.txt. These tests pin
the four places that has to stay true: the engine call the bridge makes, the
JSON fields the page reads, the filter flag that makes the plugin build the
starred list (rather than the page filtering one page of rows), and the page's
own click guards - a star inside a row must not select it or basket it.

They are source-level on purpose. The behaviour itself was exercised in a
browser against tests/mock_server.py (two stars across two categories, the
starred view listing both, unstarring inside it, and a star click leaving the
row unselected and the basket empty); there is no browser in the test run.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE_H = ROOT / "src" / "ShopExplorer.h"
ENGINE_CPP = ROOT / "src" / "ShopExplorer.cpp"
FRONTEND = ROOT / "src" / "OsfFrontend.cpp"
SHOP = ROOT.parent / "OSF Shop Explorer" / "SFSE" / "Plugins" / "OSFUI" / "views" / "devilzdad.osfshop" / "shop"
APP = SHOP / "app.js"
PAGE = SHOP / "index.html"
CSS = SHOP / "shop.css"
MOCK = ROOT.parent / "OSF Shop Explorer" / "tests" / "mock_server.py"


class OsfFavouritesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        for path in (ENGINE_H, ENGINE_CPP, FRONTEND, APP, PAGE, CSS, MOCK):
            if not path.is_file():
                raise unittest.SkipTest(f"missing {path}")
        cls.header = ENGINE_H.read_text(encoding="utf-8-sig")
        cls.engine = ENGINE_CPP.read_text(encoding="utf-8-sig")
        cls.frontend = FRONTEND.read_text(encoding="utf-8-sig")
        cls.app = APP.read_text(encoding="utf-8-sig")
        cls.page = PAGE.read_text(encoding="utf-8-sig")
        cls.css = CSS.read_text(encoding="utf-8-sig")
        cls.mock = MOCK.read_text(encoding="utf-8-sig")

    # -- engine ------------------------------------------------------------

    def test_the_page_toggles_the_overlay_list(self) -> None:
        self.assertIn("std::vector<std::uint32_t> OsfFavourites();", self.header)
        self.assertIn("bool OsfToggleFavourite(std::uint32_t a_formID, bool& a_nowFavourite);", self.header)
        self.assertIn("bool ShopBrowser::OsfToggleFavourite(", self.engine)
        body = self.engine[self.engine.index("bool ShopBrowser::OsfToggleFavourite("):]
        body = body[:body.index("std::vector<OsfBasketRow> ShopBrowser::OsfBasket()")]
        # Toggling goes through the overlay's own writer, so the persisted file,
        # the log line and the notify capture stay in one place.
        self.assertIn("ToggleFavourite(a_formID, FormDisplayName(form));", body)
        # Only something the catalog could sell may be starred.
        self.assertIn("IsCatalogRecordType(form->GetFormType())", body)
        # Both entry points read the file, whichever front end opens first.
        self.assertGreaterEqual(self.engine.count("LoadFavourites();"), 2)

    def test_a_refresh_clears_the_starred_view_with_the_rest_of_the_filter(self) -> None:
        body = self.engine[self.engine.index("void ShopBrowser::OsfRefresh()"):]
        body = body[:body.index("std::vector<OsfCategory> ShopBrowser::OsfCategoryTree()")]
        self.assertIn("favouritesOnly = false;", body)
        self.assertLess(body.index("favouritesOnly = false;"), body.index("BuildFormList();"))

    # -- bridge ------------------------------------------------------------

    def test_the_filter_flag_reaches_the_list_builder(self) -> None:
        # The duplicate toggle was appended after favouritesOnly: it was added to
        # an interface two front ends and a page share, so it goes on the end
        # rather than in the middle of the call every existing caller makes.
        self.assertIn("bool a_hideCrew, bool a_hideTurrets, bool a_hideBug, bool a_favouritesOnly,\n\t\t\tbool a_hideDuplicates);",
                      self.header)
        body = self.engine[self.engine.index("void ShopBrowser::OsfSetFilter("):]
        body = body[:body.index("std::size_t ShopBrowser::OsfPage(")]
        self.assertIn("favouritesOnly = a_favouritesOnly;", body)
        self.assertIn("hideDuplicateNames = a_hideDuplicates;", body)
        # The page's toggle must reach the plugin, not filter a local page.
        self.assertIn('if (a_in.Has("favouritesOnly")) next.favouritesOnly = a_in.Bool("favouritesOnly", next.favouritesOnly);',
                      self.frontend)
        self.assertIn("next.hideBug != g_filter.hideBug ||\n\t\t\t\tnext.hideDuplicates != g_filter.hideDuplicates ||\n\t\t\t\tnext.favouritesOnly != g_filter.favouritesOnly;",
                      self.frontend)
        # And it is echoed back, or the page's checkbox would reset on the next
        # catalog reply.
        self.assertIn('\\"hideDuplicates\\":{}', self.frontend)

    def test_every_answer_carries_the_star_list(self) -> None:
        # RowJson gained the per-row flag inside its format string.
        self.assertIn('\\"favourite\\":{}', self.frontend)
        self.assertIn('a_row.favourite ? "true" : "false"', self.frontend)
        state = self.frontend[self.frontend.index("std::string StateJson("):]
        state = state[:state.index("void EnsureSession()")]
        self.assertIn('out += ",\\"favourites\\":[";', state)
        self.assertIn("OsfFavourites()", state)
        # The row flag is set where the row is built, from the same list.
        self.assertIn("row.favourite = IsFavourite(form->GetFormID());", self.engine)

    def test_the_endpoint_is_registered(self) -> None:
        self.assertIn('constexpr const char* kEpFavourite = "devilzdad.osfshop.favourite";', self.frontend)
        self.assertIn("g_bridge.RegisterRequest(kEpFavourite, &FavouriteRequest, nullptr);", self.frontend)
        self.assertIn("void FavouriteRequest(const OSFUI::API::Request& a_request, void*) noexcept", self.frontend)

    # -- page --------------------------------------------------------------

    def test_the_page_star_cannot_steal_the_row(self) -> None:
        self.assertIn("button[data-fav]", self.app)
        self.assertIn("event.stopPropagation();", self.app)
        # A double-click on a star must not select and basket the row.
        dbl = self.app[self.app.index("el.rows.addEventListener('dblclick'"):]
        dbl = dbl[:dbl.index("el.pager.addEventListener")]
        self.assertIn("event.target.closest('button[data-fav]')", dbl)
        # The star column exists in markup and styling, and the header row
        # carries it too or the columns would not line up.
        self.assertIn('span class="starhdr"', self.page)
        grid = re.search(r"grid-template-columns:\s*26px minmax\((\d+)px, 1fr\)", self.css)
        self.assertIsNotNone(grid)
        # A 0 floor let a narrow window resolve the name column to nothing: the
        # name painted over the shelf column and the star was pushed out of the
        # scrollbox, where the click could not reach it.
        self.assertGreater(int(grid.group(1)), 0)

    def test_the_star_is_drawn_not_typed_here_too(self) -> None:
        # Same reason as the overlay: no Windows UI face carries U+2605.
        self.assertNotIn("\u2605", self.app)
        self.assertNotIn("\u2605", self.page)
        self.assertIn("STAR_PATH", self.app)
        self.assertIn("<svg", self.app)

    def test_the_toggle_sends_its_own_flag_last(self) -> None:
        body = self.app[self.app.index("async function loadRows("):]
        body = body[:body.index("function errText(")]
        # state.filter is the plugin's echo of the last filter; merging it after
        # the explicit keys silently reverted the toggle to the echoed value.
        self.assertIn("}, state.filter, { favouritesOnly: state.favouritesOnly });", body)

    def test_the_hide_filters_moved_behind_a_button(self) -> None:
        self.assertIn('id="hideBtn"', self.page)
        self.assertIn('id="favouritesOnly"', self.page)
        bar = self.page[self.page.index('<section class="shopbar">'):]
        bar = bar[:bar.index("</section>")]
        self.assertNotIn('id="hideTest"', bar)
        popup = self.page[self.page.index('id="hidePop"'):]
        self.assertIn('id="hideTest"', popup)
        self.assertIn("el.hideBtn.addEventListener('click', () => openPopup('hide'));", self.app)

    # -- harness -----------------------------------------------------------

    def test_the_mock_answers_the_star_endpoint(self) -> None:
        self.assertIn("devilzdad.osfshop.favourite", self.mock)
        self.assertIn("favourites: favourites.slice()", self.mock)
        self.assertIn("if (payload.favouritesOnly) {", self.mock)


if __name__ == "__main__":
    unittest.main()
