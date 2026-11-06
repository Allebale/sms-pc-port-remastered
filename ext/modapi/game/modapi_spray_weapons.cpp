// FLUDD's spray weapons (api->held_fludd->set_weapon, combined-plus SDK): while a first-person mod
// holds FLUDD in view (modapi_held_fludd.cpp), the spray may fire as a weapon instead of its
// stream. Still water, FLUDD's own drops, only fired another way (Allebale's numbers):
//
//              shot                               reach      shove                 to defeat  every
//   Shotgun    8 drops in a spread                medium     medium                2 shots    1 s
//   Rifle      one big blob, dead straight                 very long  strong                1 shot     1 s
//   Bazooka    one huge drop, bursting on a hit   long       very strong, thrown   1 shot     5 s
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
#include <Map/MapCollisionData.hpp>
#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/NozzleBase.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/HitActor.hpp>
#include <Enemy/EnemyManager.hpp>
#include <MSound/MSound.hpp>
#include <Player/SplashManager.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include <MSound/SoundEffects.hpp>
#include <Strategic/LiveManager.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Emario.hpp>
#include <Enemy/EnemyMario.hpp>
#include <Enemy/GateKeeper.hpp>
#include <Map/PollutionManager.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjTree.hpp>
#include "port_framerate.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" int port_cheat_infinite_fludd;
// Set once a spray weapon has fired: its drops carry the weapon in their attack's upper bits, and
// TModelWaterManager::getParticleAttack (patch zzzzzzz-held-fludd-4-weapons) then gives the game
// only the lower ones. Until then (no First Person mod, or no weapon fired) the game's own attack.
int port_spray_tagged_drops = 0;
bool PortFluddHeld(TMario* mario);
bool PortFluddHeldView(JGeometry::TVec3<f32>* eye, JGeometry::TVec3<f32>* forward);
void PortFluddHeldKick(f32 amount);
bool PortFluddBackMuzzle(JGeometry::TVec3<f32>* at);

namespace {

enum { NORMAL = 0, SHOTGUN = 1, RIFLE = 2, BAZOOKA = 3, SMG = 4, WEAPONS = 5 };
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
	f32 kick;      // the held gun's recoil, 0 to 1
	bool straight; // flies dead straight to what the crosshair is on, no fall, no end to its reach
};
const Weapon kDefaults[WEAPONS] = { // the game's own numbers (a mod may set others: set_weapon_stats)
	{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false},                                // normal: the game's own
	{8, 1.0f, 0.7f, 1.25f, 1.2f, 0.12f, 1.0f, 24, 6.0f, 16.0f, 0.0f, 2, 0, 0.7f, false},       // shotgun
	{1, 4.0f, 0.7f, 3.5f, 5.0f, 0.0f, 1.0f, 24, 6.0f, 16.0f, 0.0f, 2, 0, 0.7f, true},  // rifle (the Multi Shotgun zoomed in): one big blob
	{1, 1.8f, 1.4f, 4.0f, 3.0f, 0.0f, 5.0f, 60, 16.0f, 34.0f, 38.0f, 1, 550.0f, 1.0f, true},  // bazooka
	{1, 4.0f, 0.7f, 3.5f, 5.0f, 0.0f, 0.15f, 24, 2.0f, 8.0f, 0.0f, 2, 0, 0.3f, true},   // submachine gun: the rifle's blob, quick, a smaller shove
};
Weapon kWeapons[WEAPONS]; // as they are now (kDefaults, or a mod's)
f32 s_hitScale[WEAPONS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // a weapon's hit size (the rifle's and submachine gun's), times its own
f32 s_reachScale[WEAPONS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // reach, times its own
f32 s_viewDistance[WEAPONS];  // first person: how far ahead of the camera it is held (0 the first-person mod's)
f32 s_farScale = 1.0f;        // long range: enemies and characters drawn and moving this many times further
bool s_waterScaled;           // ...and FLUDD's drops living that much further from Mario (set back at 1)
const f32 kHitWindow = 4.0f; // seconds: shots counted together towards a defeat
const f32 kSlow      = 0.82f; // a shove kept each 30th of a second

bool s_firing; // our own requests, let through
f32 s_lastShotAt;     // when the last shot went (the gun's pump)
int s_lastShotWeapon;
bool s_defeatNormal;  // the normal spray may defeat enemies (SMS_SPRAY_DEFEAT_NORMAL)
f32 s_normalTime    = 1.0f;  // seconds of it on one enemy that defeat it (the mod's menu)
const f32 kNormalGap = 0.5f; // a pause longer than this starts it over
bool s_third;        // the weapons fire in third person too (SMS_SPRAY_THIRD_PERSON), FLUDD on his back
f32 s_rocketJump;     // the bazooka's burst throws Mario too (a rocket jump), times its own; 0 not (the mod's menu)
bool s_rocketAir;     // he is in the air from one (his fall then hurts him no more than a jump's)
f32 s_rocketVX, s_rocketVZ, s_rocketTop; // his speed across the ground in the air from one, and its most
void rocketSteer(TMario* m);
bool s_thirdModel;   // and in third person their models in FLUDD's nozzle's place (SMS_SPRAY_THIRD_PERSON_MODEL)
bool s_trigger;      // R held (the mod keeps it from the game while a weapon is chosen)
f32 s_soundUntil;    // the spray's sound, for a moment after a shot
void fire();
void followBazooka();
void followStraight();
const char* const kNames[WEAPONS] = {"FLUDD", "shotgun", "rifle", "bazooka", "submachine gun"};
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
	f32 normalStart; // the normal spray on it since (its defeating: SMS_SPRAY_DEFEAT_NORMAL)
	f32 normalLast;  // and last
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

void clearStraights();
void stageSetup()
{
	s_rocketAir = false;
	clearStraights(); // (their enemies were the last stage's)
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
	if (s_weapon != NORMAL && s_trigger && s_time >= s_ready)
		fire();
	// long range: FLUDD's drops too (the game ends every drop farther than 5000 across the ground
	// from Mario, 6000 drawn: a shot never reached a far enemy); its own again at 1
	if (gpModelWaterManager && (s_farScale > 1.0f || s_waterScaled)) {
		gpModelWaterManager->unk5E08 = 5000.0f * s_farScale;
		gpModelWaterManager->unk5E0C = 6000.0f * s_farScale;
		s_waterScaled                = s_farScale > 1.0f;
	}
	followStraight();
	followBazooka();
	if (s_rocketAir) { // up from a rocket jump: his fall counts from where he is, so landing does not stun him
		TMario* m = gpMarioOriginal;
		if (m && (m->mStatus & MARIO_STATUS_FLAG_JUMPING) && !m->checkFlag(MARIO_FLAG_IN_WATER)) {
			m->unk104 = m->mPosition.y;
			rocketSteer(m);
		} else
			s_rocketAir = false;
	}
	if (s_time < s_soundUntil && gpMarioOriginal && gpMarioOriginal->mWaterGun) // the spray's own sound, only as it fires
		SMSGetMSound()->startSoundActorWithInfo(MSD_SE_PO_NORMAL_NOZZLE_IMI, &gpMarioOriginal->mWaterGun->getEmitPos0(), nullptr, 1.0f, 0, 0,
		                                     nullptr, 0, 4);
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
		if (gpMap && !gpMap->isInArea(e->mPosition.x + mx, e->mPosition.z + mz)) {
			h.vx = h.vz = 0.0f; // the stage's edge
			continue;
		}
		// moved with the map's own wall test, as actors are: kept out of walls, sliding along
		// them; steps, slopes and kerbs no longer stop it (a thin line did)
		const f32 radius = e->mBodyRadius > 30.0f ? e->mBodyRadius : 30.0f;
		const f32 height = e->mHeadHeight > 60.0f ? e->mHeadHeight * 0.5f : 30.0f;
		TBGWallCheckRecord wall(e->mPosition.x + mx, e->mPosition.y + height, e->mPosition.z + mz, radius, 1, 0);
		const bool touched = gpMap && gpMap->isTouchedWallsAndMoveXZ(&wall);
		e->mPosition.x = wall.mCenter.x;
		e->mPosition.z = wall.mCenter.z;
		if (touched) {
			// against a wall: those the game defeats against walls (Strollin' Stus and their
			// kind, rolling away from the water) are defeated so here too, hard enough
			THamuKuri* stu = dynamic_cast<THamuKuri*>(e);
			if (stu && h.vx * h.vx + h.vz * h.vz > 6.0f * 6.0f
			    && stu->getSpine()->getCurrentNerve() != &TNerveHamuKuriWallDie::theNerve()) {
				stu->getSpine()->reset();
				stu->getSpine()->setNext(&TNerveHamuKuriWallDie::theNerve());
				h.vx = h.vz = 0.0f;
				continue;
			}
			h.vx *= 0.5f; // slides on, slower
			h.vz *= 0.5f;
		}
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
	h->lastShot = shot;
	if (!s_defeat)
		return; // hits count towards a defeat only while it is on
	if (s_time - h->last > kHitWindow * (s_farScale > 1.0f ? s_farScale : 1.0f)) // longer at long range
		h->hits = 0;
	h->hits++;
	h->last = s_time;
	if (h->hits < k.toDefeat)
		fprintf(stderr, "[weapons] an enemy hit by the %s, %d of %d\n", kNames[weapon], h->hits, k.toDefeat);
	if (h->hits >= k.toDefeat) {
		h->hits = 0;
		fprintf(stderr, "[weapons] an enemy defeated by the %s\n", kNames[weapon]);
		e->kill(); // as a stomp: its own defeat, coins and all
	}
}

// The bazooka's water explosion where it bursts: the game's water-hit sprays and splashes round
// it, a ring of FLUDD's drops thrown out and up (they wet what they reach), a very big blob in
// the middle, and a bang.
void explode(const JGeometry::TVec3<f32>& at, f32 r)
{
	if (gpMarioParticleManager) {
		gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &at, 0, nullptr);
		for (int i = 0; i < 6; i++) {
			const f32 a = (f32)i * (6.2831853f / 6.0f);
			JGeometry::TVec3<f32> p(at.x + sinf(a) * r * 0.35f, at.y + 30.0f, at.z + cosf(a) * r * 0.35f);
			gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &p, 0, nullptr);
		}
	}
	if (gpSplashManager)
		for (int i = 0; i < 8; i++) {
			const f32 a = (f32)i * (6.2831853f / 8.0f);
			gpSplashManager->newSplash(JGeometry::TVec3<f32>(at.x + sinf(a) * r * 0.5f, at.y, at.z + cosf(a) * r * 0.5f), 12.0f);
		}
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (g && g->mEmitInfo && gpModelWaterManager) {
		TWaterEmitInfo info(*g->mEmitInfo);
		info.mNum.set(1);
		info.mAttack.set(1);
		info.mV.value.set(0.0f, 0.0f, 0.0f);
		info.mDirTremble.set(0.05f);
		info.mPowTremble.set(0.2f);
		info.mSize.set(info.mSize.get() * 1.5f);
		info.mFlag.set(0x40);
		s_firing = true;
		for (int i = 0; i < 24; i++) {
			const f32 a = (f32)i * (6.2831853f / 24.0f), up = 0.55f + 0.35f * random01();
			info.mPos.value.set(at.x, at.y + 40.0f, at.z);
			info.mDir.value.set(sinf(a) * (1.0f - up * 0.6f), up, cosf(a) * (1.0f - up * 0.6f));
			info.mPow.set(30.0f + 25.0f * random01());
			gpModelWaterManager->emitRequest(info);
		}
		// and one very big blob of water at the middle of it, thrown a little up, landing there
		const f32 base = g->mEmitInfo->mSize.get();
		info.mSize.set(base * 12.0f);
		info.mDirTremble.set(0.0f);
		info.mPowTremble.set(0.0f);
		info.mPos.value.set(at.x, at.y + 60.0f, at.z);
		info.mDir.value.set(0.0f, 1.0f, 0.0f);
		info.mPow.set(12.0f);
		gpModelWaterManager->emitRequest(info);
		s_firing = false;
	}
	if (SMSGetMSound())
		SMSGetMSound()->startSoundActor(MSD_SE_PO_WFLOWER_EXPLOSION, &at, 0, nullptr, 0, 4);
}

