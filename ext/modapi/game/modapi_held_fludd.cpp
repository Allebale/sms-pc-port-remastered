// FLUDD held in view (api->held_fludd, First Person SDK): for a first-person camera, the game's own
// FLUDD model is moved each frame in front of the view, held like a gun, its nozzle's tip at the
// mod's place (right, down and forward of the camera) and pointing at the middle of the view, a
// point `aim` units ahead. The water comes out of the nozzle there, towards that point.
//
// When: as Mario's FLUDD is calculated for the view (CUE_CALC_VIEW, TWaterGun::perform, patch
// zzzzzzz-held-fludd), after the camera has been placed for the frame, so it never lags the view.
// FLUDD has been posed on his back by then (CUE_CALC_ANIM); that pose is moved as a whole (a turn
// and a move, and the mod's scale), so its animations (the pump, the nozzle's tilt) still play.
// The turn: the nozzle's direction to the view's aim, its roll about it kept as it was against
// the world's up. With the nozzle near straight up or down (climbing, hanging), the last good turn
// against the view is kept instead.
//
// Only with the spray and underwater nozzles, off Yoshi, on the player's own Mario.
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include "../modapi_bridge.h"
#include "sms_gx/gx_pc.h"
#include "port_menu_draw.h"

#include <math.h>
#include <string.h>
#include "port_framerate.h"

// Split-screen co-op (ext/coop): each player's FLUDD is held in their own view (modapi_game.cpp
// tells the players apart). All that follows is each player's own: Held, s_held[player].
int PortPlayerNow();
int PortPlayerIndex(const void* m);
int PortViewer();
int PortPlayers();
TMario* PortPlayerMario(int i);

namespace {

struct Held {
	bool on;
	f32 right, down, forward, scale, aim;
	Vec target;           // where this frame's water goes
	Vec viewEye, viewWay; // the view this frame (held_fludd->aim)
	f32 kick;             // a shot's recoil, 0 to 1, settling
	f32 bobPhase, bobSize, bobLift; // the sway as he walks, runs and jumps
	bool placed;          // placed this frame (target good)
	Mtx lastBase;         // the base it was given (the same next frame: not posed again, frozen)
	bool haveLast;
	f32 inView[3][3];     // its last good turn in the view's own frame
	bool haveInView;
	int flipAfter;        // frames left to keep it still after a flip
	int view;             // the first-person camera is on (set_view)
	f32 zoomFovy;         // a mod's zoom: the field of view to narrow the camera to (0 none)
	int zoomFlags;        // SMS_ZOOM_FIRST_PERSON, SMS_ZOOM_SCOPE
	int packFwd, packUp;  // the pack's axes (onBack)
	f32 packFwdSign, packUpSign;
	int packFwdO, packUpO; // ...seen by the other player while FLUDD is held (co-op: posed on the chest,
	f32 packFwdSignO, packUpSignO; // other axes than on his back: learnt apart)
	int poseFlags;        // a mod's pose for it this tick (held_fludd->pose): SMS_POSE_*,
	f32 posePump;         // its pump (0 to 1, -1 none) and how far ahead held in view (0 the set's)
	f32 poseHold;
	Mtx heldEmit;         // held in view: its emit matrix and scale (the roller coaster's rocket)
	f32 heldEmitScale;
	bool haveHeldEmit;
	TWaterGun* heldEmitGun;
	Vec backEmit;         // co-op: where its nozzle is as the other player sees it (on his back)
	bool haveBackEmit;
	Held()
	{
		memset(this, 0, sizeof *this);
		scale = 1.0f, aim = 1200.0f, packFwd = -1, packFwdO = -1, heldEmitScale = 1.0f;
	}
};
Held s_held[2];
int s_for = -1; // the player whose FLUDD is being worked on (its owner, whoever the game runs as)
Held& H() { return s_held[s_for >= 0 ? s_for : PortPlayerNow()]; }
// For this player's FLUDD while it lives
struct For {
	int saved;
	explicit For(int i) : saved(s_for) { s_for = i; }
	~For() { s_for = saved; }
};
#define s_on H().on
#define s_right H().right
#define s_down H().down
#define s_forward H().forward
#define s_scale H().scale
#define s_aim H().aim
#define s_target H().target
#define s_viewEye H().viewEye
#define s_viewWay H().viewWay
#define s_kick H().kick
#define s_bobPhase H().bobPhase
#define s_bobSize H().bobSize
#define s_bobLift H().bobLift
#define s_placed H().placed
#define s_lastBase H().lastBase
#define s_haveLast H().haveLast
#define s_inView H().inView
#define s_haveInView H().haveInView
#define s_flipAfter H().flipAfter
#define s_view H().view
#define s_zoomFovy H().zoomFovy
#define s_zoomFlags H().zoomFlags
#define s_packFwd H().packFwd
#define s_packUp H().packUp
#define s_packFwdSign H().packFwdSign
#define s_packUpSign H().packUpSign
#define s_heldEmit H().heldEmit
#define s_heldEmitScale H().heldEmitScale
#define s_haveHeldEmit H().haveHeldEmit
#define s_heldEmitGun H().heldEmitGun

// J3DModel's base scale is protected: read and set through a class of its own
struct ModelScale : J3DModel {
	static Vec& of(J3DModel* m) { return static_cast<ModelScale*>(m)->unk14; }
};

f32 dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(const Vec& a, const Vec& b)
{
	Vec c = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	return c;
}
bool unit(Vec& v)
{
	const f32 l = sqrtf(dot(v, v));
	if (l < 1e-6f)
		return false;
	v.x /= l, v.y /= l, v.z /= l;
	return true;
}
Vec along(const Vec& a, f32 s, const Vec& b) // a + s * b
{
	Vec c = {a.x + s * b.x, a.y + s * b.y, a.z + s * b.z};
	return c;
}
// a frame from a forward and a rough up: rows forward, up, side
bool frame(const Vec& forward, const Vec& roughUp, Vec out[3])
{
	out[0] = forward;
	out[1] = along(roughUp, -dot(roughUp, forward), forward);
	if (!unit(out[1]))
		return false;
	out[2] = cross(out[0], out[1]);
	return true;
}

// A flip or spin: his body turned right round (MarioStatus.hpp), and 5 frames after one
bool flipping(TMario* m)
{
	switch (m->getStatus()) {
	case MARIO_STATUS_ULTRA_JUMP:        // the triple jump's flip
	case MARIO_STATUS_BACK_JUMP:         // back flip
	case MARIO_STATUS_U_TURN_JUMP:       // side flip
	case MARIO_STATUS_ROTATE_BROAD_JUMP:
	case MARIO_STATUS_WIRE_ROLL_JUMP:
	case MARIO_STATUS_LEFT_ROTATE_JUMP:  // spin jumps
	case MARIO_STATUS_RIGHT_ROTATE_JUMP:
		s_flipAfter = 5;
		return true;
	default:
		break;
	}
	if (s_flipAfter > 0) {
		s_flipAfter--;
		return true;
	}
	return false;
}

} // namespace

