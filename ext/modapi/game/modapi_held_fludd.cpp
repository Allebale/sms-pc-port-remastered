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
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>

#include <math.h>
#include <string.h>
#include "port_framerate.h"

namespace {

bool s_on;
f32 s_right, s_down, s_forward, s_scale = 1.0f, s_aim = 1200.0f;

Vec s_target;           // where this frame's water goes
Vec s_viewEye, s_viewWay; // the view this frame (the rifle's aim)
f32 s_kick;              // a shot's recoil, 0 to 1, settling
f32 s_bobPhase, s_bobSize, s_bobLift; // the sway as he walks, runs and jumps
bool s_placed;          // placed this frame (s_target good)
Mtx s_lastBase;         // the base it was given (the same next frame: not posed again, frozen)
bool s_haveLast;
f32 s_inView[3][3];     // its last good turn in the view's own frame
bool s_haveInView;
int s_flipAfter;         // frames left to keep it still after a flip

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

void PortWeaponModelHide(); // modapi_weapon_models.cpp

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
		PortWeaponModelHide();
	}
	s_flipAfter = 0;
	return gpMarioOriginal ? 1 : 0;
}

// Is this Mario's FLUDD held in view now (drawn even with his models left out)?
bool PortSprayWeaponChosen(); // modapi_spray_weapons.cpp
int PortSprayWeaponId();
void PortWeaponModelShow(int which, MtxPtr viewModel); // modapi_weapon_models.cpp
void PortWeaponModelHide();
void PortWeaponModelKeep();
float PortWeaponModelMuzzle(int which);

f32 PortSprayWeaponShotPhase();
bool PortSprayWeaponOnBack(TMario* m, bool* model); // modapi_spray_weapons.cpp
bool PortSprayWeaponTrigger();
const float* PortWeaponPackMuzzle(int which); // modapi_weapon_models.cpp
void PortWeaponModelShowSecond(int which, MtxPtr viewModel);

bool PortFluddHeld(TMario* mario)
{
	if (!s_on || !mario || mario != gpMarioOriginal || mario->onYoshi())
		return false;
	TWaterGun* g = mario->mWaterGun;
	if (!g || !g->mFluddModel)
		return false;
	const int n = g->mCurrentNozzle;
	return n == TWaterGun::Spray || n == TWaterGun::Underwater;
}