// The small enemies of one manager within the burst, each hit (a shot counts once for each).
// Shadow Mario (TEMario): the stream's drops each take one of his water meter (mWaterCtMax of them,
// then he goes down), so a weapon's one drop a shot did next to nothing. A shot takes a share of
// it instead, its weapon's: a Rifle shot a third, a Bazooka burst a half, a Submachine gun shot 1 in 10;
// emptied, he goes down at once (the game's own next hit, as the stream's last drop would).
// Only while the game lets water count (him running his course or waiting), as its own.
const f32 kShadowShare[WEAPONS] = {0.0f, 0.0f, 0.34f, 0.5f, 0.1f};
f32 s_shadowDamage = 1.0f; // a mod's setting: how much harder (above 1) or softer each hit is on him
const f32 kShadowHitbox = 3.0f; // the weapons' aim and bursts: Shadow Mario this many times as big
int s_shadowShot;
f32 s_shadowAt; // when (a shot's number comes round again after 7: not the same shot then)
bool sameShadowShot(int shot) { return shot == s_shadowShot && s_time - s_shadowAt < 3.0f; }
void shadowHit(TEMario* em, int weapon, int shot, bool gameCounted)
{
	TEnemyMario* m = em ? em->mEnemyMario : 0;
	if (!m || weapon <= NORMAL || weapon >= WEAPONS || sameShadowShot(shot) || m->getSettingsParams()->mInvincibleFlag.get())
		return;
	const u16 doing = m->mEMDoing;
	if (doing != TEnemyMario::EM_DOING_REPLAY && doing != TEnemyMario::EM_DOING_REPLAY_WAITING
	    && doing != TEnemyMario::EM_DOING_REPLAY_JUMP_TO_NEAREST_NODE)
		return;
	s_shadowShot   = shot;
	s_shadowAt     = s_time;
	const int most = m->getSettingsParams()->mWaterCtMax.get();
	int take       = (int)((f32)most * kShadowShare[weapon] * s_shadowDamage + 0.5f) - (gameCounted ? 1 : 0); // the game took one itself
	if (take < 1)
		return;
	m->mWaterCounter = (s16)(m->mWaterCounter > take ? m->mWaterCounter - take : 0);
	fprintf(stderr, "[weapons] Shadow Mario hit by the %s: %d of his %d water left\n", kNames[weapon], (int)m->mWaterCounter, most);
	if (m->mWaterCounter == 0)
		m->hitWater(em); // empty: down now
}

// Proto Piranha (TBiancoGateKeeper, the goop piranha plant). Asleep in its goop pile its body takes
// water (mVulnerable): the game wakes it after about two seconds of spraying (unk17C counting up to
// 255 while drops keep coming, unk17E); a bazooka burst wakes it at once, two rifle shots do. Awake,
// water in its open mouth (its head, mHead, while that is mVulnerable) hurts it, one drop being
// enough (unk154). The weapons reach both with bigger hitboxes.
const f32 kGatekeeperHitbox = 2.5f;

TBiancoGateKeeper* asGatekeeper(TLiveActor* a) { return dynamic_cast<TBiancoGateKeeper*>(a); }

bool gatekeeperMouth(TBiancoGateKeeper* g)
{
	return g && g->mHead && g->mHead->mVulnerable && !g->checkLiveFlag(LIVE_FLAG_DEAD);
}

