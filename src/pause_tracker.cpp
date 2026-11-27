// Port: the pause menu's progress tracker. At the menu's bottom left, under
// the HUD's counters: an area's name, between the L and R buttons, and its
// Shine Sprites and blue coins as "got / all there are". It opens on the area
// Mario is in; L and R step through the others he has been to.
//
// Everything it draws is the game's, as the save state menu's is
// (savestate_menu.cpp): the area's name in the file select's lettering
// (port_savestate_world_art), the system font's L and R glyphs (swapped for
// the controller in use, patch zzz-prompts-01), and the HUD's art from
// game_6: the brushstroke behind its counters, the Shine and blue coin icons,
// the counter digits, and the slash of the HUD's "3/8" counters. A count
// that is complete has the HUD counters' green brushstroke.
//
// The areas and their counts are the game's own: the Shine tables the map
// screen (TGuide) and the file select count with, the Airstrip's two Shines
// (0x56, 0x58) as those two count them, and Delfino Plaza's the rest of the
// 120. Blue coins are TFlagManager's 50 per area, the Airstrip's being
// counted with the Plaza's as the game files them.
#include <GC2D/MessageUtil.hpp>
#include <GC2D/ShineTable.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/J2D/J2DPane.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <stdio.h>
#include <string.h>

#include "port_pause_tracker.h"
#include "port_savestate.h"

// sms_gx widescreen: with the HUD at the screen edges (SMS_WIDESCREEN_HUD=edges)
// the tracker goes to the left edge with the counters above it
extern "C" void GXPC_HudPieceBegin(float x1, float x2);
extern "C" void GXPC_HudPieceEnd(void);
// custom levels (ext/levels): the level Mario is in, its own Shines, and the save file's flags
// kept aside while he is there (weak: absent without the extension)
extern "C" int port_levels_in(void) __attribute__((weak));
extern "C" const char* port_levels_name(void) __attribute__((weak));
extern "C" void port_levels_counts(int* shines, int* allShines, int* blues, int* allBlues) __attribute__((weak));
extern "C" int port_levels_file_flag(unsigned flag) __attribute__((weak));
extern "C" int port_levels_special_total(void) __attribute__((weak));

