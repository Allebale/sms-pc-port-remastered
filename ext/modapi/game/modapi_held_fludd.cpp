// FLUDD held in view (api->held_fludd, combined-plus SDK): for a first-person camera, the game's own
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

// api->held_fludd->set (mod_runtime.cpp)
extern "C" int modg_held_fludd_set(int on, float right, float down, float forward, float scale, float aim)
{
	s_on      = on != 0;
	s_right   = right;
	s_down    = down;
	s_forward = forward;
	s_scale   = scale > 0.05f && scale < 4.0f ? scale : 1.0f;
	s_aim     = aim > 50.0f ? aim : 1200.0f;
	if (!s_on)
		s_placed = s_haveLast = s_haveInView = false;
	s_flipAfter = 0;
	return gpMarioOriginal ? 1 : 0;
}

// Is this Mario's FLUDD held in view now (drawn even with his models left out)?
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

// As FLUDD is calculated for the view (TWaterGun::perform): moved in front of it.
void PortFluddHeldPlace(TWaterGun* g, JDrama::TGraphics* graphics)
{
	s_placed = false;
	if (!g || !graphics || !PortFluddHeld(g->mMario))
		return;
	J3DModel* model = g->mFluddModel->getModel();
	MtxPtr base     = model->getBaseTRMtx();
	if (s_haveLast && memcmp(base, s_lastBase, sizeof(Mtx)) == 0) {
		s_placed = true; // not posed again this frame (Mario frozen): where it was
		return;
	}
	MtxPtr em = g->getEmitMtx(0);
	if (!em)
		return;

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
	if (!unit(way))
		return;

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
		if (!s_haveInView || !flip) {
			memcpy(s_inView, inView, sizeof inView);
			s_haveInView = true;
		}
	}
	if (!s_haveInView)
		return;
	f32 turned[3][3];
	for (int r = 0; r < 3; r++) // the view's turn back to the world (its transpose)
		for (int c = 0; c < 3; c++)
			turned[r][c] = v[0][r] * s_inView[0][c] + v[1][r] * s_inView[1][c] + v[2][r] * s_inView[2][c];

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
	model->calc();
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

// A shot's recoil for the held gun (the spray weapons): 0 to 1.
void PortFluddHeldKick(f32 amount)
{
	if (amount > s_kick)
		s_kick = amount > 1.0f ? 1.0f : amount;
}
