// FLUDD's spray weapons (api->held_fludd->set_weapon, combined-plus SDK): while a first-person mod
// holds FLUDD in view (modapi_held_fludd.cpp), the spray nozzle may fire as a weapon instead of
// its stream. Still water, FLUDD's own drops, only fired another way:
//
//   Shotgun  a spread of 8 drops at once, short reach, a strong shove
//   Rifle    3 fast drops in a line, far and straight, a light shove
//   Bazooka  one huge slow drop, a very strong shove, cleans a wide patch
//
// Each fires while R is held, again once it is ready (its own rate), using the tank's water
// as so many drops of the stream would. Their drops carry the weapon in their attack's upper bits
// (TModelWaterManager::getParticleAttack gives the game only the lower ones, patch
// zzzzzzz-held-fludd-4-weapons), so when one reaches an enemy (objectHitDone) it is shoved the
// drop's way, harder or softer by the weapon, and with "defeat enemies" on, defeated after a few
// hits (TSmallEnemy::kill, as a stomp). Shoved into a wall, those the game defeats against walls
// (Strollin' Stus, rolling) are defeated, as ever. The normal spray (weapon 0) is the game's own
// stream, its pushes and all: nothing is added to it. Only small enemies (TSmallEnemy) are shoved
// or defeated; bosses are not.
#include "port_ext.h"

#include <Enemy/SmallEnemy.hpp>
#include <Enemy/WalkerEnemy.hpp>
#include <Enemy/HamuKuri.hpp>
#include <Enemy/AmiNoko.hpp>
#include <Enemy/BossTelesaObj.hpp>
#include <Enemy/Telesa.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/NozzleBase.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/HitActor.hpp>

#include <math.h>
#include <string.h>

extern "C" int port_cheat_infinite_fludd;
bool PortFluddHeld(TMario* mario);

