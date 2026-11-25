// Hiding parts of the game's own HUD (port_ext.h, "the HUD"): game code, with
// the game's flags and headers.
//
// The parts are left out of the HUD's *draw* only. port_hud_draw runs just
// before TGCConsole2::perform's draw pass (zzzzz-ext-23-hud-draw) and hides
// each part's panes, remembering whether each was shown; just after, it puts
// every one back as it was. So the HUD's own logic (its slide-in animations,
// their sounds, FLUDD's messages, which wait for the tank to be up) never sees
// a part hidden, and showing a part again is immediate and exactly as the game
// has it. The counters' sparkles are particles drawn in another pass: their
// emitters are kept from drawing (STATUS_STOP_DRAW, which the HUD itself never
// uses) while their counter is hidden. The bursts a counter's digits give off as
// they change are not made while it is hidden (zzzzz-ext-23-hud-draw, port_hud_hidden).
#include "port_ext.h"
#include "port_hud_settings.h"

#include <GC2D/BoundPane.hpp>
#include <GC2D/ExPane.hpp>
#include <GC2D/GCConsole2.hpp>
#include <JSystem/J2D/J2DPane.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/J2D/J2DScreen.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/ResTIMG.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <string.h>
#include <System/MarDirector.hpp>

namespace {

unsigned s_hidden[PORT_HUD_OWNERS];
unsigned s_mask; // the union of s_hidden
unsigned s_drawn = PORT_HUD_EVERYTHING; // port_hud_set_drawn

// Whether the pause menu is up (the settings menu opens in it). gpMarDirector is null
// outside a stage.
bool pauseMenu()
{
	return gpMarDirector && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU;
}

// Every owner's parts, and the settings menu's HUD page's (port_hud_settings.h), asked fresh:
// it changes from the menu and the HUD_TOGGLE key, and is read from its file on first use.
// The page's are shown while the pause menu is up, so the counters can be read there, and
// hidden again as it closes; a mod's or a server's stay hidden, as they asked.
unsigned hiddenNow()
{
	const unsigned settings = port_hud_settings_mask();
	return s_mask | (settings && !pauseMenu() ? settings : 0) | (~s_drawn & PORT_HUD_ALL);
}

// The panes hidden for this draw and how each was, to put back after it.
enum { kMaxPanes = 64 };
J2DPane* s_pane[kMaxPanes];
bool s_was[kMaxPanes];
int s_count;
u8 s_tankAppearing, s_tankUp; // the HUD's unk45, unk46, while drawing
bool s_tankSaved;

void hideForDraw(J2DPane* pane)
{
	if (!pane || s_count >= kMaxPanes)
		return;
	s_pane[s_count] = pane;
	s_was[s_count]  = pane->isVisible();
	s_count++;
	pane->hide();
}

void hideForDraw(TExPane* pane)
{
	if (pane)
		hideForDraw(pane->getPane());
}

void hideForDraw(TBoundPane* pane)
{
	if (pane)
		hideForDraw(pane->getPane());
}

// A counter's sparkle: kept from drawing while the counter is hidden. The
// emitter keeps running, so it is as the game has it when shown again.
void sparkle(JPABaseEmitter* emitter, bool hidden)
{
	if (!emitter)
		return;
	if (hidden)
		emitter->stopDrawParticle();
	else
		emitter->playDrawParticle();
}

// Pictures of the HUD's own given others (port_hud_picture): by pane tag, each a texture of RGBA8 of its
// own in the system heap (it outlives stages), laid on the pane before each draw. A stage's HUD is made
// afresh with the game's textures, so each new one takes them again.
enum { kMaxPictures = 16 };
struct HudPicture {
	u32 tag;
	ResTIMG* image; // header, then the image
};
HudPicture s_pictures[kMaxPictures];

void pictures(TGCConsole2* c)
{
	if (!c->unkB0)
		return;
	for (int i = 0; i < kMaxPictures; i++) {
		const HudPicture& p = s_pictures[i];
		if (!p.image)
			continue;
		J2DPane* pane = c->unkB0->search(p.tag);
		if (!pane || pane->getTag() != 0x12) // a J2DPicture
			continue;
		J2DPicture* pic = (J2DPicture*)pane;
		if (pic->getTexture(0) && pic->getTexture(0)->getTexInfo() != p.image)
			pic->changeTexture(p.image, 0);
	}
}

void before(TGCConsole2* c)
{
	pictures(c);
	s_count     = 0;
	s_tankSaved = false;
	const unsigned mask = hiddenNow(); // every owner's, and the settings page's

	// every draw of its counter, hidden or not: a sparkle shown again when
	// its counter is
	if (s_drawn & PORT_HUD_COINS)
		sparkle(c->unk124, mask & PORT_HUD_COINS);
	if (s_drawn & PORT_HUD_SHINES)
		sparkle(c->unk144, mask & PORT_HUD_SHINES);
	if (s_drawn & PORT_HUD_BLUE_COINS)
		sparkle(c->unk164, mask & PORT_HUD_BLUE_COINS);
	if (!(s_drawn & PORT_HUD_REST)) {
		// every group of the layout that is no part's: the timer, the balloon
		// counter, the message ticker, the help balloon, and whatever a code
		// mod's layout adds and looks after on the game's HUD (Eclipse's
		// Shadow Mario brush card); drawConsole leaves out the radar and the
		// mod's draw callbacks (port_hud_drawing_rest)
		const J2DPane* parts[] = { c->unk108->getPane(), c->unk140->getPane(), c->unk160->getPane(),
			                   c->unk1C4->getPane(), c->unk2F8->getPane(), c->unk3A8->getPane(),
			                   c->unk428->getPane(), c->unk314[0], c->unk314[1], c->unk324 };
		if (J2DPane* root = c->unkB0->search('ROOT'))
			for (JSUTree<J2DPane>* t = root->getFirstChild(); t != root->getEndChild(); t = t->getNextChild()) {
				J2DPane* group = t->getObject();
				bool part      = false;
				for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++)
					part = part || group == parts[i];
				if (!part)
					hideForDraw(group);
			}
	}
	if (!mask)
		return;

