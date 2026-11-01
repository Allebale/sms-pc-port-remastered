// Wet Characters (port_puddles.h): the people and enemies get wet as Mario does, with a look of their
// own (the menu's Wet Characters and Wet Characters Shine), all over when FLUDD's water hits them (the hit's objectHitDone, as each
// water particle's HIT_MESSAGE_SPRAYED_BY_WATER reaches them), or up to where the water reaches
// when they stand or swim in it; drying over 45 seconds as he does.
//
// The renderer sees no one actor's draw (the game's draw buffers mix them), but it knows the
// characters' display lists (registered at stage setup for Round Shadows, port_shadows.cpp). So each
// frame, as the main camera sets its view (port_wet_characters_view), the wettest few near Mario go
// to it as spheres round them in the camera's view, with how wet each is and its waterline; a
// character's draw is wet where it is inside one of them.
#include "port_ext.h"
#include "port_puddles.h"

#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Enemy.hpp>
#include <M3DUtil/MActor.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <dolphin/os.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" void GXPC_SetWetCharacters(int count, const float* data, float darkness, float shine, const float* up);

namespace {

const int kMost       = 48;        // characters wet at once (the wettest kept)
const int kDrawn      = 8;         // sent to the renderer: the wettest near Mario (gx_shader.cpp)
const s32 kDryMs      = 45000;     // as Wet Mario
const f32 kAllOver    = 100000.0f; // a waterline over all of them
const f32 kNear       = 3000.0f;   // only these near Mario are checked for water, and drawn wet
const f32 kMargin     = 35.0f;     // the sphere past their joints (their skin round the bones)
const f32 kLargest    = 450.0f;    // no sphere larger (a boss's)
const f32 kDripsPerSec = 40.0f;    // soaked and still; fewer as they dry (by wetness squared), more moving

struct Wet {
	TLiveActor* actor;
	f32 wet;     // 1 soaked, 0 dry
	f32 height;  // how far up, over their feet (kAllOver: all of them)
};
Wet s_wet[kMost];
int s_count;
s32 s_last;
f32 s_dripDue[kDrawn];
// each drawn one's joints last frame, from its position (its own animation, not its walking)
const int kJoints = 64;
f32 s_prevJoint[kDrawn][kJoints][3];
bool s_prevValid[kDrawn];
u32 s_rand = 777u;

f32 rand01()
{
	s_rand = s_rand * 1664525u + 1013904223u;
	return (f32)(s_rand >> 8) * (1.0f / 16777216.0f);
}
int s_next;  // the next actor to check for water (a few each frame)

bool character(TLiveActor* a) { return a && a->getMActor() && dynamic_cast<TSpineEnemy*>(a); }

Wet* find(TLiveActor* a, bool add)
{
	for (int i = 0; i < s_count; i++)
		if (s_wet[i].actor == a)
			return &s_wet[i];
	if (!add)
		return 0;
	int slot = s_count < kMost ? s_count++ : -1;
	if (slot < 0) {  // full: the driest goes
		slot = 0;
		for (int i = 1; i < kMost; i++)
			if (s_wet[i].wet < s_wet[slot].wet)
				slot = i;
	}
	s_wet[slot].actor  = a;
	s_wet[slot].wet    = 0.0f;
	s_wet[slot].height = 0.0f;
	return &s_wet[slot];
}

void soak(TLiveActor* a, f32 height)
{
	Wet* w = find(a, true);
	if (w->wet <= 0.0f)
		fprintf(stderr, "[puddles] a character is wet (%s)\n", height >= kAllOver ? "sprayed" : "in water");
	w->wet = 1.0f;
	if (height > w->height)
		w->height = height;
}

f32 distSq(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	const f32 x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
	return x * x + y * y + z * z;
}

// The water over an actor's feet, if it stands or swims in it: how deep (0 none).
f32 waterDepth(TLiveActor* a)
{
	if (!gpMap)
		return 0.0f;
	const JGeometry::TVec3<f32>& p = a->mPosition;
	const TBGCheckData* d = 0;
	const f32 h = gpMap->checkGround(p.x, p.y + 400.0f, p.z, &d);
	if (!d || !d->isWaterSurface() || h <= p.y + 1.0f)
		return 0.0f;
	return h - p.y;
}

// A few actors near Mario each frame, checked for water over their feet.
void checkWater()
{
	TMario* m = gpMarioOriginal;
	if (!gpConductor || !m)
		return;
	int index = 0, checked = 0;
	const int from = s_next;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++, index++) {
			if (index < from || checked >= 12)
				continue;
			TLiveActor* a = (*it)->getObj(i);
			checked++;
			if (!character(a) || distSq(a->mPosition, m->mPosition) > kNear * kNear)
				continue;
			const f32 depth = waterDepth(a);
			if (depth > 0.0f)
				soak(a, depth > 150.0f ? kAllOver : depth + 8.0f);
		}
	s_next = checked >= 12 ? from + checked : 0;  // round again from the first
}