namespace {

enum { NORMAL = 0, SHOTGUN = 1, RIFLE = 2, BAZOOKA = 3, WEAPONS = 4 };
const int kTagShift = 8; // the weapon in a drop's attack, above the game's own (1 to 255)

struct Weapon {
	int drops;     // at once
	f32 pow;       // times the stream's speed
	f32 size;      // times the stream's drop size
	f32 hit;       // times its reach (hit radius and height)
	f32 spread;    // radians either way (a random cone)
	int every;     // frames between shots
	int water;     // the tank's water used per shot, in the stream's drops
	f32 recoil;    // Mario pushed back
	f32 shove;     // an enemy pushed, units a frame (slowing down)
	int toDefeat;  // hits to defeat (with defeat enemies on), within kHitWindow
};
const Weapon kWeapons[WEAPONS] = {
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                      // normal: the game's own
	{8, 0.95f, 1.25f, 1.2f, 0.13f, 22, 12, 6.0f, 20.0f, 4},  // shotgun
	{3, 1.9f, 0.7f, 1.0f, 0.0f, 9, 4, 1.5f, 9.0f, 2},       // rifle
	{1, 1.15f, 4.0f, 3.0f, 0.0f, 45, 30, 14.0f, 34.0f, 1},  // bazooka
};
const int kHitWindow = 45; // frames: hits counted together towards a defeat
const f32 kSlow      = 0.82f;

int s_weapon;   // the one chosen
bool s_defeat;  // weapons may defeat enemies
u32 s_frame;    // frames played (afterMovement)
u32 s_ready;    // the frame the next shot may go
u32 s_seed = 12345u;

f32 random01()
{
	s_seed = s_seed * 1664525u + 1013904223u;
	return (f32)((s_seed >> 8) & 0xffff) / 65535.0f;
}

// a direction turned a little at random, within `spread` radians
void scatter(JGeometry::TVec3<f32>& d, f32 spread)
{
	if (spread <= 0.0f)
		return;
	JGeometry::TVec3<f32> side(d.z, 0.0f, -d.x); // across, level
	f32 l = sqrtf(side.x * side.x + side.z * side.z);
	if (l < 1e-4f)
		side.set(1.0f, 0.0f, 0.0f), l = 1.0f;
	side.x /= l, side.z /= l;
	JGeometry::TVec3<f32> upw(d.y * side.z - d.z * 0.0f, d.z * side.x - d.x * side.z, d.x * 0.0f - d.y * side.x);
	const f32 a = (random01() * 2.0f - 1.0f) * spread, b = (random01() * 2.0f - 1.0f) * spread;
	d.x += side.x * a + upw.x * b;
	d.y += side.y * a + upw.y * b;
	d.z += side.z * a + upw.z * b;
	l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
	if (l > 1e-4f)
		d.x /= l, d.y /= l, d.z /= l;
}

// the enemies shoved (and hit) lately
struct Hit {
	TSmallEnemy* enemy;
	f32 vx, vz;
	int hits;
	u32 last;
};
const int kMostHits = 32;
Hit s_hits[kMostHits];

Hit* hitOf(TSmallEnemy* e)
{
	Hit* slot = 0;
	for (int i = 0; i < kMostHits; i++) {
		if (s_hits[i].enemy == e)
			return &s_hits[i];
		if (!slot && (!s_hits[i].enemy || s_frame - s_hits[i].last > 300))
			slot = &s_hits[i];
	}
	if (!slot)
		return 0;
	memset(slot, 0, sizeof *slot);
	slot->enemy = e;
	return slot;
}

void stageSetup()
{
	memset(s_hits, 0, sizeof s_hits);
	s_ready = 0;
}

// the shoves, each frame: along the ground, stopped by a wall, slowing down
void afterMovement()
{
	s_frame++;
	for (int i = 0; i < kMostHits; i++) {
		Hit& h = s_hits[i];
		if (!h.enemy || (h.vx * h.vx + h.vz * h.vz) < 0.09f)
			continue;
		TSmallEnemy* e = h.enemy;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD)) {
			h.vx = h.vz = 0.0f;
			continue;
		}
		JGeometry::TVec3<f32> from(e->mPosition.x, e->mPosition.y + 40.0f, e->mPosition.z);
		JGeometry::TVec3<f32> to(from.x + h.vx * 1.5f, from.y, from.z + h.vz * 1.5f);
		JGeometry::TVec3<f32> wall;
		if (gpMap && gpMap->intersectLine(from, to, true, &wall)) {
			// against a wall: those the game defeats against walls (Strollin' Stus and their
			// kind, rolling away from the water) are defeated so here too, hard enough
			THamuKuri* stu = dynamic_cast<THamuKuri*>(e);
			if (stu && h.vx * h.vx + h.vz * h.vz > 6.0f * 6.0f
			    && stu->getSpine()->getCurrentNerve() != &TNerveHamuKuriWallDie::theNerve()) {
				stu->getSpine()->reset();
				stu->getSpine()->setNext(&TNerveHamuKuriWallDie::theNerve());
			}
			h.vx = h.vz = 0.0f;
			continue;
		}
		e->mPosition.x += h.vx;
		e->mPosition.z += h.vz;
		h.vx *= kSlow;
		h.vz *= kSlow;
	}
}