bool gatekeeperPile(TBiancoGateKeeper* g) { return g && g->mVulnerable && !g->checkLiveFlag(LIVE_FLAG_DEAD); }

TBiancoGateKeeper* s_pileOf; // the last pile a rifle shot hit, when and how many times
f32 s_pileAt;
int s_pileHits;

void gatekeeperHit(TBiancoGateKeeper* g, int weapon)
{
	if (gatekeeperMouth(g)) {
		g->unk154++;
		gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &g->mHead->mPosition, 0, nullptr);
		fprintf(stderr, "[weapons] Proto Piranha hit in the mouth by the %s\n", kNames[weapon]);
		return;
	}
	if (!gatekeeperPile(g))
		return;
	g->unk154++;
	g->unk17E = 0x10;
	gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &g->mPosition, 0, nullptr);
	bool wake = weapon == BAZOOKA;
	if (weapon == RIFLE) {
		if (s_pileOf != g || s_time - s_pileAt > 5.0f)
			s_pileHits = 0;
		s_pileOf = g, s_pileAt = s_time;
		wake     = ++s_pileHits >= 2;
	}
	if (wake) {
		g->unk17C  = 0xFF;
		s_pileHits = 0;
	}
	fprintf(stderr, "[weapons] Proto Piranha's goop pile hit by the %s%s\n", kNames[weapon], wake ? ": it comes out" : "");
}

// A shot that got to what it was aimed at (far away, or Shadow Mario, who runs too fast to meet
// a drop): an enemy hit, or Shadow Mario wetted (the game's own count, then the weapon's share).
void hitTarget(TLiveActor* a, int weapon, int shot, f32 dx, f32 dz)
{
	if (TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(a)) {
		hitEnemy(e, weapon, shot, dx, dz);
	} else if (TBiancoGateKeeper* g = asGatekeeper(a)) {
		gatekeeperHit(g, weapon);
	} else if (TEMario* em = dynamic_cast<TEMario*>(a)) {
		if (sameShadowShot(shot))
			return;
		em->receiveMessage(em, HIT_MESSAGE_SPRAYED_BY_WATER);
		shadowHit(em, weapon, shot, true);
	}
}

// Shadow Mario in a burst (where he is seen, his hitbox bigger)
bool burstShadow(TEMario* em, int weapon, int shot, const JGeometry::TVec3<f32>& at, f32 r)
{
	if (!em->mEnemyMario || em->checkLiveFlag(LIVE_FLAG_DEAD))
		return false;
	const JGeometry::TVec3<f32>& ep = em->mEnemyMario->mPosition;
	const f32 x = ep.x - at.x, y = ep.y + 80.0f - at.y, z = ep.z - at.z, big = r * kShadowHitbox;
	if (x * x + z * z > big * big || fabsf(y) > 400.0f * kShadowHitbox)
		return false;
	hitTarget(em, weapon, shot, 0.0f, 0.0f);
	return true;
}

int burstIn(TLiveManager* mgr, int weapon, int shot, const JGeometry::TVec3<f32>& at, f32 r)
{
	if (!mgr)
		return 0;
	int n = 0;
	for (int i = 0; i < mgr->getObjNum(); i++) {
		if (TEMario* em = dynamic_cast<TEMario*>(mgr->getObj(i))) {
			n += burstShadow(em, weapon, shot, at, r) ? 1 : 0;
			continue;
		}
		if (TBiancoGateKeeper* g = asGatekeeper(mgr->getObj(i))) { // Proto Piranha: near its open mouth or pile
			const bool mouth = gatekeeperMouth(g);
			if (mouth || gatekeeperPile(g)) {
				const JGeometry::TVec3<f32>& h = mouth ? g->mHead->mPosition : g->mPosition;
				const f32 x = h.x - at.x, y = h.y - at.y, z = h.z - at.z, big = r * kGatekeeperHitbox;
				if (x * x + y * y + z * z <= big * big) {
					gatekeeperHit(g, weapon);
					n++;
				}
			}
			continue;
		}
		TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(mgr->getObj(i));
		if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
			continue;
		const f32 x = e->mPosition.x - at.x, y = e->mPosition.y - at.y, z = e->mPosition.z - at.z;
		if (x * x + z * z > r * r || fabsf(y) > 400.0f) // across the ground, and not far above or below
			continue;
		const f32 l = sqrtf(x * x + z * z);
		hitEnemy(e, weapon, shot, l > 1.0f ? x / l : 0.0f, l > 1.0f ? z / l : 0.0f);
		n++;
	}
	return n;
}

// A rocket jump (SmsSprayStats.self_jump, Team Fortress 2's rocket launcher): Mario near the
// bazooka's burst is thrown away from it, harder the nearer he is (half as hard at its edge), as
// high as the rocket nozzle takes him from right on it, in the air too (a jump first throws him
// higher). Made to fling him forward, the way he looks: a burst below him also throws him that
// way, one in front of him (a wall, a corner) throws him up and over, never back. He steers in the air as in any jump, and
// the landing never stuns.
// In the air from a rocket jump he steers freely, as in Team Fortress 2: the stick pushes his speed
// across the ground its way (towards where it points, turning him round in a moment, slowing him
// pulled back), never faster than the jump threw him (or 30). His speed is kept here each tick, so
// nothing turning him (the view) takes it another way.
void rocketSteer(TMario* m)
{
	f32 vx = s_rocketVX, vz = s_rocketVZ;
	const f32 mag = m->mIntendedMag / 32.0f;
	if (mag > 0.05f) {
		const f32 iy = m->mIntendedYaw * (3.14159265f / 32768.0f);
		const f32 wx = sinf(iy), wz = cosf(iy);
		const f32 push = 2.4f * (mag > 1.0f ? 1.0f : mag) * port_frame_scale(); // a 30th of a second's
		vx += wx * push, vz += wz * push;
		// and the way it already goes turned towards the stick's (steering, not only pushing)
		const f32 v = sqrtf(vx * vx + vz * vz);
		if (v > 0.5f) {
			const f32 k = 0.12f * mag * port_frame_scale();
			vx += (wx * v - vx) * k, vz += (wz * v - vz) * k;
		}
	}
	const f32 v = sqrtf(vx * vx + vz * vz);
	if (v > s_rocketTop)
		vx *= s_rocketTop / v, vz *= s_rocketTop / v;
	s_rocketVX = vx, s_rocketVZ = vz;
	const f32 h = sqrtf(vx * vx + vz * vz);
	if (h > 0.5f) {
		m->mFaceAngle.y = (s16)(atan2f(vx, vz) * (32768.0f / 3.14159265f));
		m->mForwardVel  = h;
	} else
		m->mForwardVel = 0.0f;
}