// api->held_fludd->set (mod_runtime.cpp)
extern "C" int modg_held_fludd_set(int on, float right, float down, float forward, float scale, float aim)
{
	s_on      = on != 0;
	s_right   = right;
	s_down    = down;
	s_forward = forward;
	s_scale   = scale > 0.05f && scale < 4.0f ? scale : 1.0f;
	s_aim     = aim > 50.0f ? aim : 1200.0f;
	if (!s_on) { // (its turn in the view is kept: shown again with the nozzle away, it is held so)
		s_placed = s_haveLast = false;
	}
	s_flipAfter = 0;
	return PortPlayerMario(PortPlayerNow()) ? 1 : 0;
}

// Shared by a first-person mod and a mod with a gun (api->held_fludd's set_view, set_zoom, get_state):
// what each sets, for the other to read (each player's). Nothing in the game acts on it.

extern "C" int modg_held_fludd_view(int on)
{
	s_view = on != 0;
	return 1;
}

extern "C" int modg_held_fludd_zoom(float fovy, int flags)
{
	s_zoomFovy  = fovy > 1.0f && fovy < 170.0f ? fovy : 0.0f;
	s_zoomFlags = s_zoomFovy > 0.0f ? flags : 0;
	return 1;
}

extern "C" int port_coop_view_rect(int view, float* r); // ext/coop: a player's part of the screen

// SMS_EV_DRAW_UNDER_HUD: the mods' shapes for this view (co-op: this player's half), drawn into it
// here, after the scene and before the HUD. Their canvas is the view's, 480 high, its units square as
// seen (its picture's aspect, side by side halves half as wide).
void drawUnderHud(JDrama::TGraphics* graphics)
{
	const JDrama::TRect& vp = graphics->getViewport();
	const f32 w = (f32)(vp.x2 - vp.x1), h = (f32)(vp.y2 - vp.y1);
	if (w <= 0.0f || h <= 0.0f)
		return;
	const int player = PortPlayerNow();
	f32 cw = 640.0f, ch = 480.0f;
	GXPC_ModLayerCanvas(&cw, &ch);
	float part[4];
	f32 aspect = cw / ch;
	if (port_coop_view_rect(player, part) && part[2] > 0.01f && part[3] > 0.01f)
		aspect *= part[2] / part[3];
	const f32 vw = 480.0f * aspect, vh = 480.0f;
	static u32 s_frame;
	const GXPCModVertex* v = nullptr;
	const int n            = modh_draw_under(vw, vh, player, ++s_frame, &v);
	if (n < 3 || !v)
		return;
	J2DOrthoGraph graph(vp);
	graph.setPort();
	graph.setup2D();
	PortMenu::wholeScreen(w, h); // to the last row too (the game's 2D port leaves it out)
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	const f32 kx = w / vw, ky = h / vh;
	for (int at = 0; at + 3 <= n;) {
		int k = n - at > 60000 ? 60000 : n - at;
		k -= k % 3;
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)k);
		for (int i = 0; i < k; i++) {
			const GXPCModVertex& q = v[at + i];
			GXPosition3f32(q.x * kx, q.y * ky, 0.0f);
			GXColor4u8(q.rgba[0], q.rgba[1], q.rgba[2], q.rgba[3]);
		}
		GXEnd();
		at += k;
	}
}