// FLUDD on his back while a spray weapon fires from it (third person, SMS_SPRAY_THIRD_PERSON):
// the nozzle kept out while R is held and a moment after (the game then puts it away as ever),
// with a pump as each shot goes. With the weapons' models on (SMS_SPRAY_THIRD_PERSON_MODEL), all
// of FLUDD drawn as the weapon's pack instead (modapi_weapon_models.cpp), on his back as FLUDD is.
// Where its water starts (the shots, modapi_spray_weapons.cpp): the nozzle, or the pack's muzzle.
JGeometry::TVec3<f32> s_backMuzzle, s_backMario; // and where he was then
// Which of FLUDD's own axes (its base turn on his back) point forward and up, and their signs:
// learnt while he stands up straight, then kept, so the pack lies down with him in a dive or a
// belly slide as FLUDD does.
int s_packFwd = -1, s_packUp;
f32 s_packFwdSign, s_packUpSign;
bool s_haveBackMuzzle;
void onBack(TWaterGun* g, JDrama::TGraphics* graphics)
{
	s_haveBackMuzzle = false;
	bool model = false;
	if (!g->mFluddModel || !PortSprayWeaponOnBack(g->mMario, &model))
		return;
	const int weapon  = PortSprayWeaponId();
	const int which   = weapon == 2 ? 0 : weapon == 3 ? 1 : weapon == 4 ? 2 : -1; // the rifle, bazooka, submachine gun
	model             = model && which >= 0;
	const f32 phase   = PortSprayWeaponShotPhase();
	J3DModel* fm      = g->mFluddModel->getModel();
	if (model || phase >= 0.0f || PortSprayWeaponTrigger()) {
		g->unk1CEC = 0.0f;       // out
		g->unk1CFA = g->unk1CF8; // and the game's own wait before it puts it away
		g->mFluddModel->setBck(phase >= 0.0f && !model ? "wg_pump" : "wg_house");
		if (J3DFrameCtrl* ctrl = g->mFluddModel->getFrameCtrl(ANM_TYPE_BCK)) {
			ctrl->setRate(0.0f);
			ctrl->setFrame(phase >= 0.0f && !model ? phase * ctrl->getEnd() : 0.0f);
		}
		g->mFluddModel->calc();
		if (MActor* nozzle = g->getCurrentNozzle()->getMActor()) {
			nozzle->getModel()->setBaseTRMtx(fm->getAnmMtx(g->unk1CD8));
			nozzle->getModel()->calc();
		}
		g->setEmitPt();
	}
	MtxPtr em = g->getEmitMtx(0);
	s_backMario.set(g->mMario->mPosition.x, g->mMario->mPosition.y, g->mMario->mPosition.z);
	if (!model) {
		if (em && g->unk1CEC <= 0.0f) { // the nozzle's tip, while it is out
			s_backMuzzle.set(em[0][3], em[1][3], em[2][3]);
		} else { // put away: where it comes out from
			MtxPtr nc = fm->getAnmMtx(g->unk1CD8);
			s_backMuzzle.set(nc[0][3], nc[1][3], nc[2][3]);
		}
		s_haveBackMuzzle = true;
		return;
	}
	// the pack's frame: from FLUDD's nozzle_center, its turn on his back (its axes nearest the way
	// he faces and up), so it leans and sways with him as FLUDD does
	MtxPtr base   = fm->getBaseTRMtx();
	MtxPtr nc     = fm->getAnmMtx(g->unk1CD8);
	const f32 ang = g->mMario->mFaceAngle.y * (3.14159265f / 32768.0f);
	const Vec face = {sinf(ang), 0.0f, cosf(ang)}, worldUp = {0.0f, 1.0f, 0.0f};
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
	const u32 st = g->mMario->mStatus;
	if (bestF > 0.9f && best > 0.9f && (st == MARIO_STATUS_WAIT || st == MARIO_STATUS_RUN)) { // standing or running (not lying)
		s_packFwd     = fi, s_packUp = ui;
		s_packFwdSign = axis[fi].x * face.x + axis[fi].z * face.z < 0.0f ? -1.0f : 1.0f;
		s_packUpSign  = axis[ui].y < 0.0f ? -1.0f : 1.0f;
	}
	if (s_packFwd >= 0) { // as learnt: FLUDD's own axes, however he lies
		fi = s_packFwd, ui = s_packUp;
		axis[fi].x *= s_packFwdSign, axis[fi].y *= s_packFwdSign, axis[fi].z *= s_packFwdSign;
		axis[ui].x *= s_packUpSign, axis[ui].y *= s_packUpSign, axis[ui].z *= s_packUpSign;
	}
	Vec fwd = axis[fi], up = axis[ui];
	if (s_packFwd < 0) { // not learnt yet: the nearest
		if (fwd.x * face.x + fwd.z * face.z < 0.0f)
			fwd.x = -fwd.x, fwd.y = -fwd.y, fwd.z = -fwd.z;
		if (up.y < 0.0f)
			up.x = -up.x, up.y = -up.y, up.z = -up.z;
	}
	Vec right = cross(fwd, up);
	if (!unit(right))
		return;
	up = cross(right, fwd);
	unit(up);
	const Vec at = {nc[0][3], nc[1][3], nc[2][3]};
	Mtx place, viewModel;
	const f32 col[3][3] = {{right.x, fwd.x, up.x}, {right.y, fwd.y, up.y}, {right.z, fwd.z, up.z}};
	const f32 o[3]      = {at.x, at.y, at.z};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			place[r][c] = col[r][c];
		place[r][3] = o[r];
	}
	MTXConcat(graphics->getViewMtx(), place, viewModel);
	PortWeaponModelShow(3 + which, viewModel); // all of FLUDD left out, the pack in its place
	// its weapon on top, held level and pointing ahead whatever his body's lean as he walks or runs
	// (lying in a dive: the way his head points), kicking back and up as it fires, turned about
	// where it sits on the neck
	const f32 py = 2.0f, pz = 21.0f; // that point, in the pack's units (make_pack.py)
	const Vec pivot = {at.x + fwd.x * py + up.x * pz, at.y + fwd.y * py + up.y * pz, at.z + fwd.z * py + up.z * pz};
	// level while he is up (walking, running, jumping); lying down (a belly slide, asleep) along
	// him with the pack, as FLUDD's nozzle lies
	Vec ahead = up, gUp = {-fwd.x, -fwd.y, -fwd.z}; // lying: along him, the way his head is, its top off his back
	if (up.y > 0.6f) {
		ahead = fwd;
		ahead.y = 0.0f;
		if (!unit(ahead))
			ahead = face;
		gUp.x = 0.0f, gUp.y = 1.0f, gUp.z = 0.0f;
	}
	Vec gRight = cross(ahead, gUp);
	unit(gRight);
	Mtx level;
	const f32 gc[3][3] = {{gRight.x, ahead.x, gUp.x}, {gRight.y, ahead.y, gUp.y}, {gRight.z, ahead.z, gUp.z}};
	const f32 pv[3]    = {pivot.x, pivot.y, pivot.z};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			level[r][c] = gc[r][c];
		level[r][3] = pv[r] - (gc[r][1] * py + gc[r][2] * pz); // the pivot where it is on the pack
	}
	{
		const f32 kick = phase >= 0.0f ? (1.0f - phase) * (1.0f - phase) : 0.0f;
		const f32 back = kick * (which == 1 ? 9.0f : which == 0 ? 6.0f : 3.5f);
		const f32 tilt = kick * (which == 1 ? 0.2f : which == 0 ? 0.25f : 0.12f);
		const f32 c = cosf(tilt), s = sinf(tilt);
		Mtx kicked, gunWorld, gunView;
		MTXIdentity(kicked);
		kicked[1][1] = c, kicked[1][2] = -s, kicked[2][1] = s, kicked[2][2] = c;
		kicked[1][3] = py - (c * py - s * pz) - back;
		kicked[2][3] = pz - (s * py + c * pz);
		MTXConcat(level, kicked, gunWorld);
		MTXConcat(graphics->getViewMtx(), gunWorld, gunView);
		PortWeaponModelShowSecond(6 + which, gunView);
	}
	const float* mz = PortWeaponPackMuzzle(which); // the shots from its muzzle, held still
	s_backMuzzle.set(level[0][0] * mz[0] + level[0][1] * mz[1] + level[0][2] * mz[2] + level[0][3],
	                 level[1][0] * mz[0] + level[1][1] * mz[1] + level[1][2] * mz[2] + level[1][3],
	                 level[2][0] * mz[0] + level[2][1] * mz[1] + level[2][2] * mz[2] + level[2][3]);
	s_haveBackMuzzle = true;
}