void rocketJump(const JGeometry::TVec3<f32>& at, f32 r)
{
	TMario* m = gpMarioOriginal;
	if (s_rocketJump <= 0.0f || !m || !PortFluddHeld(m) || m->onYoshi() || m->checkFlag(MARIO_FLAG_IN_WATER))
		return; // first person only (FLUDD held in view): in third person he is never thrown
	const f32 reach = r > 400.0f ? r : 400.0f; // far enough to reach him from the ground at a jump's top
	f32 dx = m->mPosition.x - at.x, dy = m->mPosition.y + 60.0f - at.y, dz = m->mPosition.z - at.z; // from his middle
	const f32 dist = sqrtf(dx * dx + dy * dy + dz * dz);
	if (dist > reach)
		return;
	if (dist > 1.0f)
		dx /= dist, dy /= dist, dz /= dist;
	else
		dx = 0.0f, dy = 1.0f, dz = 0.0f;
	const f32 g     = m->mJumpParams.mGravity.get() > 0.05f ? m->mJumpParams.mGravity.get() : 0.8f;
	const f32 speed = sqrtf(2.0f * g * 1500.0f) * (1.0f - 0.5f * dist / reach) * s_rocketJump; // the rocket nozzle's height, 1500
	f32 up          = dy * speed;
	if (up < speed * 0.3f) // a burst beside or above him still lifts him a little
		up = speed * 0.3f;
	// the way he looks (first person: the view's; else the way he faces), across the ground
	JGeometry::TVec3<f32> eye, look;
	f32 fx, fz;
	if (PortFluddHeldView(&eye, &look) && look.x * look.x + look.z * look.z > 0.01f) {
		const f32 l = sqrtf(look.x * look.x + look.z * look.z);
		fx = look.x / l, fz = look.z / l;
	} else {
		const f32 face = m->mFaceAngle.y * (3.14159265f / 32768.0f);
		fx = sinf(face), fz = cosf(face);
	}
	// thrown straight away from the burst, as in Team Fortress 2: a burst behind him throws him
	// forward, one in front back (aim a little behind to fly forward)
	const f32 along = (dx * fx + dz * fz) * speed;                        // its push the way he looks
	const f32 sx = dx * speed - along * fx, sz = dz * speed - along * fz; // and to the side
	const bool inAir = (m->mStatus & MARIO_STATUS_FLAG_JUMPING) != 0;
	if (!inAir)
		m->changePlayerStatus(MARIO_STATUS_JUMP, 0, false);
	m->mVel.y = (inAir && m->mVel.y > 0.0f ? m->mVel.y : 0.0f) + up;
	// across the ground: the way he goes now and the push, as his facing and forward speed (backwards
	// if it throws him behind him, so he keeps facing the way he looked)
	const f32 face = m->mFaceAngle.y * (3.14159265f / 32768.0f);
	const f32 hx = sinf(face) * m->mForwardVel + along * fx + sx, hz = cosf(face) * m->mForwardVel + along * fz + sz;
	const f32 h  = sqrtf(hx * hx + hz * hz);
	if (h > 0.5f) {
		f32 way = atan2f(hx, hz);
		f32 fwd = h;
		if (cosf(way - face) < 0.0f) // behind him
			way += 3.14159265f, fwd = -h;
		m->mFaceAngle.y = (s16)(way * (32768.0f / 3.14159265f));
		m->mForwardVel  = fwd;
	}
	s_rocketAir = true;
	s_rocketVX  = hx, s_rocketVZ = hz; // steered from here (rocketSteer)
	s_rocketTop = h > 30.0f ? h : 30.0f;
	fprintf(stderr, "[weapons] rocket jump: %.0f away%s, up %.1f, across %.1f\n", dist, inAir ? " (in the air)" : "", up, h);
}

// The bazooka's burst where its drop first hits an enemy or lands: every small enemy near,
// thrown away from it.
// The bazooka's burst shakes the things round it: fruit, balls and the like (TMapObjBall) are
// thrown out and up from it, as a kick throws them (fruit on a tree falls off), and trees' leaves
// sway (TMapObjTree), harder nearer. Not one being held, appearing or gone.
int shakeObjects(const JGeometry::TVec3<f32>& at, f32 r)
{
	const f32 lift = kWeapons[BAZOOKA].lift > 1.0f ? kWeapons[BAZOOKA].lift : 20.0f;
	int n = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* a = (*it)->getObj(i);
			if (!a || a->checkLiveFlag(LIVE_FLAG_DEAD) || a->checkLiveFlag(LIVE_FLAG_HIDDEN))
				continue;
			const f32 x = a->mPosition.x - at.x, y = a->mPosition.y - at.y, z = a->mPosition.z - at.z;
			const f32 d = sqrtf(x * x + z * z);
			if (d > r * 1.3f || fabsf(y) > r)
				continue;
			const f32 near1 = 1.0f - d / (r * 1.3f); // 1 at the middle, 0 at the edge
			if (TMapObjBall* b = dynamic_cast<TMapObjBall*>(a)) {
				const int st = b->mState;
				if (st == TMapObjGeneral::STATE_HOLDING || st == TMapObjGeneral::STATE_APPEARING
				    || st == TMapObjGeneral::STATE_BREAKING || st == TMapObjGeneral::STATE_WAITING_TO_APPEAR
				    || st == TMapObjGeneral::STATE_BURIED || st == TMapObjGeneral::STATE_SINKING)
					continue;
				TResetFruit* fruit = dynamic_cast<TResetFruit*>(b);
				if (fruit && (st == TMapObjGeneral::STATE_ROTTING || st == TMapObjGeneral::STATE_BROKEN))
					continue;
				const f32 out = lift * 0.55f * (0.35f + 0.65f * near1);
				const f32 dx = d > 1.0f ? x / d : 1.0f, dz = d > 1.0f ? z / d : 0.0f;
				b->mVelocity.x += dx * out;
				b->mVelocity.z += dz * out;
				b->mVelocity.y = lift * 0.5f * (0.4f + 0.6f * near1);
				b->mPosition.y += 6.0f;
				b->offLiveFlag(LIVE_FLAG_UNK10); // moving again (as kicked)
				if (fruit)
					fruit->makeObjLiving(); // off its tree or its spot, as a kicked one
				n++;
			} else if (TMapObjTree* t = dynamic_cast<TMapObjTree*>(a)) {
				const f32 push = (t->mLeafHipDropImpulse > 0.0f ? t->mLeafHipDropImpulse : 0.05f) * (0.5f + near1);
				for (int l = 0; t->mLeaves && l < t->mLeafNum; l++)
					t->mLeaves[l].mAngularVelocity += (l & 1) ? push : -push;
				n++;
			}
		}
	return n;
}

void burst(int weapon, int shot, const JGeometry::TVec3<f32>& at)
{
	const f32 r = kWeapons[weapon].burst;
	if (!gpConductor)
		return;
	int n = 0; // the stage's enemy managers (unk20), then its other managers (unk10)
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
		n += burstIn(*it, weapon, shot, at, r);
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		n += burstIn(*it, weapon, shot, at, r);
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it)
		if (TEMario* em = dynamic_cast<TEMario*>(*it)) // a Shadow Mario on his own (the plaza's): no manager
			n += burstShadow(em, weapon, shot, at, r) ? 1 : 0;
	const int shaken = shakeObjects(at, r);
	if (gpPollution) // goop all round it washed away (a drop's splash cleans only its own small spot)
		gpPollution->clean(at.x, at.y, at.z, r * 1.2f);
	explode(at, r);
	rocketJump(at, r);
	fprintf(stderr, "[weapons] the bazooka burst at (%.0f, %.0f, %.0f), %d enemies and %d things in it\n", at.x, at.y, at.z, n, shaken);
}

// A weapon pushing less than the game's own stream (its push under 100 percent): the game's own push
// of a drop (a small enemy's water reaction, its velocity) only so much of it. Its velocity before the
// hit (objectHit), and after it the change the game made taken down (objectHitDone).
f32 s_gamePush[WEAPONS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
TSmallEnemy* s_softened;
JGeometry::TVec3<f32> s_softenedV;

int dropWeapon(void* sender)
{
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !drop || drop->mParticleIndex < 0 || drop->mParticleIndex >= (int)w->mParticleCount)
		return -1;
	return (w->mParticleAttackSOA[drop->mParticleIndex] >> kTagShift) & 0xf;
}

int objectHit(void* object, void* sender, unsigned message)
{
	s_softened = 0;
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender)
		return 0;
	const int weapon = dropWeapon(sender);
	if (weapon <= NORMAL || weapon >= WEAPONS || s_gamePush[weapon] >= 1.0f)
		return 0;
	if (TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(static_cast<THitActor*>(object))) {
		s_softened  = e;
		s_softenedV = e->getVelocity();
	}
	return 0; // the game's own hit still: only its push taken down after
}