// The mods' place in the game's last list, after the characters and before the HUD
// (MarDirectorInitECT.cpp): their under-HUD draws there each frame.
namespace {
class TUnderHudObj : public JDrama::TViewObj {
public:
	TUnderHudObj()
	    : JDrama::TViewObj("<mods under the HUD>")
	{
	}
	virtual void perform(u32 cue, JDrama::TGraphics* graphics)
	{
		if ((cue & 0x8) && graphics) // CUE_DRAW: the mods' draws under the HUD (SMS_EV_DRAW_UNDER_HUD)
			drawUnderHud(graphics);
	}
};
TUnderHudObj s_underHudObj;
} // namespace

JDrama::TViewObj* PortUnderHudObj() { return &s_underHudObj; }

bool PortFluddHeld(TMario* mario);
extern "C" int modg_held_fludd_state(int* firstPerson, int* held, float* zoomFovy, int* zoomFlags)
{
	*firstPerson = s_view;
	*held        = PortPlayerMario(PortPlayerNow()) && PortFluddHeld(PortPlayerMario(PortPlayerNow()));
	*zoomFovy    = s_zoomFovy;
	*zoomFlags   = s_zoomFlags;
	return 1;
}

// api->held_fludd->pose (API 1.8): a mod's pose for a player's FLUDD this tick (asked each tick)
extern "C" int modg_held_fludd_pose(int player, int flags, float pump, float hold)
{
	if (player < 0 || player > 1)
		return 0;
	Held& h     = s_held[player];
	h.poseFlags = flags & (SMS_POSE_STILL | SMS_POSE_NOZZLE_OUT);
	h.posePump  = pump >= 0.0f ? (pump < 1.0f ? pump : 1.0f) : -1.0f;
	h.poseHold  = hold > 0.0f ? hold : 0.0f;
	return 1;
}

// each tick (modapi_models.cpp's): the poses asked for again
void PortHeldFluddPoseTick()
{
	for (int p = 0; p < 2; p++)
		s_held[p].poseFlags = 0, s_held[p].posePump = -1.0f, s_held[p].poseHold = 0.0f;
}
void PortModelsHideFluddNow(int player, int frames); // modapi_models.cpp

// Co-op's player 2 (Luigi, ext/coop): while the game runs as player 2, gpMarioOriginal is Luigi.
// The First Person mod and FLUDD held in view are each player's own.
extern "C" THitActor* port_coop_player2_actor(void);
bool PortIsPlayer2(TMario* mario)
{
	return mario && (THitActor*)mario == port_coop_player2_actor();
}

bool PortFluddHeld(TMario* mario)
{
	const int i = PortPlayerIndex(mario);
	if (!s_held[i].on || !mario || mario != PortPlayerMario(i) || mario->onYoshi())
		return false;
	TWaterGun* g = mario->mWaterGun;
	if (!g || !g->mFluddModel)
		return false;
	// the spray nozzle only: the underwater one is a jet pack, steered by the stick and pushing him
	// the other way from its water, so it stays on his back
	return g->mCurrentNozzle == TWaterGun::Spray;
}

