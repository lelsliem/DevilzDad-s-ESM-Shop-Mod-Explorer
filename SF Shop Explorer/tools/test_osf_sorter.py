"""The OSF page's column sorter: one order, agreed by four files.

The page asks for one window of a hundred rows, so it cannot sort the catalog
itself - page 2 would come back unsorted. The order is therefore built in the
plugin (`ShopBrowser::OsfPage`), and these tests pin the four places that have
to agree on it:

  1. the page sends the column and the direction with every row request, and
     the click rule is the overlay's (a new column sorts ascending, clicking the
     active one flips);
  2. the engine takes those two values, clamps the column to its six meanings,
     and caches the order against the list rather than rebuilding it per page;
  3. the six headings are the only sortable cells, so the star column cannot
     claim a column number that means something else;
  4. tests/mock_server.py sorts with the same numbering, so the harness cannot
     disagree with the plugin about what "column 4" is.

The behaviour itself was exercised in a browser against the harness: Weight
gives 0, 0, 0.1 ... 12.4 and then the reverse, Name gives an alphabetical list,
one heading is highlighted at a time, and the page returns to page 0.
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


class OsfSorterTests(unittest.TestCase):
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

    def test_the_window_takes_a_sort(self) -> None:
        self.assertIn("std::size_t OsfPage(const std::string& a_search, std::size_t a_offset, std::size_t a_limit,", self.header)
        self.assertIn("int a_sortColumn, bool a_sortAscending, std::vector<OsfItemRow>& a_out);", self.header)
        self.assertIn("int a_sortColumn, const bool a_sortAscending", self.engine)

    def test_the_order_is_cached_not_rebuilt_per_page(self) -> None:
        # Keyed on the column, the direction and formsStamp (only BuildFormList
        # bumps it): a page turn reuses the order, a header click does not.
        self.assertIn("osfOrderStamp != formsStamp", self.engine)
        self.assertIn("osfOrderColumn != a_sortColumn", self.engine)
        self.assertIn("osfOrderAscending != a_sortAscending", self.engine)
        self.assertIn("osfOrderStamp = formsStamp;", self.engine)

    def test_the_sort_covers_the_whole_list(self) -> None:
        body = self.engine[self.engine.index("std::size_t ShopBrowser::OsfPage("):]
        body = body[:body.index("std::vector<std::uint32_t> ShopBrowser::OsfFavourites()")]
        # The window is cut after the order is applied, so offset/limit walk the
        # sorted list rather than the catalog order.
        self.assertIn("osfOrder.size() != forms.size()", body)
        self.assertIn("osfOrder[r]", body)
        # Total order: ties fall back to the FormID, as the overlay's sort does.
        self.assertIn("std::stable_sort(osfOrder.begin(), osfOrder.end()", body)
        self.assertIn("cmp = ia < ib ? -1 : ia > ib ? 1 : 0;", body)
        # NAME sorts on the readable key, not raw bytes (padded names).
        self.assertIn("NameSortKey(FormDisplayName(form))", body)

    def test_the_bridge_clamps_the_column(self) -> None:
        self.assertIn('in.Int("sort", -1), -1, 5)', self.frontend)
        self.assertIn('in.Bool("asc", true)', self.frontend)
        # Catalogue order is still available and is what the page starts on.
        self.assertIn('OsfPage("", 0, 0, -1, true, none)', self.frontend)

    # -- page --------------------------------------------------------------

    def test_the_six_headings_are_the_sorter(self) -> None:
        columns = re.findall(r'data-sort="(\d)"', self.page)
        self.assertEqual(columns, ["0", "1", "2", "3", "4", "5"])
        # The star is not a column: it has no number to send.
        self.assertNotIn('data-sort=', self.page[self.page.index('class="starhdr"'):self.page.index('data-sort="0"')])
        self.assertIn(".tblhdr [data-sort] { cursor: pointer;", self.css)
        self.assertIn(".tblhdr [data-sort].sorted { color: var(--gold); }", self.css)

    def test_every_row_request_carries_the_sort(self) -> None:
        self.assertIn("sort: state.sort.column,", self.app)
        self.assertIn("asc: state.sort.ascending,", self.app)
        self.assertIn("sort: { column: -1, ascending: true }", self.app)

    def test_a_click_flips_the_active_column_only(self) -> None:
        body = self.app[self.app.index("el.tblhdr.addEventListener('click'"):]
        body = body[:body.index("el.rows.addEventListener('click'")]
        self.assertIn("if (state.sort.column === column) {", body)
        self.assertIn("state.sort.ascending = !state.sort.ascending;", body)
        self.assertIn("state.sort.column = column;", body)
        self.assertIn("state.sort.ascending = true;", body)
        # A different order means a different first page.
        self.assertIn("loadRows(true);", body)
        # The arrow is drawn into the label, so the heading keeps its width.
        self.assertIn("' \\u25b2'", self.app)
        self.assertIn("' \\u25bc'", self.app)

    def test_the_sort_is_remembered_between_opens(self) -> None:
        # The page holds one window of rows, so it cannot keep this itself: the
        # plugin writes it to the INI the way the overlay keeps [shop]
        # sortColumn, hands it back with the catalog, and the page adopts it on
        # the first reply only - a later re-sync must not undo a click.
        self.assertIn("OsfStoredSortColumn()", self.engine)
        self.assertIn("OsfStoredSortAscending()", self.engine)
        self.assertIn("OsfStoreSort(a_sortColumn, a_sortAscending);", self.engine)
        self.assertIn('L"osfSortColumn"', self.engine)
        self.assertIn('L"osfSortAscending"', self.engine)
        self.assertIn("json += SortJson();", self.frontend)
        # Two writers, same values: OsfPage stores the order it actually built,
        # and the bridge stores the request that asked for it (through SaveSort,
        # which skips a redundant INI write on a page turn).
        self.assertIn("ShopBrowser::OsfStoreSort(a_column, a_ascending);", self.frontend)
        self.assertIn("SaveSort(sort, ascending);", self.frontend)
        self.assertIn("if (!state.sortRestored) {", self.app)
        self.assertIn("state.sortRestored = true;", self.app)
        self.assertIn("renderSort();", self.app)
        # The harness keeps it too, or reloading the page would prove nothing.
        self.assertIn("mock.osfshop.sort", self.mock)

    # -- harness -----------------------------------------------------------

    def test_the_harness_sorts_the_same_way(self) -> None:
        self.assertIn("if (sort === 0) return String(row.name || '').trim().toLowerCase();", self.mock)
        self.assertIn("if (sort === 1) return String(row.sub || '').toLowerCase();", self.mock)
        self.assertIn("if (sort === 3) return Number(row.value || 0);", self.mock)
        self.assertIn("if (sort === 4) return Number(row.weight || 0);", self.mock)
        # Same tie-break, same direction rule as the plugin.
        self.assertIn("return asc ? cmp : -cmp;", self.mock)


if __name__ == "__main__":
    unittest.main(verbosity=2)