// The water's surface over an actor's feet, if it stands in water (-1e9 none).
f32 waterSurface(TLiveActor* a)
{
	const f32 depth = waterDepth(a);
	return depth > 0.0f ? a->mPosition.y + depth : -1e9f;
}

// Drops off a wet one, as Mario's (port_wet_drip): from its joints below how far up it is wet, a
// little out from its middle, above the water it stands in; more the wetter it is, the faster it
// moves (jumping, running) and the more its animation moves its body (shaking, waving: its joints
// against its position since last frame), flung the way the joint swings; none under the water.
void drip(const Wet& w, int slot, f32* due, f32 dtMs)
{
	TLiveActor* a   = w.actor;
	J3DModel* model = a->getMActor() ? a->getMActor()->getModel() : 0;
	const int joints = model && model->getModelData() ? model->getModelData()->getJointNum() : 0;
	if (joints <= 1 || dtMs <= 0.0f)
		return;
	const f32 waterY = waterSurface(a);
	if (waterY > a->mPosition.y + 150.0f) {  // under the water
		*due = 0.0f;
		return;
	}
	const JGeometry::TVec3<f32>& v = a->getVelocity();  // units a frame
	const f32 speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	// the animation: each joint's move since last frame (from its position), in units a game frame
	const int nj = joints < kJoints ? joints : kJoints;
	f32 (*prev)[3] = s_prevJoint[slot];
	f32 jv[kJoints][3];
	f32 anim = 0.0f;
	const f32 perFrame = 33.3f / dtMs;  // to units a frame (30 a second)
	for (int j = 0; j < nj; j++) {
		MtxPtr mt = model->getAnmMtx(j);
		const f32 rel[3] = {mt[0][3] - a->mPosition.x, mt[1][3] - a->mPosition.y, mt[2][3] - a->mPosition.z};
		for (int k = 0; k < 3; k++) {
			jv[j][k]   = s_prevValid[slot] ? (rel[k] - prev[j][k]) * perFrame : 0.0f;
			prev[j][k] = rel[k];
		}
		anim += sqrtf(jv[j][0] * jv[j][0] + jv[j][1] * jv[j][1] + jv[j][2] * jv[j][2]);
	}
	const bool moved = s_prevValid[slot];
	s_prevValid[slot] = true;
	anim = moved ? anim / (f32)nj : 0.0f;  // a joint's average move a frame
	if (anim > 30.0f)  // a jump to a new pose (teleported, a cut): not a shake
		anim = 0.0f;
	*due += kDripsPerSec * w.wet * w.wet
	        * (1.0f + (speed < 20.0f ? speed : 20.0f) / 6.0f + (anim < 12.0f ? anim : 12.0f) / 1.5f) * dtMs * 0.001f;
	if (*due > 8.0f)
		*due = 8.0f;
	const f32 top = w.height >= kAllOver ? 1e30f : a->mPosition.y + w.height;
	for (; *due >= 1.0f; *due -= 1.0f)
		for (int tries = 0; tries < 4; tries++) {
			const int ji = 1 + (int)(rand01() * (joints - 1)) % (joints - 1);
			MtxPtr j = model->getAnmMtx(ji);
			f32 x = j[0][3], y = j[1][3], z = j[2][3];
			// flung the way the joint swings, as well as the way it walks
			const f32 sx = ji < nj ? jv[ji][0] : 0.0f, sy = ji < nj ? jv[ji][1] : 0.0f, sz = ji < nj ? jv[ji][2] : 0.0f;
			if (y > top || y < waterY + 2.0f || y < a->mPosition.y + 3.0f)
				continue;
			f32 ox = x - a->mPosition.x, oz = z - a->mPosition.z;
			const f32 d = sqrtf(ox * ox + oz * oz);
			if (d > 0.5f)
				ox /= d, oz /= d;
			else
				ox = rand01() - 0.5f, oz = rand01() - 0.5f;
			x += ox * (5.0f + 4.0f * rand01()) + (rand01() - 0.5f) * 6.0f;
			z += oz * (5.0f + 4.0f * rand01()) + (rand01() - 0.5f) * 6.0f;
			port_wet_drip(x, y - 2.0f * rand01(), z, v.x + sx, v.y + sy, v.z + sz, waterY);
			break;
		}
}