void softenPush(void* object, void* sender)
{
	TSmallEnemy* e = s_softened;
	s_softened     = 0;
	if (!e || static_cast<THitActor*>(e) != static_cast<THitActor*>(object))
		return;
	const int weapon = dropWeapon(sender);
	if (weapon <= NORMAL || weapon >= WEAPONS)
		return;
	const f32 k = s_gamePush[weapon];
	JGeometry::TVec3<f32> v = e->getVelocity();
	v.x = s_softenedV.x + (v.x - s_softenedV.x) * k;
	v.z = s_softenedV.z + (v.z - s_softenedV.z) * k;
	if (v.y > s_softenedV.y)
		v.y = s_softenedV.y + (v.y - s_softenedV.y) * k;
	e->setVelocity(v);
}

// a drop reached something: if it is a weapon's and an enemy, shoved (and maybe defeated)
void objectHitDone(void* object, void* sender, unsigned message, int)
{
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender || !gpModelWaterManager)
		return;
	softenPush(object, sender);
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	TModelWaterManager* w = gpModelWaterManager;
	if (!drop || drop->mParticleIndex < 0 || drop->mParticleIndex >= (int)w->mParticleCount)
		return;
	const int tag    = w->mParticleAttackSOA[drop->mParticleIndex];
	const int weapon = (tag >> kTagShift) & 0xf;
	const int shot   = (tag >> kTagShift) & 0x7f; // weapon and number
	if (weapon == NORMAL && s_defeatNormal) { // the normal spray, with its defeating on: a short burst
		TSmallEnemy* ne = dynamic_cast<TSmallEnemy*>(static_cast<THitActor*>(object));
		Hit* h          = ne && !ne->checkLiveFlag(LIVE_FLAG_DEAD) ? hitOf(ne) : 0;
		if (h) {
			// how long it has been sprayed without a pause of more than half a second
			if (s_time - h->normalLast > kNormalGap)
				h->normalStart = s_time;
			h->normalLast = s_time;
			if (s_time - h->normalStart >= s_normalTime) {
				h->normalStart = s_time;
				fprintf(stderr, "[weapons] an enemy defeated by the normal spray\n");
				ne->kill(); // as a stomp: its own defeat, coins and all
			}
		}
		return;
	}
	if (weapon <= NORMAL || weapon >= WEAPONS)
		return; // the normal spray, or any other water: the game's own
	if (TEMario* em = dynamic_cast<TEMario*>(static_cast<THitActor*>(object))) { // Shadow Mario: the weapon's share
		if (kWeapons[weapon].burst > 0.0f) {
			if (s_burst != shot) {
				s_burst = shot;
				burst(weapon, shot, w->mParticlePositionSOA[drop->mParticleIndex]);
			}
		} else {
			shadowHit(em, weapon, shot, true);
		}
		return;
	}
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

// The rifle's aim: the first small enemy (or other actor) along the view, short of the map's
// walls, within its reach; true with where it is hit.
f32 alongRay(const JGeometry::TVec3<f32>& eye, const JGeometry::TVec3<f32>& f, THitActor* a, f32 most, f32 grow = 1.0f)
{
	const f32 r  = a->getDamageRadius() > a->getAttackRadius() ? a->getDamageRadius() : a->getAttackRadius();
	f32 hh = a->getDamageHeight() > a->getAttackHeight() ? a->getDamageHeight() : a->getAttackHeight();
	if (grow > 1.0f && hh < 160.0f)
		hh = 160.0f; // Shadow Mario: his whole body (his own hit height is only his feet)
	// the rifle's: each enemy hit within a good deal more than its own size, so it is easy to hit
	const f32 rad0 = (r > 60.0f ? r : 60.0f) + 70.0f * s_hitScale[s_weapon];
	const f32 cx = a->mPosition.x - eye.x, cy = a->mPosition.y + 0.5f * hh - eye.y, cz = a->mPosition.z - eye.z;
	const f32 t = cx * f.x + cy * f.y + cz * f.z;
	if (t < 0.0f || t > most + (grow > 1.0f ? 400.0f : 0.0f)) // a bigger one: the ground or a wall just in front still lets it be hit
		return -1.0f;
	const f32 rad = (rad0 + t * 0.03f * s_hitScale[s_weapon]) * grow; // kinder still far away (150 more at 5000)
	const f32 px = cx - f.x * t, py = cy - f.y * t, pz = cz - f.z * t;
	const f32 vy = fabsf(py) - 0.5f * (hh > 60.0f ? hh : 60.0f) * grow - 40.0f; // a body's height (and some) counts as near
	const f32 d2 = px * px + pz * pz + (vy > 0.0f ? vy * vy : 0.0f);
	return d2 <= rad * rad ? t : -1.0f;
}

// Where along the line an enemy is (alongRay), -1 if not on it: a small enemy, or Shadow Mario (his
// own actor is left out of the drawing, a stand-in for the Mario-like one seen: that one's place)
f32 shotAlong(const JGeometry::TVec3<f32>& eye, const JGeometry::TVec3<f32>& f, TLiveActor* e, f32 most)
{
	if (TEMario* em = dynamic_cast<TEMario*>(e))
		return em->mEnemyMario && !em->checkLiveFlag(LIVE_FLAG_DEAD) ? alongRay(eye, f, em->mEnemyMario, most, kShadowHitbox) : -1.0f;
	if (TBiancoGateKeeper* g = asGatekeeper(e)) // Proto Piranha: its open mouth, or its goop pile
		return gatekeeperMouth(g) ? alongRay(eye, f, g->mHead, most, kGatekeeperHitbox)
		       : gatekeeperPile(g) ? alongRay(eye, f, g, most + 300.0f, kGatekeeperHitbox)
		                           : -1.0f;
	if (!dynamic_cast<TSmallEnemy*>(e) || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
		return -1.0f;
	return alongRay(eye, f, e, most);
}

TLiveActor* rifleTarget(const JGeometry::TVec3<f32>& eye, const JGeometry::TVec3<f32>& f, f32 reach, JGeometry::TVec3<f32>* at)
{
	// the map's walls and ground along it, a piece at a time and only inside the stage (a line
	// far out of it crashes the map's lookup)
	f32 most = 0.0f;
	const f32 kPiece = 2000.0f;
	while (gpMap && most < reach) {
		const f32 next = most + kPiece < reach ? most + kPiece : reach;
		JGeometry::TVec3<f32> a(eye.x + f.x * most, eye.y + f.y * most, eye.z + f.z * most);
		JGeometry::TVec3<f32> b(eye.x + f.x * next, eye.y + f.y * next, eye.z + f.z * next), wall;
		if (!gpMap->isInArea(a.x, a.z) || !gpMap->isInArea(b.x, b.z)) {
			most = reach; // past the stage's edge: no wall known in front, so the whole reach (an enemy aimed high
			break;        // at, the line leaving the stage behind him, is still on it)
		}
		if (gpMap->intersectLine(a, b, true, &wall)) {
			const f32 x = wall.x - eye.x, y = wall.y - eye.y, z = wall.z - eye.z;
			most = sqrtf(x * x + y * y + z * z);
			break;
		}
		most = next;
	}
	if (most < 1.0f)
		most = 1.0f;
	TLiveActor* best = 0;
	f32 bestT        = most;
	if (gpConductor) {
		for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it)
			if (TEMario* em = dynamic_cast<TEMario*>(*it)) { // a Shadow Mario on his own (the plaza's): no manager
				const f32 t = shotAlong(eye, f, em, bestT);
				if (t >= 0.0f && t < bestT)
					bestT = t, best = em;
			}
		for (int list = 0; list < 2; list++) {
			JGadget::TList<TLiveManager*> none;
			if (list == 0) {
				for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
					for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
						TLiveActor* e = (*it)->getObj(i); // a small enemy, or Shadow Mario
						const f32 t   = shotAlong(eye, f, e, bestT);
						if (t >= 0.0f && t < bestT)
							bestT = t, best = e;
					}
			} else {
				for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
					for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
						TLiveActor* e = (*it)->getObj(i); // a small enemy, or Shadow Mario
						const f32 t   = shotAlong(eye, f, e, bestT);
						if (t >= 0.0f && t < bestT)
							bestT = t, best = e;
					}
			}
		}
	}
	at->set(eye.x + f.x * bestT, eye.y + f.y * bestT, eye.z + f.z * bestT);
	return best;
}

