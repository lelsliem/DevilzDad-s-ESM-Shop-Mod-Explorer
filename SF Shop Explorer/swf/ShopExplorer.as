// ShopExplorer.as - AS2 Scaleform menu for the SF Shop Explorer plugin.
// The C++ IMenu owns all state and formats the on-screen text, then pushes
// it here via _root.render(text). This keeps all logic on the C++ side; the
// SWF is a pure display surface with a backdrop + title/list/status fields.

class ShopExplorer
{
	private static var _inst:ShopExplorer;

	public static function init(rootMC:MovieClip):Void
	{
		if (_inst != null)
			return;
		_inst = new ShopExplorer(rootMC);
	}

	// Entry point used by mtasc -main.
	public static function main():Void
	{
		init(_root);
	}

	public function ShopExplorer(root_mc:MovieClip)
	{
		// opaque backdrop
		var bg = root_mc.createEmptyMovieClip("bg", 1);
		bg.beginFill(0x12121A, 0.94);
		bg.moveTo(0, 0);
		bg.lineTo(1920, 0);
		bg.lineTo(1920, 1080);
		bg.lineTo(0, 1080);
		bg.lineTo(0, 0);
		bg.endFill();

		// title (top)
		var tT = root_mc.createTextField("titleTF", 2, 40, 24, 1840, 56);
		tT.autoSize = true;

		// main scrollable list area (center)
		var lT = root_mc.createTextField("listTF", 3, 40, 88, 1840, 900);
		lT.autoSize = false;
		lT.multiline = true;
		lT.wordWrap = false;

		// status line (bottom)
		var sT = root_mc.createTextField("statusTF", 4, 40, 1008, 1840, 48);
		sT.autoSize = true;

		root_mc.shop_titleTF = tT;
		root_mc.shop_listTF = lT;
		root_mc.shop_statusTF = sT;
	}

	// C++ calls _root.render(title, body, status).
	public function render(t:String, b:String, s:String):Void
	{
		var tT = _root.shop_titleTF;
		var lT = _root.shop_listTF;
		var sT = _root.shop_statusTF;
		if (tT != null)
			tT.text = (t == undefined) ? "" : t;
		if (lT != null)
			lT.text = (b == undefined) ? "" : b;
		if (sT != null)
			sT.text = (s == undefined) ? "" : s;
	}
}