namespace {

const int kWorlds = 10; // SMS_getShineStage's: 0 the Airstrip ... 9 Corona Mountain
const int kLevel  = kWorlds; // the custom level Mario is in (ext/levels), after them
const int kPages  = kWorlds + 1;

bool inLevel() { return port_levels_in && port_levels_in(); }
// a save file's flag, the file's own when a custom level has put its own in their place
bool fileFlag(unsigned flag)
{
	return inLevel() && port_levels_file_flag ? port_levels_file_flag(flag) != 0 : TFlagManager::getInstance()->getFlag(flag) != 0;
}

// layout, in the pause menu's 2D units (the 4:3 frame, 640 wide): clear of
// the HUD's counters above and the stage name's panel to the right
const f32 kLeft     = 24.0f;
const f32 kNameY    = 312.0f; // the rows' centres
const f32 kShineY   = 345.0f;
const f32 kBlueY    = 378.0f;
const f32 kRowW     = 176.0f; // the name's row, L to R
const f32 kNameH    = 22.0f;
const f32 kButton   = 24.0f;  // the L and R glyphs
const f32 kIconH    = 29.0f;
const f32 kDigitH   = 23.0f;
const f32 kBrushW   = 186.0f;
const f32 kBrushH   = 29.0f;

const u32 kWhite    = 0xFFFFFFFF;
const u32 kBrush    = 0x6E28A0FF; // the pause menu's stage panel purple
const u32 kComplete = 0x28AA28FF; // the HUD counters' green

struct Tally {
	int shines, allShines;
	int blueCoins, allBlueCoins;
};

int sWorld = -1; // the area shown, -1 until the menu appears
int sHome;       // the area Mario is in
Tally sTally[kPages];

JUTTexture sBrushArt, sShineArt, sBlueCoinArt, sCrossArt, sSlashArt, sDigitArt[10];
bool sHaveArt;

bool loadArt(JUTTexture& tex, const char* name)
{
	char path[64];
	snprintf(path, sizeof path, "/game_6/timg/%s.bti", name);
	const ResTIMG* timg = (const ResTIMG*)JKRGetResource(path);
	if (!timg)
		return false;
	tex.storeTIMG(timg);
	return true;
}

// game_6 comes with every stage, so the art is looked up again each time.
bool loadAllArt()
{
	bool ok = loadArt(sBrushArt, "coin_back") && loadArt(sShineArt, "shine_icon")
	          && loadArt(sBlueCoinArt, "bluecoin_icon") && loadArt(sCrossArt, "coin_number_x")
	          && loadArt(sSlashArt, "balloon_slash");
	for (int i = 0; i < 10 && ok; i++) {
		char name[32];
		snprintf(name, sizeof name, "coin_number_%d", i);
		ok = loadArt(sDigitArt[i], name);
	}
	return ok;
}

void count()
{
	memset(sTally, 0, sizeof sTally);

	// whose each of the 120 Shines is: the areas' eight episodes and three
	// others, the Airstrip's two, and the Plaza the rest
	u8 owner[120];
	memset(owner, 1, sizeof owner);
	for (int w = 2; w <= 8; w++) {
		for (int i = 0; i < 8; i++)
			owner[scShineConvTable[w][i]] = w;
		for (int i = 0; i < 3; i++)
			owner[scEtcShineConvTable[w][i]] = w;
	}
	owner[0x56] = owner[0x58] = 0;
	for (int id = 0; id < 120; id++) {
		Tally& t = sTally[owner[id]];
		t.allShines++;
		if (fileFlag(0x10000 + id))
			t.shines++;
	}

	// blue coins: 30 in each area, 20 in the Plaza (the Airstrip's one with
	// them), 10 on Corona Mountain
	for (int w = 1; w < kWorlds; w++) {
		Tally& t       = sTally[w];
		t.allBlueCoins = w == 1 ? 20 : w == 9 ? 10 : 30;
		for (int i = 0; i < 50; i++)
			if (fileFlag(0x10078 + (w - 1) * 50 + i)) // TFlagManager::getBlueCoinFlag's
				t.blueCoins++;
	}
	// the custom level's own
	if (inLevel() && port_levels_counts) {
		Tally& t = sTally[kLevel];
		port_levels_counts(&t.shines, &t.allShines, &t.blueCoins, &t.allBlueCoins);
	}
}

// The areas L and R reach: the one Mario is in, the Plaza, and those he has
// been to (the map screen's test, a flag each stage sets as it loads).
bool reachable(int world)
{
	if (world == kLevel)
		return inLevel();
	return world == sHome || world == 1 || TFlagManager::getInstance()->getBool(0x103A5 + world);
}

void step(int by)
{
	int w = sWorld;
	for (int i = 0; i < kPages; i++) {
		w = (w + by + kPages) % kPages;
		if (reachable(w))
			break;
	}
	if (w == sWorld)
		return;
	sWorld = w;
	SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CURSOR, 0, nullptr, 0);
}

// --- drawing: quads as JUTResFont draws its glyphs (savestate_menu.cpp) -----

void setupTextured()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

// A w x h quad of the texture loaded in GX_TEXMAP0, its left edge at x and
// centred on y.
void quad(f32 x, f32 y, f32 w, f32 h, u32 colour, u8 alpha)
{
	const u8 a = (u8)(((colour & 0xFF) * alpha) / 255);
	static const f32 kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
	setupTextured();
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	for (int i = 0; i < 4; i++) {
		GXPosition3f32(x + kU[i] * w, y - h / 2 + kV[i] * h, 0.0f);
		GXColor4u8(colour >> 24, colour >> 16, colour >> 8, a);
		GXTexCoord2f32(kU[i], kV[i]);
	}
	GXEnd();
}

// The art h tall at its own proportions; its width.
f32 art(JUTTexture& tex, f32 x, f32 y, f32 h, u32 colour, u8 alpha)
{
	const ResTIMG* t = tex.getTexInfo();
	const f32 w      = t ? h * t->width / t->height : h;
	tex.load(GX_TEXMAP0);
	quad(x, y, w, h, colour, alpha);
	return w;
}

// "x16/41" in the HUD's counter digits; its width.
f32 counter(int got, int all, f32 x, f32 y, u8 alpha)
{
	const f32 step = kDigitH * 0.86f; // the digits' spacing, as the HUD's
	char text[16];
	snprintf(text, sizeof text, "%d/%d", got, all);
	const f32 left = x;
	art(sCrossArt, x, y, kDigitH, kWhite, alpha);
	x += step;
	for (const char* p = text; *p; p++) {
		if (*p == '/') {
			art(sSlashArt, x - step * 0.12f, y, kDigitH * 1.05f, kWhite, alpha);
			x += step * 0.75f;
		} else {
			art(sDigitArt[*p - '0'], x, y, kDigitH, kWhite, alpha);
			x += step;
		}
	}
	return x - left;
}