// The straight shots in flight (the rifle's, submachine gun's and bazooka's), several at once: a
// quick weapon has a few in the air together.
const int kMostLanes = 8;
struct Straight {
	int shot;                                  // its shot (weapon and number), 0 none
	JGeometry::TVec3<f32> from, way;           // its line
	JGeometry::TVec3<f32> lanes[kMostLanes];   // each drop's own way (a spread)
	int laneCount;
	f32 reachTo, speed;
	f32 aim;             // how far away what it was aimed at is
	TLiveActor* target;  // far away (long range), or Shadow Mario: what it was aimed at, hit as the shot gets there
	int weapon;
};
const int kMostStraight = 8;
Straight s_straights[kMostStraight];
void clearStraights() { memset(s_straights, 0, sizeof s_straights); }
int s_nextStraight;
int s_bazookaShot;   // the bazooka's drop in flight (its shot), to burst where it lands
JGeometry::TVec3<f32> s_bazookaAt;
bool s_bazookaSeen;
JGeometry::TVec3<f32> s_bazookaAim; // where this shot is aimed (the map or an enemy along the view), and how far
f32 s_bazookaAimDist;
const f32 kBazookaNow = 900.0f; // aimed nearer: it bursts there as it fires (a rocket jump's, at the ground near him)
const f32 kRifleReach = 60000.0f; // as far as the stage goes

// FLUDD can fire a weapon now: held in view, or (SMS_SPRAY_THIRD_PERSON) on his back as the game has it
bool armed(TMario* m)
{
	if (PortFluddHeld(m))
		return true;
	if (!s_third || !m || m != gpMarioOriginal || m->onYoshi())
		return false;
	TWaterGun* g = m->mWaterGun;
	if (!g || !g->mFluddModel)
		return false;
	const int n = g->mCurrentNozzle;
	return n == TWaterGun::Spray || n == TWaterGun::Underwater;
}

// In third person (nothing on screen changes): the aim from about his shoulders the way he faces,
// or at the nearest small enemy a little to either side of that (no crosshair to aim with)
bool thirdView(TMario* m, JGeometry::TVec3<f32>* eye, JGeometry::TVec3<f32>* f)
{
	if (!s_third || !m)
		return false;
	const f32 a = m->mFaceAngle.y * (3.14159265f / 32768.0f);
	const f32 fx = sinf(a), fz = cosf(a);
	eye->set(m->mPosition.x, m->mPosition.y + 100.0f, m->mPosition.z);
	f->set(fx, 0.0f, fz);
	if (!gpConductor)
		return true;
	const f32 kNear = 3000.0f, kCone = 0.87f; // within about 30 degrees of the way he faces
	f32 best = kNear;
	JGeometry::TVec3<f32> to;
	bool found = false;
	for (int list = 0; list < 2; list++) {
		JGadget::TList<TEnemyManager*>::iterator a0 = gpConductor->unk20.begin(), a1 = gpConductor->unk20.end();
		JGadget::TList<TLiveManager*>::iterator b0 = gpConductor->unk10.begin(), b1 = gpConductor->unk10.end();
		while (list == 0 ? a0 != a1 : b0 != b1) {
			TLiveManager* mgr = list == 0 ? (TLiveManager*)*a0 : *b0;
			if (list == 0)
				++a0;
			else
				++b0;
			for (int i = 0; mgr && i < mgr->getObjNum(); i++) {
				TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(mgr->getObj(i));
				if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
					continue;
				const f32 hh = e->getDamageHeight() > e->getAttackHeight() ? e->getDamageHeight() : e->getAttackHeight();
				const f32 dx = e->mPosition.x - eye->x, dz = e->mPosition.z - eye->z;
				const f32 dy = e->mPosition.y + 0.5f * hh - eye->y;
				const f32 flat = sqrtf(dx * dx + dz * dz);
				if (flat < 1.0f || flat > best || (dx * fx + dz * fz) / flat < kCone || fabsf(dy) > 600.0f)
					continue;
				best = flat;
				to.set(dx, dy, dz);
				found = true;
			}
		}
	}
	if (found) {
		const f32 l = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
		f->set(to.x / l, to.y / l, to.z / l);
	}
	return true;
}

// A shot, the moment the weapon is ready while R is held (afterMovement).
void fire()
{
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!g || !gpModelWaterManager || !armed(m) || g->mCurrentNozzle != TWaterGun::Spray || g->mCurrentWater <= 0)
		return;
	TNozzleBase* n = g->getCurrentNozzle();
	JGeometry::TVec3<f32> eye, f;
	if (!n || !(PortFluddHeldView(&eye, &f) || thirdView(m, &eye, &f)))
		return;
	const Weapon& k = kWeapons[s_weapon];
	s_ready         = s_time + k.every;
	s_shot          = s_shot % 7 + 1;
	const int shot  = s_weapon | (s_shot << (kShotShift - kTagShift));

	// the drop the stream would make, from the gun's nozzle towards the middle of the view
	TWaterEmitInfo info(*g->mEmitInfo);
	n->emitCommon(0, &info);
	JGeometry::TVec3<f32> from = info.mPos.value;
	if (!PortFluddHeld(m) && PortFluddBackMuzzle(&from)) // on his back: from its nozzle (or the pack's muzzle) now
		info.mPos.value = from;
	JGeometry::TVec3<f32> way        = info.mDir.value;
	Straight* st = 0; // its straight flight (followStraight)
	if (k.straight) {
		// aimed at what the crosshair is on; the drops fly dead straight (followStraight): the
		// bazooka's to it (then it falls as any drop), the rifle's on as far as its reach
		JGeometry::TVec3<f32> at;
		const f32 reachNow = kRifleReach * (s_reachScale[s_weapon] < 1.0f ? s_reachScale[s_weapon] : 1.0f);
		TLiveActor* e      = rifleTarget(eye, f, reachNow, &at);
		JGeometry::TVec3<f32> d(at.x - from.x, at.y - from.y, at.z - from.z);
		const f32 l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
		if (l > 1.0f)
			way.set(d.x / l, d.y / l, d.z / l);
		st = &s_straights[s_nextStraight++ % kMostStraight];
		for (int q = 0; q < kMostStraight; q++) // a shot's number come round again: its old flight is done
			if (s_straights[q].shot == shot)
				st = &s_straights[q];
		st->shot      = shot;
		st->from      = from;
		st->way       = way;
		st->reachTo   = s_weapon == BAZOOKA ? l + 200.0f : reachNow;
		st->aim       = l;
		if (s_weapon == BAZOOKA)
			s_bazookaAim = at, s_bazookaAimDist = l;
		st->target    = (l > 3000.0f || dynamic_cast<TEMario*>(e) || asGatekeeper(e)) && s_weapon != BAZOOKA ? e : 0;
		st->weapon    = s_weapon;
		st->speed     = 0.0f;
		st->laneCount = 0;
		fprintf(stderr, "[weapons] the %s aimed at %s %.0f away\n", kNames[s_weapon],
		        dynamic_cast<TEMario*>(e) ? "Shadow Mario" : asGatekeeper(e) ? "Proto Piranha" : e ? "an enemy" : "nothing", l);
	}
	const f32 reach = k.straight ? 1.0f : s_reachScale[s_weapon]; // further: faster and longer lived
	const f32 pow   = n->mEmitParams.mEmitPow.get() * k.pow * (reach > 0.0f ? sqrtf(reach) : 1.0f);
	if (!PortFluddHeld(m)) { // on his back (third person): the drops start a frame's flight ahead of the muzzle, so their
		                     // streak (drawn back along their way) starts at it, not over the weapon and behind
		info.mPos.value.set(from.x + way.x * pow, from.y + way.y * pow, from.z + way.z * pow);
		if (st)
			st->from = info.mPos.value;
	}
	const s16 base = n->mEmitParams.mAttack.get();
	info.mNum.set(1);
	info.mAttack.set((s16)(((base > 0 && base < 256) ? base : 1) | (shot << kTagShift)));
	info.mAlive.set((s16)(info.mAlive.get() * k.alive * (reach > 0.0f ? sqrtf(reach) : 1.0f)));
	info.mSize.set(n->mEmitParams.mSize.get() * k.size);
	info.mSizeTremble.set(0.0f);
	const f32 hit = k.hit * s_hitScale[s_weapon]; // its hit size (its menu)
	info.mHitRadius.set(info.mHitRadius.get() * hit);
	info.mHitHeight.set(info.mHitHeight.get() * hit);
	info.mDirTremble.set(0.0f);
	info.mPowTremble.set(0.0f);
	info.mFlag.set(0x40);
	int made = 0;
	s_firing = true;
	for (int i = 0; i < k.drops; i++) {
		JGeometry::TVec3<f32> d = way;
		scatter(d, k.spread);
		info.mDir.value = d;
		if (st && st->laneCount < kMostLanes)
			st->lanes[st->laneCount++] = d; // each drop's own way, kept straight
		info.mPow.set(pow * (1.0f - 0.05f * (f32)i)); // the rifle's in a line, one behind another
		made += gpModelWaterManager->emitRequest(info) ? 1 : 0;
	}
	s_firing = false;
	if (!made)
		return;
	port_spray_tagged_drops = 1; // the game reads only the lower bits of a drop's attack from now on
	PortFluddHeldKick(k.kick);
	s_lastShotAt     = s_time;
	s_lastShotWeapon = s_weapon;
	if (s_weapon == BAZOOKA) {
		s_bazookaShot = shot;
		s_bazookaSeen = false;
		if (s_bazookaAimDist < kBazookaNow && s_burst != shot) { // near: at once, not as its drop lands
			s_burst = shot;
			burst(BAZOOKA, shot, s_bazookaAim);
		}
	}
	s_soundUntil = s_time + (s_weapon == BAZOOKA ? 0.45f : s_weapon == SMG ? 0.1f : 0.3f);
	if (s_told != s_weapon) {
		fprintf(stderr, "[weapons] %s fired (%d drops)\n", kNames[s_weapon], made);
		s_told = s_weapon;
	}
	if (!(port_cheat_infinite_fludd && m->isMario()))
		g->depleteWater(k.water * (n->mEmitParams.mDecRate.get() > 0 ? n->mEmitParams.mDecRate.get() : 1));
	m->mVel.x -= way.x * k.recoil; // the kick, back along the shot
	m->mVel.z -= way.z * k.recoil;
}