// As FLUDD is calculated for the view (TWaterGun::perform): moved in front of it.
void PortFluddHeldPlace(TWaterGun* g, JDrama::TGraphics* graphics)
{
	s_placed = false;
	PortWeaponModelHide(); // shown again below, held with the bazooka chosen
	if (g && graphics && !PortFluddHeld(g->mMario))
		onBack(g, graphics);
	if (!g || !graphics || !PortFluddHeld(g->mMario))
		return;
	// its nozzle kept out, as the gun: idle, the game puts it away after a moment (the "house"
	// animation, unk1CEC from 0 out to 1 away), which would turn the whole of it round to face
	// the view (the nozzle's way is what is turned to the aim). Held at 0 for the next frame:
	// the idle step adds unk1CF4 to it, and with no delay left (unk1CFA) poses it there.
	// Pumping, 0 itself: the pump's own animation then (the normal spray). With a spray weapon
	// chosen its animation is set below, still but for a pump as it fires.
	const u32 upper = (g->mMario->mUpperState & 0x8000) ? 0 : g->mMario->mUpperState;
	g->unk1CFA      = 0;
	g->unk1CEC      = (upper == TMario::UPPER_STATE_PUMPING || upper == TMario::UPPER_STATE_HOLDING_PUMP) ? 0.0f : -g->unk1CF4;
	const bool nozzleAway = g->unk1CEC > 0.0f; // put away, or on its way (should the game's own do it)

	J3DModel* model = g->mFluddModel->getModel();
	MtxPtr base     = model->getBaseTRMtx();
	if (s_haveLast && memcmp(base, s_lastBase, sizeof(Mtx)) == 0) {
		s_placed = true; // not posed again this frame (Mario frozen): where it was, a weapon's model too
		if (PortSprayWeaponChosen())
			PortWeaponModelKeep();
		return;
	}
	// still, nozzle out (the put-away animation at its start), unless water comes out: the
	// normal spray's pump as the game has it while it sprays; a weapon's one pump as it fires.
	// (Otherwise the game may pump it along with Mario's arm: Super Mario Eclipse keeps him
	// holding the pump.)
	const bool weapon = PortSprayWeaponChosen();
	const bool overridden = weapon || !g->isEmitting();
	const f32 phase = weapon ? PortSprayWeaponShotPhase() : -1.0f;
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
		if (PortSprayWeaponChosen())
			PortWeaponModelKeep(); // not placed this frame: its model where it was
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
	Vec tip      = along(along(along(eye, s_right, R), -s_down, U), s_forward, F);
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
		if (PortSprayWeaponChosen())
			PortWeaponModelKeep(); // not placed this frame: its model where it was
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
		if (PortSprayWeaponChosen())
			PortWeaponModelKeep(); // not placed this frame: its model where it was
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

	// The rifle, the bazooka and the submachine gun: their own models in FLUDD's place (modapi_weapon_models.cpp), on
	// the shoulder, the muzzle a little past where the water starts and pointing at the aim,
	// tipped up by the kick; their own units to about 50 and 47 of the game's long.
	const int chosenWeapon = PortSprayWeaponId();
	if (chosenWeapon == 2 || chosenWeapon == 3 || chosenWeapon == 4) {
		const int which   = chosenWeapon == 2 ? 0 : chosenWeapon == 3 ? 1 : 2; // the rifle, bazooka, submachine gun
		const f32 scale   = which == 0 ? 12.5f : which == 1 ? 15.0f : 15.0f;
		const f32 ahead   = which == 0 ? 4.0f : which == 1 ? 6.0f : 4.0f; // the muzzle past the water's start
		Vec fwd = {toTarget.x + U.x * s_kick * 0.35f, toTarget.y + U.y * s_kick * 0.35f, toTarget.z + U.z * s_kick * 0.35f};
		unit(fwd);
		Vec right = cross(fwd, U);
		if (unit(right)) {
			const Vec up     = cross(right, fwd);
			const Vec muzzle = along(tip, ahead, fwd);
			const Vec origin = along(muzzle, -PortWeaponModelMuzzle(which) * scale, fwd);
			Mtx world, viewModel;
			const f32 col[3][3] = {{right.x, fwd.x, up.x}, {right.y, fwd.y, up.y}, {right.z, fwd.z, up.z}};
			const f32 at[3]     = {origin.x, origin.y, origin.z};
			for (int row = 0; row < 3; row++) {
				for (int c2 = 0; c2 < 3; c2++)
					world[row][c2] = col[row][c2] * scale;
				world[row][3] = at[row];
			}
			MTXConcat(v, world, viewModel);
			PortWeaponModelShow(which, viewModel);
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
	if (overridden && phase >= 0.0f) { // a weapon's pump as it fires
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
}

// The water's way from the held FLUDD (TWaterGun::getEmitPosDirSpeed): to the view's aim.
bool PortFluddHeldWay(TWaterGun* g, const JGeometry::TVec3<f32>& from, JGeometry::TVec3<f32>* way)
{
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
	if (!s_on || !mario || mario != gpMarioOriginal || mario->onYoshi())
		return false;
	const u32 status = mario->getStatus();
	return (status == MARIO_STATUS_BACK_JUMP && anm_id == 0xF7)
	       || (status == MARIO_STATUS_U_TURN_JUMP && anm_id == TMario::ANIM_TJMP1);
}

// The view this frame, while FLUDD is held in it (the spray weapons' aim): the eye and the way
// it looks (a unit vector).
bool PortFluddHeldView(JGeometry::TVec3<f32>* eye, JGeometry::TVec3<f32>* forward)
{
	if (!s_placed)
		return false;
	eye->set(s_viewEye.x, s_viewEye.y, s_viewEye.z);
	forward->set(s_viewWay.x, s_viewWay.y, s_viewWay.z);
	return true;
}

// Where a weapon's shot starts with FLUDD on his back (third person): its nozzle's tip, or the
// weapon's pack's muzzle; false when neither is known this frame.
bool PortFluddBackMuzzle(JGeometry::TVec3<f32>* at)
{
	TMario* m = gpMarioOriginal;
	if (!s_haveBackMuzzle || !m)
		return false;
	// placed as last drawn: moved on with him since (the shot goes after his move this frame)
	at->set(s_backMuzzle.x + m->mPosition.x - s_backMario.x, s_backMuzzle.y + m->mPosition.y - s_backMario.y,
	        s_backMuzzle.z + m->mPosition.z - s_backMario.z);
	return true;
}

// A shot's recoil for the held gun (the spray weapons): 0 to 1.
void PortFluddHeldKick(f32 amount)
{
	if (amount > s_kick)
		s_kick = amount > 1.0f ? 1.0f : amount;
}