// Which of FLUDD's own axes (its base turn on his back) point forward and up, and their signs:
// learnt while he stands up straight, then kept, so a model on his back lies down with him in a
// dive or a belly slide as FLUDD does (Held's packFwd, packUp). `other`: co-op, the other player's
// view of a player whose FLUDD is held in their own (posed on their chest there).
// FLUDD on his back's frame (a mod's models' SMS_FRAME_BACK): from its
// nozzle_center, its turn on his back (its axes nearest the way he faces and up), so it leans and
// sways with him as FLUDD does. `other`: co-op, the other player's view of a player whose FLUDD is
// held in their own (posed on the chest there: upright, the way his body faces). Its axes (right,
// forward, up) and the way he faces across the ground; false when FLUDD has no model.
bool backAxes(TWaterGun* g, bool other, Vec* rightOut, Vec* fwdOut, Vec* upOut, Vec* faceOut, MtxPtr* ncOut)
{
	if (!g || !g->mFluddModel || !g->mMario)
		return false;
	J3DModel* fm = g->mFluddModel->getModel();
	// the pack's frame: from FLUDD's nozzle_center, its turn on his back (its axes nearest the way
	// he faces and up), so it leans and sways with him as FLUDD does
	MtxPtr base   = fm->getBaseTRMtx();
	MtxPtr nc     = fm->getAnmMtx(g->unk1CD8);
	const f32 ang = g->mMario->mFaceAngle.y * (3.14159265f / 32768.0f);
	Vec face = {sinf(ang), 0.0f, cosf(ang)};
	const Vec worldUp = {0.0f, 1.0f, 0.0f};
	if (other) { // the way his body faces as drawn (strafing in first person it is not his facing):
		// from FLUDD on his back (posed on his chest) to his chest, across the ground
		J3DModel* body = g->mMario->getM3UModel() ? g->mMario->getM3UModel()->getModel() : 0;
		MtxPtr nc0     = g->mFluddModel->getModel()->getAnmMtx(g->unk1CD8);
		if (body) {
			MtxPtr ch = body->getAnmMtx(g->mMario->mJointIdChest);
			Vec z = {ch[0][3] - nc0[0][3], 0.0f, ch[2][3] - nc0[2][3]};
			if (z.x * z.x + z.z * z.z > 4.0f && unit(z))
				face = z;
			else {
				MtxPtr bm = body->getBaseTRMtx();
				Vec b = {bm[0][2], 0.0f, bm[2][2]};
				if (unit(b))
					face = b;
			}
		}
	}
	Vec axis[3];
	for (int c = 0; c < 3; c++) {
		axis[c].x = base[0][c], axis[c].y = base[1][c], axis[c].z = base[2][c];
		unit(axis[c]);
	}
	int fi = 0, ui = 1;
	f32 best = -1.0f, bestF = 0.0f;
	for (int c = 0; c < 3; c++) {
		const f32 d = fabsf(axis[c].x * face.x + axis[c].z * face.z);
		if (d > best)
			best = d, fi = c;
	}
	bestF = best;
	best  = -1.0f;
	for (int c = 0; c < 3; c++) {
		if (c == fi)
			continue;
		const f32 d = fabsf(axis[c].y);
		if (d > best)
			best = d, ui = c;
	}
	Held& hp = H();
	int& packFwd = other ? hp.packFwdO : hp.packFwd; // (the other player's view: its own, learnt apart)
	int& packUp  = other ? hp.packUpO : hp.packUp;
	f32& fwdSign = other ? hp.packFwdSignO : hp.packFwdSign;
	f32& upSign  = other ? hp.packUpSignO : hp.packUpSign;
	const u32 st = g->mMario->mStatus;
	if (bestF > 0.9f && best > 0.9f && (st == MARIO_STATUS_WAIT || st == MARIO_STATUS_RUN)) { // standing or running (not lying)
		packFwd     = fi, packUp = ui;
		fwdSign = axis[fi].x * face.x + axis[fi].z * face.z < 0.0f ? -1.0f : 1.0f;
		upSign  = axis[ui].y < 0.0f ? -1.0f : 1.0f;
	}
	if (packFwd >= 0) { // as learnt: FLUDD's own axes, however he lies
		fi = packFwd, ui = packUp;
		axis[fi].x *= fwdSign, axis[fi].y *= fwdSign, axis[fi].z *= fwdSign;
		axis[ui].x *= upSign, axis[ui].y *= upSign, axis[ui].z *= upSign;
	}
	Vec fwd = axis[fi], up = axis[ui];
	if (packFwd < 0) { // not learnt yet: the nearest
		if (fwd.x * face.x + fwd.z * face.z < 0.0f)
			fwd.x = -fwd.x, fwd.y = -fwd.y, fwd.z = -fwd.z;
		if (up.y < 0.0f)
			up.x = -up.x, up.y = -up.y, up.z = -up.z;
	}
	if (other) { // the other player's view (FLUDD posed on the chest, not his back): upright, the way he faces
		fwd = face;
		up  = worldUp;
	}
	Vec right = cross(fwd, up);
	if (!unit(right))
		return false;
	up = cross(right, fwd);
	unit(up);
	*rightOut = right, *fwdOut = fwd, *upOut = up, *faceOut = face, *ncOut = nc;
	return true;
}

// FLUDD on his back posed by a mod (held_fludd->pose): its nozzle kept out (SMS_POSE_NOZZLE_OUT, a
// mod's gun firing from it in third person), still but for a pump at the pose's point of one
// (posePump), not put away or pumped as the game would.
void onBack(TWaterGun* g)
{
	For f(PortPlayerIndex(g->mMario));
	const f32 phase = H().posePump;
	if (!g->mFluddModel || (!(H().poseFlags & SMS_POSE_NOZZLE_OUT) && phase < 0.0f))
		return;
	J3DModel* fm = g->mFluddModel->getModel();
	g->unk1CEC   = 0.0f;       // out
	g->unk1CFA   = g->unk1CF8; // and the game's own wait before it puts it away
	g->mFluddModel->setBck(phase >= 0.0f ? "wg_pump" : "wg_house");
	if (J3DFrameCtrl* ctrl = g->mFluddModel->getFrameCtrl(ANM_TYPE_BCK)) {
		ctrl->setRate(0.0f);
		ctrl->setFrame(phase >= 0.0f ? phase * ctrl->getEnd() : 0.0f);
	}
	g->mFluddModel->calc();
	if (MActor* nozzle = g->getCurrentNozzle()->getMActor()) {
		nozzle->getModel()->setBaseTRMtx(fm->getAnmMtx(g->unk1CD8));
		nozzle->getModel()->calc();
	}
	g->setEmitPt();
}

// As FLUDD is calculated for the view (TWaterGun::perform): moved in front of it.
// FLUDD held in view's emit matrix (after it is placed) and its scale: a rocket Mario picked up (the
// roller coaster's, TRocket::calcRootMatrix) sits on its nozzle there, not on his back's
// (Held's heldEmit, heldEmitScale, haveHeldEmit, heldEmitGun)



void placeFor(TWaterGun* g, JDrama::TGraphics* graphics);