// The rifle's and bazooka's drops: kept flying dead straight at the speed they set off with (the
// game's fall taken back out each frame) and alive, until past what they were aimed at.
void followStraight()
{
	if (!gpModelWaterManager)
		return;
	TModelWaterManager* w = gpModelWaterManager;
	for (int i = 0; i < (int)w->mParticleCount; i++) {
		if ((w->mParticleFlagSOA[i] & 0xf) != 1)
			continue;
		const int tag = (w->mParticleAttackSOA[i] >> kTagShift) & 0x7f;
		if (!(tag & 0xf))
			continue; // not a weapon's
		Straight* st = 0;
		for (int q = 0; q < kMostStraight && !st; q++)
			if (s_straights[q].shot == tag)
				st = &s_straights[q];
		if (!st)
			continue;
		const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
		if (gpMap && !gpMap->isInArea(p.x, p.z)) {
			w->mParticleLifetimeSOA[i] = 0.0f; // out of the stage: gone
			continue;
		}
		const f32 x = p.x - st->from.x, y = p.y - st->from.y, z = p.z - st->from.z;
		if (x * x + y * y + z * z > st->reachTo * st->reachTo)
			continue; // past it: falls as any drop
		JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[i];
		if (st->speed <= 0.0f)
			st->speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		// its own way: the lane nearest the way it is going (a frame's fall bends it only a little)
		const f32 vl = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z) + 1e-4f;
		int lane     = 0;
		f32 best     = -2.0f;
		for (int k = 0; k < st->laneCount; k++) {
			const f32 c = (v.x * st->lanes[k].x + v.y * st->lanes[k].y + v.z * st->lanes[k].z) / vl;
			if (c > best)
				best = c, lane = k;
		}
		const JGeometry::TVec3<f32>& way = st->laneCount ? st->lanes[lane] : st->way;
		v.set(way.x * st->speed, way.y * st->speed, way.z * st->speed);
		// and put back exactly on its line (the game moved it with its fall first): from the gun
		// to what the crosshair is on, however far, never above or below it
		JGeometry::TVec3<f32>& at = w->mParticlePositionSOA[i];
		const f32 along = (at.x - st->from.x) * way.x + (at.y - st->from.y) * way.y + (at.z - st->from.z) * way.z;
		// far away (long range): faster, to get there in about half a second, not seconds (an enemy
		// walked out of the way meanwhile)
		const f32 quick = st->weapon == BAZOOKA ? st->aim / 6.0f // the bazooka's there in a fifth of a second
		                  : st->aim > 3000.0f || (st->target && (dynamic_cast<TEMario*>(st->target) || asGatekeeper(st->target))) ? st->aim / 15.0f : 0.0f;
		const f32 extra = quick > st->speed ? (quick - st->speed) * port_frame_scale() : 0.0f;
		if (along > 0.0f)
			at.set(st->from.x + way.x * (along + extra), st->from.y + way.y * (along + extra), st->from.z + way.z * (along + extra));
		// that fast it could pass through the enemy between two frames: hit as it gets there
		if (st->target && along + extra >= st->aim - 100.0f) {
			const f32 hl = sqrtf(way.x * way.x + way.z * way.z);
			hitTarget(st->target, st->weapon, st->shot, hl > 1e-3f ? way.x / hl : 0.0f, hl > 1e-3f ? way.z / hl : 0.0f);
			st->target = 0;
		}
		if (w->mParticleLifetimeSOA[i] < 30.0f)
			w->mParticleLifetimeSOA[i] = 30.0f; // no end to its reach
	}
}