	if (mask & PORT_HUD_COINS)
		hideForDraw(c->unk108);
	if (mask & PORT_HUD_SHINES)
		hideForDraw(c->unk140);
	if (mask & PORT_HUD_BLUE_COINS)
		hideForDraw(c->unk160);
	if (mask & PORT_HUD_HEALTH)
		hideForDraw(c->unk1C4);
	if (mask & PORT_HUD_LIVES)
		hideForDraw(c->unk3A8);
	if (mask & PORT_HUD_RED_COINS)
		hideForDraw(c->unk428);
	if (mask & PORT_HUD_WATER) {
		// The tank and every pane of the gauge, whichever group the layout
		// puts them in: the back, its text, the nozzle icons and their
		// labels, the level and the juice icons (Yoshi).
		hideForDraw(c->unk2F8);
		hideForDraw(c->unk26C);
		hideForDraw(c->unk270);
		hideForDraw(c->unk29C);
		for (int i = 0; i < 4; i++) {
			hideForDraw(c->unk278[i]);
			hideForDraw(c->unk28C[i]);
			hideForDraw(c->unk314[i]);
		}
		hideForDraw(c->unk324);
		hideForDraw(c->unk328);
		hideForDraw(c->unk32C);
		// The gauge's back is drawn by hand while the tank is up or coming
		// up (drawWaterBack): for this draw only, the HUD is told it is not.
		s_tankAppearing = c->unk45;
		s_tankUp        = c->unk46;
		s_tankSaved     = true;
		c->unk45 = c->unk46 = 0;
	}
}