// A mod's models' frames (modapi_models.cpp): FLUDD on his back's, and turned level, in this view
void PortModelsView(int view, MtxPtr viewMtx);
void PortModelsFrame(int player, int view, int frame, MtxPtr world); // null: not there
void backFrames(TWaterGun* g, int who, bool other)
{
	For f(who);
	const int view = PortViewer() ? 1 : 0;
	Vec right, fwd, up, face;
	MtxPtr nc;
	if (!backAxes(g, other, &right, &fwd, &up, &face, &nc)) {
		PortModelsFrame(who, view, 2, nullptr);
		PortModelsFrame(who, view, 3, nullptr);
		return;
	}
	// level: ahead across the ground and up while he is up; lying down along him, its top off his back
	Vec ahead = up, gUp = {-fwd.x, -fwd.y, -fwd.z};
	if (up.y > 0.6f) {
		ahead   = fwd;
		ahead.y = 0.0f;
		if (!unit(ahead))
			ahead = face;
		gUp.x = 0.0f, gUp.y = 1.0f, gUp.z = 0.0f;
	}
	Vec gRight = cross(ahead, gUp);
	unit(gRight);
	Mtx place, level;
	const Vec ax[3] = {right, fwd, up}, lx[3] = {gRight, ahead, gUp};
	const f32 at[3] = {nc[0][3], nc[1][3], nc[2][3]};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			place[r][c] = r == 0 ? ax[c].x : r == 1 ? ax[c].y : ax[c].z;
			level[r][c] = r == 0 ? lx[c].x : r == 1 ? lx[c].y : lx[c].z;
		}
		place[r][3] = level[r][3] = at[r];
	}
	PortModelsFrame(who, view, 2, place);
	PortModelsFrame(who, view, 3, level);
}

void PortFluddHeldPlace(TWaterGun* g, JDrama::TGraphics* graphics)
{
	const int who = g ? PortPlayerIndex(g->mMario) : PortPlayerNow();
	For f(who);
	if (graphics)
		PortModelsView(PortViewer() ? 1 : 0, graphics->getViewMtx());
	if (g && g->mMario && PortPlayers() == 2 && who != PortViewer()) {
		// co-op, the other player's view: their FLUDD on their back there (as the game poses it),
		// held in their own view only; its water still starts where it is held (not set again)
		const bool held = PortFluddHeld(g->mMario);
		if (held && g->mFluddModel) {
			J3DModel* model = g->mFluddModel->getModel();
			model->setBaseTRMtx(g->mMario->getM3UModel()->getModel()->getAnmMtx(g->mMario->mJointIdChest));
			g->mFluddModel->calc();
			if (MActor* nozzle = g->getCurrentNozzle()->getMActor()) {
				nozzle->getModel()->setBaseTRMtx(model->getAnmMtx(g->unk1CD8));
				nozzle->getModel()->calc();
			}
			if (MtxPtr em = g->getEmitMtx(0)) { // its water seen coming from there (PortWaterSeenAt)
				Held& h = s_held[who];
				h.backEmit.x = em[0][3], h.backEmit.y = em[1][3], h.backEmit.z = em[2][3];
				h.haveBackEmit = true;
			}
		}
		PortModelsFrame(who, PortViewer() ? 1 : 0, 1, nullptr); // (held in their own view only)
		backFrames(g, who, held);
		return;
	}
	if (g && g->mMario && !PortFluddHeld(g->mMario))
		PortModelsFrame(who, PortViewer() ? 1 : 0, 1, nullptr);
	placeFor(g, graphics);
	if (g && g->mMario && !PortFluddHeld(g->mMario))
		backFrames(g, who, false);
	else if (g)
		PortModelsFrame(who, PortViewer() ? 1 : 0, 2, nullptr), PortModelsFrame(who, PortViewer() ? 1 : 0, 3, nullptr);
}