// The bazooka's drop: followed until it lands or is gone, then it bursts there (if it hit no
// enemy on the way).
void followBazooka()
{
	if (!s_bazookaShot || !gpModelWaterManager)
		return;
	TModelWaterManager* w = gpModelWaterManager;
	for (int i = 0; i < (int)w->mParticleCount; i++) {
		if (((w->mParticleAttackSOA[i] >> kTagShift) & 0x7f) != s_bazookaShot)
			continue;
		s_bazookaAt   = w->mParticlePositionSOA[i];
		s_bazookaSeen = true;
		if ((w->mParticleFlagSOA[i] & 0xf) == 2) // landed
			break;
		return; // still flying
	}
	if (s_bazookaSeen && s_burst != s_bazookaShot) {
		s_burst = s_bazookaShot;
		burst(BAZOOKA, s_bazookaShot, s_bazookaAt);
	}
	if (s_bazookaSeen || s_time > s_ready)
		s_bazookaShot = 0;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		memcpy(kWeapons, kDefaults, sizeof kWeapons);
		s_extension.name          = "spray weapons";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.objectHit     = objectHit;
		s_extension.objectHitDone = objectHitDone;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace



// api->held_fludd->set_weapon (mod_runtime.cpp): 0 normal, 1 shotgun, 2 rifle, 3 bazooka; flags 1:
// weapons may defeat enemies, 2: R is held, 8: they fire in third person too (FLUDD on his back),
// 16: and there their models in its nozzle's place (the mod keeps R from the game while a weapon is chosen,
// so its own spray, pump and sound never start; the weapon fires on its own clock)
extern "C" int modg_spray_weapon_set(int weapon, int flags)
{
	const int now     = weapon > NORMAL && weapon < WEAPONS ? weapon : NORMAL;
	const bool defeat = (flags & 1) != 0;
	if (defeat != s_defeat) // counted afresh from when defeating is turned on
		for (int i = 0; i < kMostHits; i++)
			s_hits[i].hits = 0;
	if (now != s_weapon || defeat != s_defeat)
		fprintf(stderr, "[weapons] %s, defeating enemies %s\n", kNames[now], defeat ? "on" : "off");
	s_weapon  = now;
	s_defeat  = defeat;
	s_trigger = (flags & 2) != 0;
	if (((flags & 8) != 0) != s_third)
		fprintf(stderr, "[weapons] in third person %s\n", (flags & 8) ? "on" : "off");
	s_third      = (flags & 8) != 0;
	s_thirdModel = (flags & 16) != 0;
	if (((flags & 4) != 0) != s_defeatNormal)
		fprintf(stderr, "[weapons] the normal spray defeating enemies %s\n", (flags & 4) ? "on" : "off");
	s_defeatNormal = (flags & 4) != 0;
	return 1;
}

// Every request for water drops (TModelWaterManager::emitRequest, patch
// zzzzzzz-held-fludd-4-weapons): with a weapon chosen, the player's spray makes none of its own
// (should R reach the game at all); the weapon's shots are let through.
bool PortSprayWeaponRequest(const TWaterEmitInfo& asked, int* made)
{
	if (s_firing || s_weapon == NORMAL || !gpModelWaterManager)
		return false;
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!g || !armed(m) || g->mCurrentNozzle != TWaterGun::Spray)
		return false;
	const JGeometry::TVec3<f32>& at = asked.mPos.value;
	const f32 dx = at.x - g->mEmitPos[0].x, dy = at.y - g->mEmitPos[0].y, dz = at.z - g->mEmitPos[0].z;
	if (dx * dx + dy * dy + dz * dz > 40.0f * 40.0f)
		return false;
	*made = 0;
	return true;
}

// The spray weapon chosen now (the bazooka's own model, modapi_held_fludd.cpp).
int PortSprayWeaponId() { return s_weapon; }

// A spray weapon fires from FLUDD on his back now (third person, SMS_SPRAY_THIRD_PERSON): its pump
// (modapi_held_fludd.cpp); `model`, its own model in the nozzle's place too
bool PortSprayWeaponOnBack(TMario* m, bool* model)
{
	*model = s_thirdModel;
	return s_third && s_weapon != NORMAL && !PortFluddHeld(m) && armed(m);
}

// R held, a spray weapon chosen
bool PortSprayWeaponTrigger() { return s_trigger; }

// A spray weapon (not the normal spray) is chosen (modapi_held_fludd.cpp keeps the nozzle out).
bool PortSprayWeaponChosen() { return s_weapon != NORMAL; }

// How far through its pump the gun is after the last shot (0 to 1), or -1 past it: the held gun's
// animation (modapi_held_fludd.cpp), a pump only as a weapon fires.
f32 PortSprayWeaponShotPhase()
{
	const f32 len = s_lastShotWeapon == BAZOOKA ? 0.5f : s_lastShotWeapon == SMG ? 0.12f : 0.35f;
	const f32 t   = s_time - s_lastShotAt;
	return s_lastShotAt > 0.0f && t >= 0.0f && t < len ? t / len : -1.0f;
}

// held_fludd->set_weapon_stats' view_distance and far_scale (mod_runtime.cpp)
extern "C" int modg_spray_view_distance(int weapon, float distance)
{
	if (weapon < 0 || weapon >= WEAPONS)
		return 0;
	s_viewDistance[weapon] = distance > 1.0f && distance < 1000.0f ? distance : 0.0f;
	return 1;
}

extern "C" int modg_spray_far_scale(float scale)
{
	const f32 k = scale > 1.0f ? (scale < 20.0f ? scale : 20.0f) : 1.0f;
	if (k != s_farScale)
		fprintf(stderr, "[weapons] long range: enemies and characters out to %.1f times as far\n", k);
	s_farScale = k;
	return 1;
}

extern "C" int modg_spray_shadow_damage(float scale)
{
	const f32 k = scale > 0.0f ? (scale < 10.0f ? scale : 10.0f) : 1.0f;
	if (k != s_shadowDamage)
		fprintf(stderr, "[weapons] Shadow Mario: each hit takes %.2f times as much of his water\n", k);
	s_shadowDamage = k;
	return 1;
}

// The first-person hold distance of the weapon chosen (modapi_held_fludd.cpp), 0 the mod's own.
f32 PortSprayWeaponViewDistance() { return s_weapon != NORMAL ? s_viewDistance[s_weapon] : 0.0f; }

// Long range (livemanager.cpp, as it clips its actors out of the view): enemies and characters
// out to s_farScale times their own distance; map objects as ever.
f32 PortSprayWeaponFarScale(TLiveManager* manager)
{
	if (s_farScale <= 1.0f || dynamic_cast<TMapObjBaseManager*>(manager))
		return 1.0f;
	return s_farScale;
}

// held_fludd->set_weapon_stats' self_jump (mod_runtime.cpp): the bazooka's rocket jump, 0 none
extern "C" int modg_spray_rocket_jump(float strength)
{
	if ((strength > 0.0f) != (s_rocketJump > 0.0f))
		fprintf(stderr, "[weapons] the bazooka's rocket jump %s\n", strength > 0.0f ? "on" : "off");
	s_rocketJump = strength > 0.0f ? (strength < 4.0f ? strength : 4.0f) : 0.0f;
	return 1;
}

// held_fludd->set_weapon_stats (mod_runtime.cpp): a weapon's numbers, as the mod's menu has them
// (`has` 0: the game's own again). push, range, hit and burst are times the game's own.
extern "C" int modg_spray_weapon_stats(int weapon, int has, float every, float push, float range, int toDefeat, int water,
                                       float hit, float burst, float lift, float dropSize)
{
	if (weapon == NORMAL) { // the normal spray: only its seconds to defeat
		s_normalTime = has && every >= 0.1f ? every : 1.0f;
		return 1;
	}
	if (weapon < NORMAL || weapon >= WEAPONS)
		return 0;
	Weapon& w       = kWeapons[weapon];
	const Weapon& d = kDefaults[weapon];
	w               = d;
	s_reachScale[weapon] = 1.0f;
	s_hitScale[weapon]   = 1.0f;
	s_gamePush[weapon]   = 1.0f;
	if (!has)
		return 1;
	w.every    = every >= 0.05f ? every : 0.05f;
	w.shove    = d.shove * (push >= 1.0f ? push : 0.0f); // under 100 percent: none of its own, and the game's
	s_gamePush[weapon] = push >= 1.0f ? 1.0f : push > 0.0f ? push : 0.0f; // own push only that much of it
	w.lift     = d.lift * (lift >= 0.0f ? lift : push >= 0.0f ? push : 0.0f); // its own, or with the push (lift < 0)
	w.size     = d.size * (dropSize > 0.05f ? dropSize : 1.0f);
	w.toDefeat = toDefeat >= 1 ? toDefeat : 1;
	w.water    = water >= 0 ? water : 0;
	w.burst    = d.burst * (burst > 0.1f ? burst : 0.1f);
	s_reachScale[weapon] = range > 0.1f ? range : 0.1f;
	if (weapon == RIFLE || weapon == SMG)
		s_hitScale[weapon] = hit >= 0.0f ? hit : 0.0f;
	return 1;
}