void after(TGCConsole2* c)
{
	// in reverse, so a pane listed twice ends as it first was
	for (int i = s_count - 1; i >= 0; i--) {
		if (s_was[i])
			s_pane[i]->show();
		else
			s_pane[i]->hide();
	}
	s_count = 0;
	if (s_tankSaved) {
		c->unk45    = s_tankAppearing;
		c->unk46    = s_tankUp;
		s_tankSaved = false;
	}
}

} // namespace

// A pane's tag from its name as the layout has it ("c_ic"; up to 4 characters, the last ones lowest).
static u32 tagOf(const char* name)
{
	u32 tag = 0;
	for (int i = 0; i < 4 && name && name[i]; i++)
		tag = tag << 8 | (u8)name[i];
	return tag;
}

extern "C" int port_hud_picture(const char* name, const unsigned char* rgba, int w, int h)
{
	const u32 tag = tagOf(name);
	if (!tag)
		return 0;
	int at = -1;
	for (int i = 0; i < kMaxPictures; i++)
		if (s_pictures[i].image && s_pictures[i].tag == tag)
			at = i;
	if (!rgba) { // the game's own again from the next stage's HUD (this one keeps it till then)
		if (at >= 0) {
			JKRHeap::free(s_pictures[at].image, JKRHeap::getSystemHeap());
			s_pictures[at].image = nullptr;
		}
		return 1;
	}
	if (w < 4 || h < 4 || w > 256 || h > 256 || (w & (w - 1)) || (h & (h - 1)))
		return 0;
	if (at < 0)
		for (int i = 0; i < kMaxPictures && at < 0; i++)
			if (!s_pictures[i].image)
				at = i;
	if (at < 0)
		return 0;
	const u32 bytes = (u32)w * h * 4;
	ResTIMG* t      = (ResTIMG*)JKRHeap::alloc(sizeof(ResTIMG) + bytes, 32, JKRHeap::getSystemHeap());
	if (!t)
		return 0;
	memset(t, 0, sizeof(ResTIMG));
	t->format = GX_TF_RGBA8, t->alphaEnabled = 1, t->width = (u16)w, t->height = (u16)h;
	t->wrapS = t->wrapT = GX_CLAMP, t->minFilter = t->magFilter = GX_LINEAR;
	t->mipmapCount = 1, t->imageDataOffset = sizeof(ResTIMG);
	// GX's RGBA8: 4x4 tiles, each 32 bytes of alpha and red pairs, then 32 of green and blue
	u8* out = (u8*)t + sizeof(ResTIMG);
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 4) {
			for (int k = 0; k < 16; k++) {
				const unsigned char* px = rgba + ((ty + k / 4) * w + tx + k % 4) * 4;
				out[k * 2] = px[3], out[k * 2 + 1] = px[0];
				out[32 + k * 2] = px[1], out[32 + k * 2 + 1] = px[2];
			}
			out += 64;
		}
	DCFlushRange(t, sizeof(ResTIMG) + bytes);
	if (s_pictures[at].image)
		JKRHeap::free(s_pictures[at].image, JKRHeap::getSystemHeap());
	s_pictures[at].tag = tag, s_pictures[at].image = t;
	return 1;
}

extern "C" void port_hud_set_hidden(int owner, unsigned mask)
{
	if (owner < 0 || owner >= PORT_HUD_OWNERS)
		return;
	s_hidden[owner] = mask & PORT_HUD_ALL;
	s_mask          = 0;
	for (int i = 0; i < PORT_HUD_OWNERS; i++)
		s_mask |= s_hidden[i];
}

extern "C" unsigned port_hud_hidden(void)
{
	return hiddenNow();
}

extern "C" void port_hud_set_drawn(unsigned parts)
{
	s_drawn = parts & PORT_HUD_EVERYTHING;
}

extern "C" int port_hud_drawing_rest(void)
{
	return (s_drawn & PORT_HUD_REST) != 0;
}

extern "C" void port_hud_draw(void* console, int afterDraw)
{
	TGCConsole2* c = (TGCConsole2*)console;
	if (!c)
		return;
	if (afterDraw)
		after(c);
	else
		before(c);
}