void placeFor(TWaterGun* g, JDrama::TGraphics* graphics)
{
	s_placed = false;
	if (g && g != s_heldEmitGun)
		s_haveHeldEmit = false; // another FLUDD (another stage): placed afresh
	const bool dead = g && g->mMario && PortFluddHeld(g->mMario) && g->mMario->checkFlag(MARIO_FLAG_GAME_OVER);
	if (g && g->mMario)
		PortModelsHideFluddNow(PortPlayerIndex(g->mMario), dead ? 1 << SMS_FRAME_HELD : 0);
	if (dead)
		return; // he is dead (or dying): FLUDD held in view hidden
	if (g && graphics && !PortFluddHeld(g->mMario))
		onBack(g);
	if (!g || !graphics || !PortFluddHeld(g->mMario))
		return;
	// its nozzle kept out, as the gun: idle, the game puts it away after a moment (the "house"
	// animation, unk1CEC from 0 out to 1 away), which would turn the whole of it round to face
	// the view (the nozzle's way is what is turned to the aim). Held at 0 for the next frame:
	// the idle step adds unk1CF4 to it, and with no delay left (unk1CFA) poses it there.
	// Pumping, 0 itself: the pump's own animation then (the normal spray). With a mod's still pose
	// chosen its animation is set below, still but for a pump as it fires.
	const u32 upper = (g->mMario->mUpperState & 0x8000) ? 0 : g->mMario->mUpperState;
	g->unk1CFA      = 0;
	g->unk1CEC      = (upper == TMario::UPPER_STATE_PUMPING || upper == TMario::UPPER_STATE_HOLDING_PUMP) ? 0.0f : -g->unk1CF4;
	const bool nozzleAway = g->unk1CEC > 0.0f; // put away, or on its way (should the game's own do it)

	J3DModel* model = g->mFluddModel->getModel();
	MtxPtr base     = model->getBaseTRMtx();
	if (s_haveLast && memcmp(base, s_lastBase, sizeof(Mtx)) == 0) {
		s_placed = true; // not posed again this frame (Mario frozen): where it was
		return;
	}
	// still, nozzle out (the put-away animation at its start), unless water comes out: the
	// normal spray's pump as the game has it while it sprays; a mod's one pump as its gun fires.
	// (Otherwise the game may pump it along with Mario's arm: Super Mario Eclipse keeps him
	// holding the pump.)
	const bool still      = (H().poseFlags & SMS_POSE_STILL) != 0; // a mod's gun (held_fludd->pose)
	const bool overridden = still || !g->isEmitting();
	const f32 phase       = still ? H().posePump : -1.0f;
	if (overridden) { // the still pose first (the gun's turn is worked out from it)
		g->mFluddModel->setBck("wg_house");
		if (J3DFrameCtrl* ctrl = g->mFluddModel->getFrameCtrl(ANM_TYPE_BCK)) {
			ctrl->setRate(0.0f);
			ctrl->setFrame(0.0f);
		}
	}
	// posed so on his back first: the nozzle's way, turned to the aim below, is then this pose's
	if (overridden) {
		g->mFluddModel->calc();
		if (MActor* nozzle = g->getCurrentNozzle()->getMActor()) {
			nozzle->getModel()->setBaseTRMtx(model->getAnmMtx(g->unk1CD8));
			nozzle->getModel()->calc();
		}
	}
	MtxPtr em = g->getEmitMtx(0);
	if (!em) {
		return;
	}

	// the view: its right, up and forward, and the eye
	MtxPtr v     = graphics->getViewMtx();
	const Vec R  = {v[0][0], v[0][1], v[0][2]};
	const Vec U  = {v[1][0], v[1][1], v[1][2]};
	const Vec F  = {-v[2][0], -v[2][1], -v[2][2]};
	Vec eye      = {0, 0, 0};
	eye          = along(eye, -v[0][3], R);
	eye          = along(eye, -v[1][3], U);
	eye          = along(eye, v[2][3], F);
	// how far ahead it is held: the first-person mod's, or a mod's pose's (its gun's own)
	const f32 vd   = H().poseHold;
	const f32 hold = vd > 1.0f && s_forward > 1.0f ? vd / s_forward : 1.0f;
	Vec tip        = along(along(along(eye, s_right * hold, R), -s_down * hold, U), s_forward * hold, F);
	s_target     = along(eye, s_aim, F);
	s_viewEye    = eye;
	s_viewWay    = F;
	Vec toTarget = {s_target.x - tip.x, s_target.y - tip.y, s_target.z - tip.z};
	if (!unit(toTarget))
		toTarget = F;

	// its pose on his back: the base's turn and place, and the nozzle's tip and way
	f32 rot[3][3];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			rot[r][c] = base[r][c];
	const Vec origin = {base[0][3], base[1][3], base[2][3]};
	Vec way          = {em[0][0], em[1][0], em[2][0]};
	const Vec at     = {em[0][3], em[1][3], em[2][3]};
	if (!unit(way)) {
		return;
	}

	// the new turn, kept in the view's frame: the nozzle's way to the aim, its roll against the up
	// kept. During a back flip, side flip, triple jump or spin (his body, and FLUDD on it, turned
	// right round), and with the nozzle near straight up or down (climbing, hanging), the last one
	// is kept, so the gun does not turn with him.
	const bool flip = flipping(g->mMario);
	const Vec up    = {0.0f, 1.0f, 0.0f};
	Vec a[3], b[3];
	if (fabsf(way.y) < 0.8f && frame(way, up, a) && frame(toTarget, U, b)) {
		f32 g3[3][3]; // b's rows from a's: sum of b_i a_i^T
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				g3[r][c] = 0.0f;
		for (int i = 0; i < 3; i++) {
			const f32 bi[3] = {b[i].x, b[i].y, b[i].z}, ai[3] = {a[i].x, a[i].y, a[i].z};
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					g3[r][c] += bi[r] * ai[c];
		}
		f32 w3[3][3], inView[3][3];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				w3[r][c] = g3[r][0] * rot[0][c] + g3[r][1] * rot[1][c] + g3[r][2] * rot[2][c];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				inView[r][c] = v[r][0] * w3[0][c] + v[r][1] * w3[1][c] + v[r][2] * w3[2][c];
		// during a flip (and a few frames after) the last shown is kept: his body turns right round
		if (!flip && !nozzleAway) {
			memcpy(s_inView, inView, sizeof inView);
			s_haveInView = true;
		}
	}
	if (!s_haveInView) {
		return;
	}
	f32 turned[3][3];
	for (int r = 0; r < 3; r++) // the view's turn back to the world (its transpose)
		for (int c = 0; c < 3; c++)
			turned[r][c] = v[0][r] * s_inView[0][c] + v[1][r] * s_inView[1][c] + v[2][r] * s_inView[2][c];

	// the sway: side to side and up and down with his steps (more as he runs), and a lag up or
	// down as he jumps and lands, smoothed
	{
		const f32 dt    = port_frame_scale(); // 30ths of a second
		const TMario* m = g->mMario;
		const f32 speed = sqrtf(m->mVel.x * m->mVel.x + m->mVel.z * m->mVel.z);
		const bool air  = (m->getStatus() & 0x800) != 0; // MARIO_STATUS_FLAG_JUMPING (in the air)
		const f32 want  = air ? 0.0f : (speed > 32.0f ? 1.0f : speed / 32.0f);
		s_bobSize += (want - s_bobSize) * (1.0f - powf(0.85f, dt));
		s_bobPhase += dt * (0.18f + 0.22f * s_bobSize);
		const f32 lift = air ? (m->mVel.y > 25.0f ? 25.0f : m->mVel.y < -25.0f ? -25.0f : m->mVel.y) * -0.25f : 0.0f;
		s_bobLift += (lift - s_bobLift) * (1.0f - powf(0.8f, dt));
		const f32 side = sinf(s_bobPhase) * 2.6f * s_bobSize, up = fabsf(cosf(s_bobPhase)) * 1.8f * s_bobSize;
		tip = along(along(tip, side, R), up + s_bobLift, U);
	}

	// a shot's recoil: the gun kicked back and its nozzle tipped up, settling again
	if (s_kick > 0.001f) {
		const f32 a = s_kick * 0.35f, c = cosf(a), sn = sinf(a), t = 1.0f - c;
		const f32 x = R.x, y = R.y, z = R.z; // about the view's right
		const f32 rr[3][3] = {{t * x * x + c, t * x * y - sn * z, t * x * z + sn * y},
		                      {t * x * y + sn * z, t * y * y + c, t * y * z - sn * x},
		                      {t * x * z - sn * y, t * y * z + sn * x, t * z * z + c}};
		f32 kicked[3][3];
		for (int r = 0; r < 3; r++)
			for (int c2 = 0; c2 < 3; c2++)
				kicked[r][c2] = rr[r][0] * turned[0][c2] + rr[r][1] * turned[1][c2] + rr[r][2] * turned[2][c2];
		memcpy(turned, kicked, sizeof kicked);
		tip = along(along(tip, -s_kick * 14.0f, F), s_kick * 3.0f, U);
		s_kick *= powf(0.78f, port_frame_scale());
	}

	// A mod's models' SMS_FRAME_HELD: at the nozzle's tip, pointing at the aim, tipped up by the kick
	{
		Vec fwd = {toTarget.x + U.x * s_kick * 0.35f, toTarget.y + U.y * s_kick * 0.35f, toTarget.z + U.z * s_kick * 0.35f};
		unit(fwd);
		Vec right = cross(fwd, U);
		if (unit(right)) {
			const Vec up = cross(right, fwd);
			Mtx held;
			const Vec ax[3] = {right, fwd, up};
			const f32 at[3] = {tip.x, tip.y, tip.z};
			for (int r = 0; r < 3; r++) {
				for (int c = 0; c < 3; c++)
					held[r][c] = r == 0 ? ax[c].x : r == 1 ? ax[c].y : ax[c].z;
				held[r][3] = at[r];
			}
			PortModelsFrame(PortPlayerIndex(g->mMario), PortViewer() ? 1 : 0, 1, held);
		}
	}

	// the new place: the tip (at the mod's scale, from the base) where the mod put it
	const f32 k     = s_scale;
	const Vec rel   = {k * (at.x - origin.x), k * (at.y - origin.y), k * (at.z - origin.z)};
	const f32 loc[3] = {rot[0][0] * rel.x + rot[1][0] * rel.y + rot[2][0] * rel.z,
	                    rot[0][1] * rel.x + rot[1][1] * rel.y + rot[2][1] * rel.z,
	                    rot[0][2] * rel.x + rot[1][2] * rel.y + rot[2][2] * rel.z};
	Mtx nb;
	const f32 tipAt[3] = {tip.x, tip.y, tip.z};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			nb[r][c] = turned[r][c];
		nb[r][3] = tipAt[r] - (turned[r][0] * loc[0] + turned[r][1] * loc[1] + turned[r][2] * loc[2]);
	}

	// posed there (its scale only for this), the nozzle with it, the water's start
	Vec& scale      = ModelScale::of(model);
	const Vec saved = scale;
	scale.x *= k, scale.y *= k, scale.z *= k;
	model->setBaseTRMtx(nb);
	if (overridden && phase >= 0.0f) { // a mod's pump as its gun fires
		g->mFluddModel->setBck("wg_pump");
		if (J3DFrameCtrl* ctrl = g->mFluddModel->getFrameCtrl(ANM_TYPE_BCK)) {
			ctrl->setRate(0.0f);
			ctrl->setFrame(phase * ctrl->getEnd());
		}
	}
	g->mFluddModel->calc(); // posed there with its animation
	scale = saved;
	if (MActor* nozzle = g->getCurrentNozzle()->getMActor()) {
		nozzle->getModel()->setBaseTRMtx(model->getAnmMtx(g->unk1CD8));
		nozzle->getModel()->calc();
	}
	g->setEmitPt();
	MTXCopy(nb, s_lastBase);
	s_haveLast = true;
	s_placed   = true;
	if (MtxPtr em2 = g->getEmitMtx(0)) {
		MTXCopy(em2, s_heldEmit);
		s_heldEmitScale = k;
		s_haveHeldEmit  = true;
		s_heldEmitGun   = g;
	}
}