// The sphere round an actor's model (its joints), in the world: false if it has none.
bool sphere(TLiveActor* a, f32* c, f32* r)
{
	J3DModel* model = a->getMActor() ? a->getMActor()->getModel() : 0;
	const int joints = model && model->getModelData() ? model->getModelData()->getJointNum() : 0;
	if (joints <= 0)
		return false;
	f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (int j = 0; j < joints; j++) {
		MtxPtr mt = model->getAnmMtx(j);
		for (int k = 0; k < 3; k++) {
			if (mt[k][3] < lo[k])
				lo[k] = mt[k][3];
			if (mt[k][3] > hi[k])
				hi[k] = mt[k][3];
		}
	}
	f32 rr = 0.0f;
	for (int k = 0; k < 3; k++) {
		c[k] = 0.5f * (lo[k] + hi[k]);
		rr += (hi[k] - c[k]) * (hi[k] - c[k]);
	}
	*r = sqrtf(rr) + kMargin;
	if (*r > kLargest)
		*r = kLargest;
	return true;
}

// FLUDD's water (any water particle) reached an actor.
void objectHitDone(void* object, void*, unsigned message, int)
{
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !port_wet_characters())
		return;
	TLiveActor* a = dynamic_cast<TLiveActor*>(static_cast<THitActor*>(object));
	if (character(a))
		soak(a, kAllOver);
}

// A new stage: no one is wet (last stage's actors are gone).
void stageSetup()
{
	s_count = 0;
	s_next  = 0;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "wet characters";
		s_extension.stageSetup    = stageSetup;
		s_extension.objectHitDone = objectHitDone;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// Once a frame, from the main camera's view (port_shadows_main_view): the wet ones dry, a few are
// checked for water, and the wettest near Mario go to the renderer in this view.
void port_wet_characters_view(float (*view)[4])
{
	const s32 now = (s32)OSTicksToMilliseconds(OSGetTime());
	const s32 dt  = s_last ? now - s_last : 0;
	s_last        = now;
	TMario* m     = gpMarioOriginal;
	if (!port_wet_characters() || !view || !m) {
		s_count = 0;
		GXPC_SetWetCharacters(0, 0, 1.0f, 1.0f, 0);
		return;
	}
	if (dt > 0 && dt < 1000)
		for (int i = 0; i < s_count; i++)
			s_wet[i].wet -= (f32)dt / (f32)kDryMs;
	for (int i = 0; i < s_count;)  // the dry ones go
		if (s_wet[i].wet <= 0.0f)
			s_wet[i] = s_wet[--s_count];
		else
			i++;
	checkWater();
	// the wettest near Mario (still alive: in a manager's list this frame)
	int pick[kDrawn];
	int picked = 0;
	bool used[kMost];
	memset(used, 0, sizeof used);
	for (int n = 0; n < kDrawn; n++) {
		int best = -1;
		for (int i = 0; i < s_count; i++)
			if (!used[i] && distSq(s_wet[i].actor->mPosition, m->mPosition) < kNear * kNear
			    && (best < 0 || s_wet[i].wet > s_wet[best].wet))
				best = i;
		if (best < 0)
			break;
		used[best]     = true;
		pick[picked++] = best;
	}
	// their drops (as each frame, so a pick's count follows it while it stays the same one)
	static TLiveActor* s_dripOf[kDrawn];
	for (int n = 0; n < picked; n++) {
		if (s_dripOf[n] != s_wet[pick[n]].actor) {
			s_dripOf[n]    = s_wet[pick[n]].actor;
			s_dripDue[n]   = 0.0f;
			s_prevValid[n] = false;  // another one: its joints from the next frame
		}
		drip(s_wet[pick[n]], n, &s_dripDue[n], dt > 0 && dt < 1000 ? (f32)dt : 0.0f);
	}
	// each: its sphere in the view (centre, radius), how wet, its waterline (up . p + d = 0 there)
	float data[kDrawn * 8];
	int count = 0;
	for (int n = 0; n < picked; n++) {
		const Wet& w = s_wet[pick[n]];
		f32 c[3], r;
		if (!sphere(w.actor, c, &r))
			continue;
		float* o = data + count * 8;
		for (int k = 0; k < 3; k++)
			o[k] = view[k][0] * c[0] + view[k][1] * c[1] + view[k][2] * c[2] + view[k][3];
		o[3] = r;
		o[4] = w.wet;
		// the line at world height y: the view's up is its second column; up . v = y - (up . t)
		if (w.height >= kAllOver)
			o[5] = -kAllOver;
		else {
			const f32 y = w.actor->mPosition.y + w.height;
			o[5] = -y - (view[0][1] * view[0][3] + view[1][1] * view[1][3] + view[2][1] * view[2][3]);
		}
		o[6] = o[7] = 0.0f;
		count++;
	}
	const float up[3] = {view[0][1], view[1][1], view[2][1]};
	GXPC_SetWetCharacters(count, data, port_wet_characters_darkness(), port_wet_characters_shine(), up);
}
