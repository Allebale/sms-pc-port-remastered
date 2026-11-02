// FLUDD's spray weapons (api->held_fludd->set_weapon, combined-plus SDK): while a first-person mod
// holds FLUDD in view (modapi_held_fludd.cpp), the spray may fire as a weapon instead of its
// stream. Still water, FLUDD's own drops, only fired another way (Allebale's numbers):
//
//              shot                               reach      shove                 to defeat  every
//   Shotgun    8 drops in a spread                close      light                 2 shots    1.5 s
//   Rifle      3 fast drops in a line             very long  strong                1 shot     3 s
//   Bazooka    one huge drop, bursting on a hit   long       very strong, thrown   1 shot     10 s
//                                                            up into the air        (all in its burst)
//
// Each fires while R is held, again once it is ready, using the tank's water as so many drops of
// the stream would. Their drops carry the weapon and the shot in their attack's upper bits
// (TModelWaterManager::getParticleAttack gives the game only the lower ones, patch
// zzzzzzz-held-fludd-4-weapons), so when one reaches an enemy (objectHitDone) it is shoved the
// drop's way, and with "defeat enemies" on, defeated after so many shots (TSmallEnemy::kill, as a
// stomp; a shot's drops count once). The bazooka's drop bursts where it first hits: every small
// enemy near is thrown and (with defeat on) defeated. Shoved into a wall, those the game defeats
// against walls (Strollin' Stus) are defeated, as ever. The normal spray (weapon 0) is the game's
// own stream, its pushes and all: nothing is added to it. Only small enemies (TSmallEnemy) are
// shoved or defeated; bosses are not. Times are in seconds and shoves in the game's 30-a-second
// units, whatever the frame rate (port_frame_scale).
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
#include <Strategic/LiveManager.hpp>
#include <Enemy/Conductor.hpp>
#include "port_framerate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" int port_cheat_infinite_fludd;
bool PortFluddHeld(TMario* mario);

namespace {

enum { NORMAL = 0, SHOTGUN = 1, RIFLE = 2, BAZOOKA = 3, WEAPONS = 4 };
const int kTagShift = 8; // the weapon in a drop's attack, above the game's own (1 to 255)

const int kShotShift = 12; // and the shot (1 to 7, round again), above the weapon

struct Weapon {
	int drops;     // at once
	f32 pow;       // times the stream's speed (reach)
	f32 alive;     // times the stream's drop life (reach)
	f32 size;      // times the stream's drop size
	f32 hit;       // times its reach (hit radius and height)
	f32 spread;    // radians either way (a random cone)
	f32 every;     // seconds between shots
	int water;     // the tank's water used per shot, in the stream's drops
	f32 recoil;    // Mario pushed back
	f32 shove;     // an enemy pushed, units a 30th of a second (slowing down)
	f32 lift;      // and thrown up, the same units
	int toDefeat;  // shots to defeat (with defeat enemies on), within kHitWindow
	f32 burst;     // a burst's radius where it first hits (0 none)
};
const Weapon kWeapons[WEAPONS] = {
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                             // normal: the game's own
	{8, 0.75f, 0.35f, 1.25f, 1.2f, 0.14f, 1.5f, 12, 6.0f, 9.0f, 0.0f, 2, 0},     // shotgun
	{3, 2.6f, 1.6f, 0.7f, 1.0f, 0.0f, 3.0f, 6, 3.0f, 26.0f, 0.0f, 1, 0},         // rifle
	{1, 1.8f, 1.4f, 4.0f, 3.0f, 0.0f, 10.0f, 30, 16.0f, 34.0f, 38.0f, 1, 400.0f}, // bazooka
};
const f32 kHitWindow = 4.0f; // seconds: shots counted together towards a defeat
const f32 kSlow      = 0.82f; // a shove kept each 30th of a second

const char* const kNames[WEAPONS] = {"normal spray", "shotgun", "rifle", "bazooka"};
int s_told = -1; // the weapon last logged as fired
int s_weapon;   // the one chosen
bool s_defeat;  // weapons may defeat enemies
f32 s_time;     // seconds played (afterMovement)
f32 s_ready;    // when the next shot may go
int s_shot;     // the last shot's number (1 to 7)
int s_burst;    // the last shot (weapon and number) that burst
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
	int lastShot; // the shot (weapon and number) last counted: a shot's drops count once
	f32 last;     // when
};
const int kMostHits = 32;
Hit s_hits[kMostHits];