// One of the HUD's counters on its brushstroke.
void counterRow(JUTTexture& icon, int got, int all, f32 y, u8 alpha)
{
	sBrushArt.load(GX_TEXMAP0);
	quad(kLeft - 10.0f, y + 1.0f, kBrushW, kBrushH, got >= all ? kComplete : kBrush, (u8)(alpha * 0.85f));
	const f32 iconW = art(icon, kLeft, y, kIconH, kWhite, alpha);
	counter(got, all, kLeft + iconW + 2.0f, y + 1.0f, alpha);
}

f32 textWidth(const char* s, f32 size)
{
	JUTFont* font = gpSystemFont;
	f32 w         = 0.0f;
	for (; *s; s++)
		w += font->getWidth((u8)*s) * (size / font->getWidth());
	return w;
}

// The area's name between the L and R buttons, which sit at the row's ends
// (as wide as the counters' brushstrokes, clear of the menu's options): the
// file select's lettering, else the pause menu's own name for it in the
// system font, outlined as the lettering is and narrowed where it would not fit.
void nameRow(u8 alpha)
{
	JUTFont* font    = gpSystemFont;
	const bool more  = sWorld != sHome || reachable((sWorld + 1) % kPages)
	                  || reachable((sWorld + kPages - 1) % kPages);
	const f32 gap    = 4.0f;
	const f32 room   = more ? kRowW - 2.0f * (kButton + gap) : kRowW;
	const f32 middle = kLeft + kRowW / 2;
	int tw = 0, th = 0, artW = 0;
	const void* data = sWorld == kLevel ? 0 : port_savestate_world_art(sWorld, 0, &tw, &th, &artW);
	const char* levelName = sWorld == kLevel && port_levels_name ? port_levels_name() : 0;
	if (data) {
		f32 h = kNameH;
		if (artW * h / th > room)
			h = room * th / artW;
		GXTexObj obj;
		GXInitTexObj(&obj, (void*)data, tw, th, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		GXLoadTexObj(&obj, GX_TEXMAP0);
		quad(middle - artW * h / th / 2, kNameY, tw * h / th, h, kWhite, alpha);
	} else if (const char* name
	           = levelName ? levelName : SMSGetMessageData(JKRFileLoader::getGlbResource("/common/2d/stagename.bmg"), sWorld)) {
		// as tall as the lettering, narrowed to fit (to half width), then smaller; white with a
		// dark outline and a bolder stroke, as the lettering is drawn
		f32 h = kNameH, w = h;
		if (textWidth(name, w) > room)
			w *= room / textWidth(name, w);
		if (w < h * 0.5f) {
			h = w * 2.0f;
		}
		const f32 x = middle - textWidth(name, w) / 2, y = kNameY + h * 0.4f;
		const int len = (int)strlen(name);
		const f32 o = h > 14.0f ? 1.6f : 1.0f;
		font->setGX();
		font->setCharColor(JUtility::TColor(0, 0, 0, alpha / 2));
		font->drawString_size_scale(x + o + 1.5f, y + o + 1.5f, w, h, name, len, false);
		font->setCharColor(JUtility::TColor(24, 16, 40, alpha));
		static const f32 kRing[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
		for (int i = 0; i < 8; i++)
			font->drawString_size_scale(x + kRing[i][0] * o, y + kRing[i][1] * o, w, h, name, len, false);
		font->setCharColor(JUtility::TColor(255, 255, 255, alpha));
		font->drawString_size_scale(x, y, w, h, name, len, false);
		font->drawString_size_scale(x + 0.6f, y, w, h, name, len, false);
	}
	if (more) {
		font->setGX();
		font->setCharColor(JUtility::TColor(255, 255, 255, alpha));
		font->drawChar_scale(kLeft, kNameY + kButton / 2 - 3.0f, kButton, kButton, '<', false);
		font->drawChar_scale(kLeft + kRowW - kButton, kNameY + kButton / 2 - 3.0f, kButton, kButton, '>', false);
	}
}

} // namespace

void PauseTrackerReset()
{
	sWorld = -1;
	if (!port_pause_tracker || !gpMarDirector)
		return;
	sHaveArt = loadAllArt();
	sHome    = SMS_getShineStage(gpMarDirector->mMap);
	if (sHome >= kWorlds)
		sHome = 1; // somewhere outside retail's areas: start at the Plaza
	if (inLevel())
		sHome = kLevel; // a custom level: its own page
	count();
	sWorld = sHome;
}

void PauseTrackerMove(TMarioGamePad* pad)
{
	if (sWorld < 0 || !pad)
		return;
	if (pad->mButton.mTrigger & PAD_TRIGGER_L)
		step(-1);
	else if (pad->mButton.mTrigger & PAD_TRIGGER_R)
		step(1);
}

void PauseTrackerDraw(const JDrama::TRect& viewport, u8 alpha)
{
	if (sWorld < 0 || !sHaveArt || !alpha)
		return;
	J2DOrthoGraph graph(viewport);
	graph.setPort(); // the menu's screen left its own projection
	graph.setup2D();
	GXPC_HudPieceBegin(kLeft - 10.0f, kLeft - 10.0f + kBrushW);
	nameRow(alpha);
	const Tally& t = sTally[sWorld];
	f32 y          = kShineY;
	if (t.allShines) {
		counterRow(sShineArt, t.shines, t.allShines, y, alpha);
		y = kBlueY;
	}
	if (t.allBlueCoins)
		counterRow(sBlueCoinArt, t.blueCoins, t.allBlueCoins, y, alpha);
	GXPC_HudPieceEnd();
}

// --- the HUD's special Shines ---------------------------------------------------
//
// The game's own Shine counter drawn again a row lower (the panes as they were just drawn, moved
// down), with a silver Shine on a blue brushstroke and the special Shines' count, for a while after
// one is got (port_levels_special_age_ms), fading in and out.

extern "C" int port_levels_special_got(void) __attribute__((weak));
extern "C" void port_log(const char* fmt, ...);
extern "C" int port_special_shines; // SMS_SPECIAL_SHINES (special_shines in settings.txt, platform)

namespace {

const int kShowMs = 6000, kFadeMs = 400;
const u32 kSpecialShine = 0x3C78DCFF; // added over the Shine's own colours: a pale, silvery Shine
const u32 kSpecialBrush = 0x3C8CEBFF; // the brushstroke's blue


} // namespace

namespace {

// The Shine row's pieces (bar, Shine, cross, three digits) as they were, while drawn as the special
// Shines' row: the Shine silver, the count the special Shines'.
struct RowWas {
	bool visible;
	u8 alpha;
	const ResTIMG* tex0;
	JUtility::TColor white, black;
};

void silverRow(J2DPane* const* row, int total, int fade, RowWas* was)
{
	char digits[4];
	snprintf(digits, sizeof digits, total >= 100 ? "%03d" : "%02d", total > 999 ? 999 : total);
	for (int i = 0; i < 6; i++) {
		J2DPane* p = row[i];
		if (!p)
			continue;
		was[i].visible = p->mVisible;
		was[i].alpha   = p->mAlpha;
		was[i].tex0    = 0;
		p->mAlpha      = (u8)(p->mAlpha * fade / 255);
		J2DPicture* pic = (J2DPicture*)p; // the Shine and the digits are pictures
		if (i == 1) {
			was[i].white = pic->mWhite;
			was[i].black = pic->mBlack;
			pic->mBlack = JUtility::TColor(40, 50, 80, 0); // a silver Shine (its grey, drawSilverShine)
			pic->mWhite = JUtility::TColor(255, 255, 255, 255);
		} else if (i >= 3) {
			was[i].tex0 = pic->getTexture(0) ? pic->getTexture(0)->getTexInfo() : 0;
			const int d = i - 3;
			p->mVisible = digits[d] != 0 && d < 3;
			if (p->mVisible)
				pic->changeTexture(sDigitArt[digits[d] - '0'].getTexInfo(), 0);
		}
	}
}

void restoreRow(J2DPane* const* row, const RowWas* was)
{
	for (int i = 0; i < 6; i++) {
		J2DPane* p = row[i];
		if (!p)
			continue;
		J2DPicture* pic = (J2DPicture*)p;
		p->mVisible = was[i].visible;
		p->mAlpha   = was[i].alpha;
		if (i == 1) {
			pic->mWhite = was[i].white;
			pic->mBlack = was[i].black;
		} else if (i >= 3 && was[i].tex0) {
			pic->changeTexture(was[i].tex0, 0);
		}
	}
}

// the Shine in grey: the picture's green in all three colours, with the silver black and white above
void drawSilverShine(J2DPane* shine, int dy, J2DOrthoGraph& graph)
{
	GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
	GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP2);
	shine->draw(0, dy, &graph, false);
	GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
}

// In a custom level the HUD's own Shine row is the special Shines' (HudSpecialShinesBefore): its
// count theirs and its Shine silver, the game's Shine hidden from its draw and drawn silver after.
RowWas s_main[6];
bool s_mainSilver;

bool artReady()
{
	static int s_art = -1;
	if (s_art < 0)
		s_art = loadAllArt() ? 1 : 0;
	return s_art != 0;
}

} // namespace