// FLUDD held in view's emit matrix and scale, or null while it is not held (TRocket, a patch)
MtxPtr PortFluddHeldEmitMtx(f32* scale)
{
	// (the player the game runs as now: the rocket being placed is theirs)
	if (!s_haveHeldEmit || !s_heldEmitGun || !s_heldEmitGun->mMario || !PortFluddHeld(s_heldEmitGun->mMario))
		return nullptr;
	if (scale)
		*scale = s_heldEmitScale;
	return s_heldEmit;
}

// The water's way from the held FLUDD (TWaterGun::getEmitPosDirSpeed): to the view's aim.
bool PortFluddHeldWay(TWaterGun* g, const JGeometry::TVec3<f32>& from, JGeometry::TVec3<f32>* way)
{
	For f(g ? PortPlayerIndex(g->mMario) : PortPlayerNow());
	if (!s_placed || !g || !PortFluddHeld(g->mMario))
		return false;
	Vec d = {s_target.x - from.x, s_target.y - from.y, s_target.z - from.z};
	if (!unit(d))
		return false;
	way->set(d.x, d.y, d.z);
	return true;
}

// While FLUDD is held in view, the back flip's and side flip's animations (TMario::setAnimation)
// are a double jump's instead: his body would turn over in front of the view.
bool PortFluddHeldNoFlip(TMario* mario, int anm_id)
{
	const int i = PortPlayerIndex(mario);
	if (!s_held[i].on || !mario || mario != PortPlayerMario(i) || mario->onYoshi())
		return false;
	const u32 status = mario->getStatus();
	return (status == MARIO_STATUS_BACK_JUMP && anm_id == 0xF7)
	       || (status == MARIO_STATUS_U_TURN_JUMP && anm_id == TMario::ANIM_TJMP1);
}