Hit* hitOf(TSmallEnemy* e)
{
	Hit* slot = 0;
	for (int i = 0; i < kMostHits; i++) {
		if (s_hits[i].enemy == e)
			return &s_hits[i];
		if (!slot && (!s_hits[i].enemy || s_time - s_hits[i].last > 10.0f))
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
	s_ready = 0.0f;
	s_burst = 0;
}

// the shoves, each frame: along the ground, stopped by a wall, slowing down
void afterMovement()
{
	const f32 dt = port_frame_scale(); // 30ths of a second this frame
	s_time += dt / 30.0f;
	const f32 keep = powf(kSlow, dt);
	for (int i = 0; i < kMostHits; i++) {
		Hit& h = s_hits[i];
		if (!h.enemy || (h.vx * h.vx + h.vz * h.vz) < 0.09f)
			continue;
		TSmallEnemy* e = h.enemy;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD)) {
			h.vx = h.vz = 0.0f;
			continue;
		}
		const f32 mx = h.vx * dt, mz = h.vz * dt;
		JGeometry::TVec3<f32> from(e->mPosition.x, e->mPosition.y + 40.0f, e->mPosition.z);
		JGeometry::TVec3<f32> to(from.x + mx * 1.5f + h.vx * 0.5f, from.y, from.z + mz * 1.5f + h.vz * 0.5f);
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
		e->mPosition.x += mx;
		e->mPosition.z += mz;
		h.vx *= keep;
		h.vz *= keep;
	}
}

// shoved: those that walk about, not those on fences (net Koopas), the boss's bubbles or the
// shadow Mario look-alikes; the rooted and flying are not moved, only hit
bool moves(TSmallEnemy* e)
{
	return dynamic_cast<TWalkerEnemy*>(e) && !dynamic_cast<TAmiNoko*>(e) && !dynamic_cast<TBubble*>(e)
	       && !dynamic_cast<TKageMarioModoki*>(e);
}

// An enemy hit by a weapon's shot (`shot`: weapon and number), shoved along (dx, dz) and maybe
// thrown up, and with defeat on, defeated after the weapon's shots.
void hitEnemy(TSmallEnemy* e, int weapon, int shot, f32 dx, f32 dz)
{
	if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	Hit* h = hitOf(e);
	if (!h)
		return;
	const Weapon& k = kWeapons[weapon];
	const bool fresh = h->lastShot != shot; // this shot's first drop to reach it
	if (moves(e) && h->vx * h->vx + h->vz * h->vz < k.shove * k.shove) { // the strongest shove, not their sum
		h->vx = dx * k.shove;
		h->vz = dz * k.shove;
	}
	if (fresh && k.lift > 0.0f && moves(e)) { // thrown into the air: the game's own fall brings it down
		JGeometry::TVec3<f32> v = e->getVelocity();
		v.y = k.lift;
		e->setVelocity(v);
		e->mPosition.y += 8.0f;
		e->onLiveFlag(LIVE_FLAG_AIRBORNE);
	}
	if (!fresh)
		return;
	if (s_time - h->last > kHitWindow)
		h->hits = 0;
	h->hits++;
	h->lastShot = shot;
	h->last     = s_time;
	if (s_defeat && h->hits >= k.toDefeat) {
		h->hits = 0;
		fprintf(stderr, "[weapons] an enemy defeated by the %s\n", kNames[weapon]);
		e->kill(); // as a stomp: its own defeat, coins and all
	}
}

// The bazooka's burst where its drop first hits: every small enemy near, thrown away from it.
void burst(int weapon, int shot, const JGeometry::TVec3<f32>& at)
{
	const f32 r = kWeapons[weapon].burst;
	if (!gpConductor)
		return;
	int n = 0;
	for (JGadget::TList<JDrama::TViewObj*>::iterator it = gpConductor->unk40.begin(); it != gpConductor->unk40.end(); ++it) {
		TLiveManager* mgr = dynamic_cast<TLiveManager*>(*it);
		if (!mgr)
			continue;
		for (int i = 0; i < mgr->getObjNum(); i++) {
			TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(mgr->getObj(i));
			if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
				continue;
			const f32 x = e->mPosition.x - at.x, y = e->mPosition.y - at.y, z = e->mPosition.z - at.z;
			if (x * x + y * y + z * z > r * r)
				continue;
			const f32 l = sqrtf(x * x + z * z);
			hitEnemy(e, weapon, shot, l > 1.0f ? x / l : 0.0f, l > 1.0f ? z / l : 0.0f);
			n++;
		}
	}
	fprintf(stderr, "[weapons] the bazooka burst, %d enemies in it\n", n);
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
	const int tag    = w->mParticleAttackSOA[drop->mParticleIndex];
	const int weapon = (tag >> kTagShift) & 0xf;
	const int shot   = (tag >> kTagShift) & 0x7f; // weapon and number
	if (weapon <= NORMAL || weapon >= WEAPONS)
		return; // the normal spray, or any other water: the game's own
	TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(static_cast<THitActor*>(object));
	if (!e)
		return;
	if (kWeapons[weapon].burst > 0.0f) {
		if (s_burst != shot) { // once a shot, where it first hits an enemy
			s_burst = shot;
			burst(weapon, shot, w->mParticlePositionSOA[drop->mParticleIndex]);
		}
		return;
	}
	const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[drop->mParticleIndex];
	const f32 l = sqrtf(v.x * v.x + v.z * v.z);
	hitEnemy(e, weapon, shot, l > 1e-3f ? v.x / l : 0.0f, l > 1e-3f ? v.z / l : 0.0f);
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
	const int now     = weapon > NORMAL && weapon < WEAPONS ? weapon : NORMAL;
	const bool defeat = (flags & 1) != 0;
	if (now != s_weapon || defeat != s_defeat)
		fprintf(stderr, "[weapons] %s, defeating enemies %s\n", kNames[now], defeat ? "on" : "off");
	s_weapon = now;
	s_defeat = defeat;
	return 1;
}