void HudSpecialShinesBefore(J2DPane* const* row, int count)
{
	s_mainSilver = false;
	if (!port_special_shines || !port_levels_special_total || count != 6 || !inLevel() || !row[0] || !row[1])
		return;
	// (the pause menu shows both rows: the save file's and, under them, the special Shines')
	if (gpMarDirector && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU)
		return;
	if (!artReady())
		return;
	silverRow(row, port_levels_special_total(), 255, s_main);
	row[1]->mVisible = false;
	s_mainSilver = true;
}

void HudSpecialShinesDraw(J2DOrthoGraph& graph, J2DPane* const* row, int count, const J2DPane* blueBar, const J2DPane* coinBar)
{
	// row: the Shine counter's bar, icon, cross and three digits
	if (s_mainSilver) {
		s_mainSilver = false;
		row[1]->mVisible = s_main[1].visible;
		// only where the game drew its row this frame: every pane above the Shine shown (the HUD hides
		// its counters by hiding a pane above them, whose place and alpha then stay as last drawn)
		bool shown = row[1]->mVisible && row[0]->isVisible() && row[0]->mColorAlpha;
		for (JSUTree<J2DPane>* t = row[1]->getPaneTree()->getParent(); shown && t; t = t->getParent())
			if (J2DPane* up = t->getObject())
				shown = up->mVisible;
		if (shown) {
			graph.setup2D();
			drawSilverShine(row[1], 0, graph);
		}
		restoreRow(row, s_main);
		return; // the row is theirs already
	}
	if (!port_special_shines || !port_levels_special_total || !port_levels_special_got || count != 6 || !blueBar || !coinBar)
		return;
	J2DPane* bar = row[0];
	if (!bar || !bar->isVisible())
		return;
	// shown from when the HUD is there again after a special Shine was got, for a while
	static int s_seen = 0;
	static OSTime s_from = 0;
	const int got = port_levels_special_got();
	if (got != s_seen && bar->mColorAlpha >= 200) {
		s_seen = got;
		s_from = OSGetTime();
	}
	// always in the pause menu; else for a while
	const bool paused = gpMarDirector && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU;
	if (!bar->mColorAlpha || (!paused && !s_from))
		return;
	const OSTime ms = paused ? 0 : OSTicksToMilliseconds(OSGetTime() - s_from);
	if (ms >= kShowMs)
		return;
	const int age = (int)ms;
	if (!artReady())
		return;
	const int total = port_levels_special_total();
	static bool s_logged;
	if (!s_logged) {
		s_logged = true;
		port_log("[levels] special Shines on the HUD: %d\n", total);
	}
	// fading in and out
	int fade = 255;
	if (!paused && age < kFadeMs)
		fade = age * 255 / kFadeMs;
	else if (!paused && age > kShowMs - kFadeMs)
		fade = (kShowMs - age) * 255 / kFadeMs;

	// a row lower than the lowest of the blue coin and coin rows, as far apart as they are
	const f32 blueY = (f32)blueBar->getGlobalBounds().y1, coinY = (f32)coinBar->getGlobalBounds().y1;
	const f32 low = coinY > blueY ? coinY : blueY, step = coinY > blueY ? coinY - blueY : blueY - coinY;
	const int dy  = (int)(low + step - (f32)bar->getGlobalBounds().y1);

	// the game's own Shine row drawn again, lower: each piece as the game draws it (its picture, turn
	// and colours), without the clipping to its own row a pane's draw has in the HUD
	RowWas was[6];
	silverRow(row, total, fade, was);
	graph.setup2D();
	// the whole row (brushstroke, Shine, cross and count) a little lower than a row's step, clear of the
	// row above, the brushstroke behind the cross and count as the other rows have it
	const int lower = (int)(step * 0.18f);
	for (int i = 0; i < 6; i++) {
		if (!row[i])
			continue;
		bool under = false; // drawn with a parent of it among the six already
		for (int j = 0; j < 6 && !under; j++)
			for (JSUTree<J2DPane>* t = row[i]->getPaneTree()->getParent(); j != i && row[j] && t; t = t->getParent())
				if (t->getObject() == row[j])
					under = true;
		if (under)
			continue;
		if (i == 1)
			drawSilverShine(row[i], dy + lower, graph);
		else
			row[i]->draw(0, dy + lower, &graph, false);
	}
	restoreRow(row, was);
}