// The view this frame, while FLUDD is held in it (held_fludd->aim): the eye and the way it looks (a
// unit vector).
bool PortFluddHeldView(JGeometry::TVec3<f32>* eye, JGeometry::TVec3<f32>* forward)
{
	if (!s_placed)
		return false;
	eye->set(s_viewEye.x, s_viewEye.y, s_viewEye.z);
	forward->set(s_viewWay.x, s_viewWay.y, s_viewWay.z);
	return true;
}

// Co-op: whose view a view matrix is, told by its eye: the first-person player whose own view's
// eye it is (FLUDD held in it), or -1 (a third-person camera). The game's globals cannot tell it:
// the views are worked out and drawn at different times.
int PortViewOwner(MtxPtr view)
{
	if (!view)
		return -1;
	f32 eye[3];
	for (int c = 0; c < 3; c++)
		eye[c] = -(view[0][c] * view[0][3] + view[1][c] * view[1][3] + view[2][c] * view[2][3]);
	for (int p = 0; p < PortPlayers(); p++) {
		const Held& h = s_held[p];
		TMario* m     = PortPlayerMario(p);
		if (!m || !PortFluddHeld(m) || !h.haveInView)
			continue;
		const f32 dx = eye[0] - h.viewEye.x, dy = eye[1] - h.viewEye.y, dz = eye[2] - h.viewEye.z;
		if (dx * dx + dy * dy + dz * dz < 60.0f * 60.0f)
			return p;
	}
	return -1;
}

// Co-op: where a drop (at `at`) is seen in the view `view`. A first-person
// player's stream comes out of the FLUDD held in their own view, in front of their face; anyone
// else sees FLUDD on their back, so the drops near the held one are seen moved that way, less the
// further they are from it (none 400 away).
void PortWaterSeenAt(f32* at, MtxPtr view)
{
	if (PortPlayers() != 2)
		return;
	const int owner = PortViewOwner(view);
	for (int p = 0; p < 2; p++) {
		const Held& h = s_held[p];
		if (p == owner || !h.haveHeldEmit || !h.haveBackEmit)
			continue;
		TMario* m = PortPlayerMario(p);
		if (!m || !PortFluddHeld(m))
			continue;
		const f32 hx = h.heldEmit[0][3], hy = h.heldEmit[1][3], hz = h.heldEmit[2][3];
		const f32 dx = at[0] - hx, dy = at[1] - hy, dz = at[2] - hz;
		const f32 d2 = dx * dx + dy * dy + dz * dz, kReach = 400.0f;
		if (d2 >= kReach * kReach)
			continue;
		const f32 w = 1.0f - sqrtf(d2) / kReach;
		at[0] += (h.backEmit.x - hx) * w;
		at[1] += (h.backEmit.y - hy) * w;
		at[2] += (h.backEmit.z - hz) * w;
	}
}

// A shot's recoil for the held gun (held_fludd->kick): 0 to 1.
void PortFluddHeldKick(f32 amount)
{
	if (amount > s_kick)
		s_kick = amount > 1.0f ? 1.0f : amount;
}

// held_fludd->kick and ->aim (the mod API: the player selected, or the one the game runs as)

extern "C" void modg_held_fludd_kick(float amount)
{
	For f(PortPlayerNow());
	PortFluddHeldKick(amount);
}

extern "C" int modg_held_fludd_aim(SmsVec3* eye, SmsVec3* forward)
{
	For f(PortPlayerNow());
	JGeometry::TVec3<f32> e, w;
	if (!PortFluddHeldView(&e, &w))
		return 0;
	if (eye)
		eye->x = e.x, eye->y = e.y, eye->z = e.z;
	if (forward)
		forward->x = w.x, forward->y = w.y, forward->z = w.z;
	return 1;
}