// a drop reached something: if it is a weapon's and an enemy, shoved (and maybe defeated)
void objectHitDone(void* object, void* sender, unsigned message, int)
{
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender || !gpModelWaterManager)
		return;
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	TModelWaterManager* w = gpModelWaterManager;
	if (!drop || drop->mParticleIndex < 0 || drop->mParticleIndex >= (int)w->mParticleCount)
		return;
	const int weapon = (w->mParticleAttackSOA[drop->mParticleIndex] >> kTagShift) & 0xf;
	if (weapon <= NORMAL || weapon >= WEAPONS)
		return; // the normal spray, or any other water: the game's own
	TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(static_cast<THitActor*>(object));
	if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	Hit* h = hitOf(e);
	if (!h)
		return;
	const Weapon& k = kWeapons[weapon];
	// shoved: those that walk about, not those on fences (net Koopas), the boss's bubbles or the
	// shadow Mario look-alikes; the rooted and flying are not moved, only hit
	const bool moves = dynamic_cast<TWalkerEnemy*>(e) && !dynamic_cast<TAmiNoko*>(e) && !dynamic_cast<TBubble*>(e)
	                   && !dynamic_cast<TKageMarioModoki*>(e);
	const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[drop->mParticleIndex];
	f32 l = sqrtf(v.x * v.x + v.z * v.z);
	if (l < 1e-3f) // straight down: away from Mario
		l = 0.0f;
	const f32 dx = l > 0.0f ? v.x / l : 0.0f, dz = l > 0.0f ? v.z / l : 0.0f;
	if (moves && h->vx * h->vx + h->vz * h->vz < k.shove * k.shove) { // the strongest shove, not their sum
		h->vx = dx * k.shove;
		h->vz = dz * k.shove;
	}
	if (s_frame - h->last > (u32)kHitWindow)
		h->hits = 0;
	h->hits++;
	h->last = s_frame;
	if (s_defeat && h->hits >= k.toDefeat) {
		h->hits = 0;
		e->kill(); // as a stomp: its own defeat, coins and all
	}
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "spray weapons";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.objectHitDone = objectHitDone;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// api->held_fludd->set_weapon (mod_runtime.cpp): 0 normal, 1 shotgun, 2 rifle, 3 bazooka; flags 1:
// weapons may defeat enemies
extern "C" int modg_spray_weapon_set(int weapon, int flags)
{
	s_weapon = weapon > NORMAL && weapon < WEAPONS ? weapon : NORMAL;
	s_defeat = (flags & 1) != 0;
	return 1;
}

// In place of the spray nozzle's emit (TNozzleBase::emit, patch zzzzzzz-held-fludd-4-weapons): true
// when a weapon fires instead (or waits to), false for the game's own stream.
bool PortSprayWeaponEmit(TNozzleBase* n, int emitter)
{
	if (s_weapon == NORMAL || !n || !n->mFludd)
		return false;
	TWaterGun* g = n->mFludd;
	if (!PortFluddHeld(g->mMario) || g->mCurrentNozzle != TWaterGun::Spray || g->getCurrentNozzle() != n)
		return false;
	if (emitter != 0 || g->mCurrentWater <= 0 || n->unk378 == 0.0f || s_frame < s_ready)
		return true;
	const Weapon& k = kWeapons[s_weapon];
	s_ready         = s_frame + (u32)k.every;

	TWaterEmitInfo* info = g->mEmitInfo;
	n->emitCommon(0, info); // where, which way (the view's aim), and the stream's own drop
	const JGeometry::TVec3<f32> way = info->mDir.value;
	const f32 pow  = n->mEmitParams.mEmitPow.get() * k.pow;
	const s16 base = n->mEmitParams.mAttack.get();
	info->mNum.set(1);
	info->mAttack.set((s16)(((base > 0 && base < 256) ? base : 1) | (s_weapon << kTagShift)));
	info->mSize.set(n->mEmitParams.mSize.get() * k.size);
	info->mHitRadius.set(info->mHitRadius.get() * k.hit);
	info->mHitHeight.set(info->mHitHeight.get() * k.hit);
	info->mDirTremble.set(0.0f);
	info->mPowTremble.set(0.0f);
	info->mFlag.set(0x40);
	int made = 0;
	for (int i = 0; i < k.drops; i++) {
		JGeometry::TVec3<f32> d = way;
		scatter(d, k.spread);
		info->mDir.value = d;
		info->mPow.set(pow * (1.0f - 0.05f * (f32)i)); // the rifle's in a line, one behind another
		made += gpModelWaterManager->emitRequest(*info) ? 1 : 0;
	}
	if (!made)
		return true;
	if (!(port_cheat_infinite_fludd && g->getMario()->isMario()))
		g->depleteWater(k.water * (n->mEmitParams.mDecRate.get() > 0 ? n->mEmitParams.mDecRate.get() : 1));
	TMario* m = g->getMario(); // the kick, back along the shot
	m->mVel.x -= way.x * k.recoil;
	m->mVel.z -= way.z * k.recoil;
	return true;
}