// Every request for water drops (TModelWaterManager::emitRequest, patch
// zzzzzzz-held-fludd-4-weapons): true when it is the player's spray, a weapon chosen, and the
// weapon fires instead (or waits to), `*made` then what the request is to return (0: the stream's
// own water use and kick are not taken; the weapon takes its own). Asked at the request, not in
// the nozzle, as a game's own FLUDD code (Super Mario Eclipse's engine) may replace the nozzle's.
bool s_firing; // our own requests, let through
bool PortSprayWeaponRequest(const TWaterEmitInfo& asked, int* made)
{
	if (s_firing || s_weapon == NORMAL || !gpModelWaterManager)
		return false;
	TMario* m = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!g || !PortFluddHeld(m) || g->mCurrentNozzle != TWaterGun::Spray)
		return false;
	// the spray's own: from its nozzle (not Yoshi's juice, a puddle's splash, an enemy's water)
	const JGeometry::TVec3<f32>& at = asked.mPos.value;
	const f32 dx = at.x - g->mEmitPos[0].x, dy = at.y - g->mEmitPos[0].y, dz = at.z - g->mEmitPos[0].z;
	if (dx * dx + dy * dy + dz * dz > 40.0f * 40.0f)
		return false;
	*made = 0;
	if (g->mCurrentWater <= 0 || s_time < s_ready)
		return true;
	const Weapon& k   = kWeapons[s_weapon];
	s_ready           = s_time + k.every;
	s_shot            = s_shot % 7 + 1;
	TNozzleBase* n    = g->getCurrentNozzle();

	TWaterEmitInfo copy(asked); // the request's, changed for each shot (on the stack: no heap)
	TWaterEmitInfo* info = &copy;
	info->mNum.set(1);
	info->mType.set(asked.mType.get());
	info->mPos.value = asked.mPos.value;
	info->mV.value   = asked.mV.value;
	const JGeometry::TVec3<f32> way = asked.mDir.value;
	const f32 pow  = n->mEmitParams.mEmitPow.get() * k.pow;
	const s16 base = n->mEmitParams.mAttack.get();
	info->mAttack.set((s16)(((base > 0 && base < 256) ? base : 1) | (s_weapon << kTagShift) | (s_shot << kShotShift)));
	info->mAlive.set((s16)(asked.mAlive.get() * k.alive));
	info->mSize.set(n->mEmitParams.mSize.get() * k.size);
	info->mSizeTremble.set(0.0f);
	info->mHitRadius.set(asked.mHitRadius.get() * k.hit);
	info->mHitHeight.set(asked.mHitHeight.get() * k.hit);
	info->mDirTremble.set(0.0f);
	info->mPowTremble.set(0.0f);
	info->mFlag.set(asked.mFlag.get());
	int shot = 0;
	s_firing = true;
	for (int i = 0; i < k.drops; i++) {
		JGeometry::TVec3<f32> d = way;
		scatter(d, k.spread);
		info->mDir.value = d;
		info->mPow.set(pow * (1.0f - 0.05f * (f32)i)); // the rifle's in a line, one behind another
		shot += gpModelWaterManager->emitRequest(*info) ? 1 : 0;
	}
	s_firing = false;
	if (!shot)
		return true;
	if (s_told != s_weapon) {
		fprintf(stderr, "[weapons] %s fired (%d drops)\n", kNames[s_weapon], shot);
		s_told = s_weapon;
	}
	if (!(port_cheat_infinite_fludd && m->isMario()))
		g->depleteWater(k.water * (n->mEmitParams.mDecRate.get() > 0 ? n->mEmitParams.mDecRate.get() : 1));
	m->mVel.x -= way.x * k.recoil; // the kick, back along the shot
	m->mVel.z -= way.z * k.recoil;
	return true;
}
