// FLUDD's goop weapons (api->held_fludd->set_weapon SMS_SPRAY_FIRE_GOOP and SMS_SPRAY_ELECTRIC_GOOP,
// a weapons mod's): FLUDD sprays as ever (its own model, animation, pump, sound and water), but its
// stream is fire goop or electric goop, not water. Its drops carry the kind in their attack's upper
// bits (as the spray weapons' do, modapi_spray_weapons.cpp), are drawn here as goop instead of water
// (patch zzzzzzzzz-goop-weapons leaves them out of the water's drawing), and where one lands on the
// ground it leaves goop: a puddle, or a puddle there grown bigger. A drop on a wall or in water is
// just gone.
//
// The puddles are the port's own (any ground of any stage, not only the stage's goop surfaces),
// drawn as the game's goop of the kind looks: electric marbled teal and yellow, fire orange with
// red swirls and flames rising off it. Mario stepping in one is hurt as by the game's own goop: fire
// throws him up burning (TMario::checkGraffitoFire), electric shocks him
// (MARIO_STATUS_ELECTRIC_DAMAGE). Enemies (with "can defeat enemies") touched by the stream or
// standing in a puddle are shocked (held trembling, Mario's shock sparks on them) or set burning
// (the game's burning flames), then defeated. FLUDD's water landing in a puddle washes it away bit
// by bit (and the bazooka's burst); left alone a puddle lasts a minute (the mod's setting),
// shrinking away at its end.
//
// Without a weapons mod choosing one, nothing here does anything: no drop is tagged, none hidden.
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <M3DUtil/MActor.hpp>
#include <Player/NozzleBase.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <Enemy/SmallEnemy.hpp>
#include <NPC/NpcBase.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/Conductor.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/Map.hpp>
#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/WaterGun.hpp>
#include <Player/MarioStatus.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include "port_ext.h"
#include "port_framerate.h"

#include <Enemy/BossManta.hpp>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern int port_spray_tagged_drops; // modapi_spray_weapons.cpp
bool PortFluddHeld(TMario* mario); // modapi_held_fludd.cpp
bool PortIsPlayer2(TMario* mario); // co-op's Luigi
MtxPtr PortFluddHeldEmitMtx(f32* scale); // the held FLUDD's nozzle tip
bool PortSprayWeaponTrigger();
class TBossManta;
TBossManta* PortSprayManta(void* object); // modapi_spray_weapons.cpp: a hit on a Phantamanta          // modapi_spray_weapons.cpp: the fire button held
bool PortSprayBazookaNoClean();     // modapi_spray_weapons.cpp
// a picture shown in place of a texture, by its data (platform/gx/src/gx_texture.cpp)
extern "C" void GXPC_SetTextureOverride(const void* data, size_t bytes, const unsigned char* rgba, int w, int h)
    __attribute__((weak));

namespace {

const int kTagShift = 8;  // the kind in a drop's attack, above the game's own (as the spray weapons')
const int kFire     = 5;  // SMS_SPRAY_FIRE_GOOP
const int kElectric = 6;  // SMS_SPRAY_ELECTRIC_GOOP
const int kBlack    = 7;  // SMS_SPRAY_BLACK_GOOP: slippery, Mario walks and slides fast through it
bool isKind(int k) { return k == kFire || k == kElectric || k == kBlack; }

int s_kind;           // the goop weapon chosen: kFire, kElectric, or 0 none
bool s_defeat;        // it may defeat enemies
bool s_emitting;      // our own request, let through
bool s_npcs = true; // friendly characters shocked, and thrown into fire, by the goop (never defeated)
bool s_grassPress = true, s_grassBurn = true; // (Remastered's 3D Grass) electric goop presses it, fire goop burns it
// Remastered's 3D Grass (port_grass.cpp), on builds that have it; none here otherwise
void (*const port_grass_goop)(float x, float y, float z, float r, int kind, int ms) = 0;
int (*const port_grass_burning)(float x, float z)                                  = 0;
int (*const port_grass_fires)(float* out, int most)                                = 0;
void (*const port_grass_extinguish)(float x, float z, float r)                     = 0;
f32 s_time;           // seconds played
// each kind's numbers (the mod's menu): seconds of its stream, or of standing in its goop, that
// defeat an enemy; how long its goop lasts, times a minute; its puddles' size, times their own
struct Stats {
	f32 defeat, life, size, drop;
	int hits;  // times an enemy is shocked or burnt before it is defeated
	f32 water; // the water it uses, times FLUDD's own
};
const Stats kDefaults = {1.0f, 1.0f, 1.0f, 1.0f, 1, 1.0f};
Stats s_stats[3]      = {kDefaults, kDefaults, kDefaults};
f32 s_blackSpeed      = 1.0f; // black goop's speed, times its own
f32 s_airTurn         = 0.5f; // turning in the air after a jump off it: 0 none, 1 at once (the stick's way)
Stats& statsOf(int kind) { return s_stats[kind == kElectric ? 1 : kind == kBlack ? 2 : 0]; }

// a puddle of goop on the ground
struct Puddle {
	bool on;
	int kind;
	f32 x, y, z;    // its middle, on the ground
	f32 nx, ny, nz; // the ground's way up there
	f32 r;          // its size (radius)
	f32 born;       // when it was last added to (s_time)
	f32 seed;       // its own shape and flicker
	f32 inside;     // Mario standing in it (electric's moment before it shocks)
};
const int kPuddles = 64;
Puddle s_puddles[kPuddles];
const f32 kStartSize = 95.0f, kMostSize = 380.0f, kGrow = 5.0f, kWash = 5.0f, kGone = 14.0f;
const f32 kLife = 60.0f, kFade = 4.0f; // seconds, and the last of them shrinking away

// goop on walls (the mod's "Goop sticks to walls"): a blob where a drop hit, dripping down the wall
// more and more, gone after the mod's seconds (shrinking away at the end)
struct WallGoop {
	bool on;
	int kind;
	f32 x, y, z;    // its middle, on the wall
	f32 nx, ny, nz; // the wall's way out
	f32 r;
	f32 born;  // when it was last added to
	f32 since; // when it first stuck (its drips' length)
	f32 seed;
};
const int kWalls = 48;
WallGoop s_wallGoop[kWalls];
bool s_wallsOn   = true;
f32 s_wallLife   = 30.0f;

// enemies touched by the goop: shocked (held still, trembling, sparks on them) or burning (the
// burning flames of the game's on them) for the kind's seconds, then defeated
struct Zap {
	TLiveActor* e; // an enemy, or a friendly character (with "Goop shocks and burns characters")
	int kind;
	f32 start;
	f32 bx, by, bz;               // where it was shocked (held there)
	JGeometry::TVec3<f32> centre; // the sparks' place (on it)
	int hits;                     // times shocked or burnt so far
	bool resting;                 // between two: let go
	f32 until;                    // its rest's end
};
const int kZaps = 24;
Zap s_zaps[kZaps];

u32 s_seed = 777u;
f32 random01()
{
	s_seed = s_seed * 1664525u + 1013904223u;
	return (f32)((s_seed >> 8) & 0xFFFF) / 65535.0f;
}

int dropKind(int i)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!port_spray_tagged_drops || !w || i < 0 || i >= (int)w->mParticleCount)
		return 0;
	const int k = ((unsigned short)w->mParticleAttackSOA[i] >> kTagShift) & 0xf;
	return isKind(k) ? k : 0;
}

f32 lifeOf(const Puddle& p) { return kLife * statsOf(p.kind).life; }

// its size now: shrinking away at the end of its life
f32 sizeNow(const Puddle& p)
{
	const f32 left = lifeOf(p) - (s_time - p.born);
	return left >= kFade ? p.r : left > 0.0f ? p.r * left / kFade : 0.0f;
}

// Goop never lands round Mario's feet: how big a puddle (or a blob on a wall) at (x, y, z) may be
// and still keep clear of him (its wavy edge reaching a quarter further, and a margin); no limit
// when he is far above or below
f32 roomFromMario(f32 x, f32 y, f32 z, bool wall)
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return 1e30f;
	const f32 dx = m->mPosition.x - x, dz = m->mPosition.z - z;
	const f32 dy = wall ? m->mPosition.y + 80.0f - y : m->mPosition.y - y;
	if (!wall && fabsf(dy) > 150.0f)
		return 1e30f;
	const f32 d = wall ? sqrtf(dx * dx + dy * dy + dz * dz) : sqrtf(dx * dx + dz * dz);
	return (d - 45.0f) / 1.25f;
}

void addGoop(int kind, f32 x, f32 y, f32 z, const TBGCheckData* ground)
{
	const Stats& st = statsOf(kind);
	Puddle* best    = 0;
	f32 bestD       = 1e30f;
	for (int i = 0; i < kPuddles; i++) { // the same kind's puddle it lands in: grown
		Puddle& p = s_puddles[i];
		if (!p.on || p.kind != kind || fabsf(p.y - y) > 50.0f)
			continue;
		const f32 dx = p.x - x, dz = p.z - z, d = dx * dx + dz * dz;
		if (d < p.r * p.r && d < bestD)
			best = &p, bestD = d;
	}
	if (best) {
		f32 most       = kMostSize * st.size;
		const f32 room = roomFromMario(best->x, best->y, best->z, false);
		most           = most < room ? most : room; // not grown under his feet
		if (best->r + kGrow * st.size <= most)
			best->r += kGrow * st.size;
		best->born = s_time;
		return;
	}
	f32 start      = kStartSize * st.size;
	const f32 room = roomFromMario(x, y, z, false);
	if (room < 20.0f)
		return; // at his feet: none
	start = start < room ? start : room;
	Puddle* p = 0; // a new one: a free place, or the oldest's
	for (int i = 0; i < kPuddles && !p; i++)
		if (!s_puddles[i].on)
			p = &s_puddles[i];
	if (!p) {
		p = &s_puddles[0];
		for (int i = 1; i < kPuddles; i++)
			if (s_puddles[i].born < p->born)
				p = &s_puddles[i];
	}
	p->on   = true;
	p->kind = kind;
	p->x = x, p->y = y, p->z = z;
	p->nx = 0.0f, p->ny = 1.0f, p->nz = 0.0f;
	if (ground && ground->isLegal()) {
		const JGeometry::TVec3<f32>& n = ground->getNormal();
		if (n.y > 0.3f)
			p->nx = n.x, p->ny = n.y, p->nz = n.z;
	}
	p->r      = start;
	p->born   = s_time;
	p->seed   = random01() * 100.0f;
	p->inside = 0.0f;
}

// washed: every puddle under (x, z) shrinks by `amount`, gone when small
void washAt(f32 x, f32 y, f32 z, f32 reach, f32 amount)
{
	for (int i = 0; i < kPuddles; i++) {
		Puddle& p = s_puddles[i];
		if (!p.on || fabsf(p.y - y) > 80.0f)
			continue;
		const f32 dx = p.x - x, dz = p.z - z, r = p.r + reach;
		if (dx * dx + dz * dz > r * r)
			continue;
		p.r -= amount;
		if (p.r < kGone)
			p.on = false;
	}
}

f32 wallSizeNow(const WallGoop& g)
{
	const f32 left = s_wallLife - (s_time - g.born);
	return left >= 3.0f ? g.r : left > 0.0f ? g.r * left / 3.0f : 0.0f;
}

void addWallGoop(int kind, f32 x, f32 y, f32 z, const TBGCheckData* wall)
{
	if (!wall)
		return;
	const JGeometry::TVec3<f32>& n = wall->getNormal();
	if (fabsf(n.y) > 0.6f)
		return; // not a wall
	const f32 size = statsOf(kind).size;
	const f32 room = roomFromMario(x, y, z, true);
	if (room < 15.0f)
		return; // where he is: none
	for (int i = 0; i < kWalls; i++) { // the same kind's blob it hits: grown
		WallGoop& g = s_wallGoop[i];
		if (!g.on || g.kind != kind || g.nx * n.x + g.ny * n.y + g.nz * n.z < 0.8f)
			continue;
		const f32 dx = g.x - x, dy = g.y - y, dz = g.z - z;
		if (dx * dx + dy * dy + dz * dz < g.r * g.r) {
			const f32 most = 140.0f * size < roomFromMario(g.x, g.y, g.z, true) ? 140.0f * size : roomFromMario(g.x, g.y, g.z, true);
			if (g.r + 2.5f * size <= most)
				g.r += 2.5f * size;
			g.born = s_time;
			return;
		}
	}
	WallGoop* g = 0;
	for (int i = 0; i < kWalls && !g; i++)
		if (!s_wallGoop[i].on)
			g = &s_wallGoop[i];
	if (!g) {
		g = &s_wallGoop[0];
		for (int i = 1; i < kWalls; i++)
			if (s_wallGoop[i].born < g->born)
				g = &s_wallGoop[i];
	}
	g->on = true, g->kind = kind;
	g->x = x, g->y = y, g->z = z;
	const f32 hl = sqrtf(n.x * n.x + n.z * n.z);
	g->nx = n.x / hl, g->ny = 0.0f, g->nz = n.z / hl;
	g->r     = 38.0f * size < room ? 38.0f * size : room;
	g->born  = g->since = s_time;
	g->seed  = random01() * 100.0f;
}

void washWallAt(f32 x, f32 y, f32 z, f32 reach, f32 amount)
{
	for (int i = 0; i < kWalls; i++) {
		WallGoop& g = s_wallGoop[i];
		if (!g.on)
			continue;
		const f32 dx = g.x - x, dy = g.y - y, dz = g.z - z, r = g.r + reach;
		if (dx * dx + dy * dy + dz * dz > r * r)
			continue;
		g.r -= amount;
		if (g.r < 10.0f)
			g.on = false;
	}
}

// an enemy in the stream or the goop: shocked or set burning (once), defeated when that is over
// fire goop stepped in (or the fire stream): thrown up two and a half times its own height, jumping
// into fire: carried on an arc right into the middle of the nearest other puddle of fire goop
// (wherever it is; the one it is in when there is no other), there however far; with none, a hop a
// random way
struct Flight {
	TLiveActor* a;
	f32 sx, sy, sz; // from
	f32 tx, ty, tz; // to: the puddle's middle
	f32 length, at; // seconds the flight takes, and so far
	f32 high;       // its arc's top over the straight way
};
const int kFlights = 24;
Flight s_flights[kFlights];

void launchEnemy(TLiveActor* e)
{
	if (e->checkLiveFlag(LIVE_FLAG_AIRBORNE))
		return; // (already up)
	for (int i = 0; i < kFlights; i++)
		if (s_flights[i].a == e)
			return; // (already flying)
	f32 tall = e->getDamageHeight() > e->getAttackHeight() ? e->getDamageHeight() : e->getAttackHeight();
	tall     = tall > 20.0f ? tall : 100.0f;
	const Puddle* to = 0;
	const Puddle* in = 0;
	f32 best = 1e30f;
	for (int i = 0; i < kPuddles; i++) {
		const Puddle& p = s_puddles[i];
		if (!p.on || p.kind != kFire || fabsf(p.y - e->mPosition.y) > 2000.0f)
			continue;
		const f32 dx = p.x - e->mPosition.x, dz = p.z - e->mPosition.z, d = dx * dx + dz * dz;
		const f32 r = sizeNow(p);
		if (d < r * r) { // the one it is in
			in = &p;
			continue;
		}
		if (d < best)
			best = d, to = &p;
	}
	if (in)
		return; // already in the fire: left to the water's push (thrown again only once out of it)
	if (!to) { // no fire: a hop a random way
		f32 g = e->getGravityY();
		g     = g > 0.05f ? g : 0.5f;
		const f32 a = random01() * 6.2831853f;
		JGeometry::TVec3<f32> v = e->getVelocity();
		v.y = sqrtf(2.0f * g * 2.5f * tall);
		v.x = sinf(a) * 6.0f, v.z = cosf(a) * 6.0f;
		e->setVelocity(v);
		e->mPosition.y += 8.0f;
		e->onLiveFlag(LIVE_FLAG_AIRBORNE);
		return;
	}
	Flight* f = 0;
	for (int i = 0; i < kFlights && !f; i++)
		if (!s_flights[i].a)
			f = &s_flights[i];
	if (!f)
		return;
	const f32 dx = to->x - e->mPosition.x, dz = to->z - e->mPosition.z, d = sqrtf(dx * dx + dz * dz);
	f->a  = e;
	f->sx = e->mPosition.x, f->sy = e->mPosition.y, f->sz = e->mPosition.z;
	f->tx = to->x, f->ty = to->y, f->tz = to->z;
	f->length = 0.7f + d / 1500.0f;
	f->length = f->length < 2.5f ? f->length : 2.5f;
	f->at     = 0.0f;
	f->high   = 2.5f * tall + d * 0.1f;
	e->onLiveFlag(LIVE_FLAG_AIRBORNE);
}

// each tick: the thrown carried along their arcs, set down in the fire at the end
void flights(f32 dt)
{
	for (int i = 0; i < kFlights; i++) {
		Flight& f = s_flights[i];
		if (!f.a)
			continue;
		TLiveActor* e = f.a;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN)) {
			f.a = 0;
			continue;
		}
		f.at += dt;
		const f32 k = f.at < f.length ? f.at / f.length : 1.0f;
		e->mPosition.x = f.sx + (f.tx - f.sx) * k;
		e->mPosition.z = f.sz + (f.tz - f.sz) * k;
		e->mPosition.y = f.sy + (f.ty - f.sy) * k + 4.0f * f.high * k * (1.0f - k);
		e->setVelocity(JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f));
		if (k >= 1.0f) {
			// down, in the fire: held right there a little while (no bounce on past it)
			e->mPosition.x = f.tx, e->mPosition.z = f.tz;
			if (f.at < f.length + dt)
				e->mPosition.y = f.ty;
			else if (e->mPosition.y > f.ty + 5.0f)
				e->mPosition.y = f.ty;
			e->offLiveFlag(LIVE_FLAG_AIRBORNE);
			if (f.at >= f.length + 0.4f)
				f.a = 0;
		}
	}
}

// `direct`: hit by the stream itself, hurt at once (a burn or shock counted now, not after its
// seconds); else touched by the goop (a puddle, the grass on fire): burned or shocked for its seconds
// a friendly character (shocked and burnt, thrown into fire, never defeated)
bool isNpc(TLiveActor* a) { return a && dynamic_cast<TBaseNPC*>(a) != 0; }

void burn(TLiveActor* e, int kind, bool launch = false, bool direct = false)
{
	if (kind == kBlack)
		return; // (only slippery)
	if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
		return;
	const bool npc = isNpc(e);
	if (npc && !s_npcs)
		return;
	if (npc)
		direct = false; // (its whole shock or burn shown each time)
	if (launch && kind == kFire)
		launchEnemy(e); // thrown into the nearest fire, burning (each spray that reaches it)
	if (!s_defeat && !npc)
		return; // (not defeated)
	Zap* free = 0;
	for (int i = 0; i < kZaps; i++) {
		Zap& z = s_zaps[i];
		if (z.e == e) {
			if (direct && !z.resting) {
				z.start = s_time - statsOf(z.kind).defeat; // the stream on it: counted at once
				return;
			}
			if (!z.resting || s_time < z.until - (direct ? 0.35f : 0.0f))
				return; // already, or resting a moment
			free = &z; // again
			break;
		}
		if (!free && !z.e)
			free = &z;
	}
	if (!free) // the longest resting one's place
		for (int i = 0; i < kZaps; i++)
			if (s_zaps[i].resting && (!free || s_zaps[i].until < free->until))
				free = &s_zaps[i];
	if (!free)
		return;
	if (free->e != e)
		free->hits = 0;
	free->resting = false;
	free->e     = e;
	free->kind  = kind;
	free->start = direct ? s_time - statsOf(kind).defeat : s_time;
	free->bx = e->mPosition.x, free->by = e->mPosition.y, free->bz = e->mPosition.z;
	free->centre = e->mPosition;
	SMSGetMSound()->startSoundActor(kind == kFire ? MSD_SE_EF_FIRE : MSD_SE_EN_DENNOKO_ELEC_DOWN, &e->mPosition, 0, nullptr, 0, 4);
	fprintf(stderr, "[goop] %s %s\n", npc ? "a character" : "an enemy", kind == kFire ? "set burning" : "shocked");
}

// each frame: the shocked held still and trembling with sparks, the burning in flames; defeated at the end
void zaps()
{
	for (int i = 0; i < kZaps; i++) {
		Zap& z = s_zaps[i];
		if (!z.e)
			continue;
		TLiveActor* e = z.e;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN)) {
			z.e = 0;
			continue;
		}
		const f32 size = e->mScaling.y > 0.1f ? e->mScaling.y : 1.0f;
		if (z.resting)
			continue;
		if (s_time - z.start >= statsOf(z.kind).defeat && isNpc(e)) {
			z.resting = true; // a character: let go, never defeated (again after a moment in the goop)
			z.until   = s_time + 1.0f;
			z.hits    = 0;
			continue;
		}
		if (s_time - z.start >= statsOf(z.kind).defeat && ++z.hits < statsOf(z.kind).hits) {
			z.resting = true; // let go; shocked or burnt again when it touches the goop again
			z.until   = s_time + 0.6f;
			continue;
		}
		if (s_time - z.start >= statsOf(z.kind).defeat) {
			z.e = 0;
			if (gpMarioParticleManager)
				gpMarioParticleManager->emit(PARTICLE_MS_MOE_FIRE_OFF, &e->mPosition, 0, nullptr); // a puff of smoke
			if (TSmallEnemy* se = dynamic_cast<TSmallEnemy*>(e))
				se->kill(); // its own defeat, coins and all
			continue;
		}
		if (z.kind == kElectric) { // held where it was shocked, trembling
			const f32 j = 3.0f * size;
			e->mPosition.x = z.bx + (random01() - 0.5f) * 2.0f * j;
			e->mPosition.y = z.by;
			e->mPosition.z = z.bz + (random01() - 0.5f) * 2.0f * j;
		}
		z.centre.x = e->mPosition.x;
		z.centre.y = e->mPosition.y + 40.0f * size;
		z.centre.z = e->mPosition.z;
		if (!gpMarioParticleManager)
			continue;
		if (z.kind == kElectric) { // Mario's own shock's sparks
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_A, &z.centre, 1, e);
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_C, &z.centre, 1, e);
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_B, &z.centre, 1, e);
		} else { // the game's burning flames (a burning Strollin' Stu's)
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_A, &e->mPosition, 1, e);
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_B, &e->mPosition, 1, e);
			gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_D, &e->mPosition, 1, e);
		}
	}
}

// black goop under Mario: slippery as ice and fast. Each tick the game moves him as ever; on black
// goop his way across the ground is kept from tick to tick instead, turning toward the game's own
// step (times the mod's speed) only slowly: he speeds up beyond his own speed, slides on when the
// stick is let go, and turns wide, as on ice
bool s_havePrev, s_gliding;
f32 s_prevX, s_prevZ, s_glideX, s_glideZ;

bool onBlack(TMario* m)
{
	if (m->onYoshi() || m->checkFlag(MARIO_FLAG_IN_WATER) || m->mPosition.y > m->mFloorPosition.y + 12.0f)
		return false;
	for (int i = 0; i < kPuddles; i++) {
		const Puddle& p = s_puddles[i];
		if (!p.on || p.kind != kBlack)
			continue;
		const f32 r = sizeNow(p) * 0.9f, dx = m->mPosition.x - p.x, dz = m->mPosition.z - p.z;
		if (dx * dx + dz * dz <= r * r && fabsf(m->mPosition.y - p.y) < 40.0f)
			return true;
	}
	return false;
}

// Mario moved on from where the game left him to (tx, tz), a little at a time, kept out of walls
// as the game keeps him (its wall check, at his knees and his chest): what stops him takes that
// way out of the glide, so he slides along a wall instead of through it
void glideTo(TMario* m, f32 tx, f32 tz)
{
	if (!gpMap)
		return;
	f32 x = m->mPosition.x, z = m->mPosition.z;
	const f32 ax = tx - x, az = tz - z, along = sqrtf(ax * ax + az * az);
	const int steps = along > 18.0f ? (int)(along / 18.0f) + 1 : 1;
	for (int k = 0; k < steps; k++) {
		const f32 wx = x + ax / steps, wz = z + az / steps; // where this step would take him
		f32 px = wx, pz = wz;
		bool hit = false;
		static const f32 kHeights[2] = {30.0f, 80.0f};
		for (int h = 0; h < 2; h++) {
			TBGWallCheckRecord wall(px, m->mPosition.y + kHeights[h], pz, 50.0f, 4, TBGWallCheckRecord::IGNORE_WATER_THROUGH);
			if (gpMap->isTouchedWallsAndMoveXZ(&wall)) {
				px = wall.mCenter.x, pz = wall.mCenter.z;
				if (wall.mResultWalls[0]) { // that way out of the glide
					const JGeometry::TVec3<f32>& n = wall.mResultWalls[0]->getNormal();
					const f32 into = s_glideX * n.x + s_glideZ * n.z;
					const f32 fast = sqrtf(s_glideX * s_glideX + s_glideZ * s_glideZ);
					// head on and faster than he can run: knocked back onto his bottom, as by a
					// hard crash (the game's own fall back down)
					if (into < 0.0f && -into > 0.7f * fast && -into > 34.0f * port_frame_scale() && !(m->mStatus & 0x20000)
					    && m->mStatus != MARIO_STATUS_JUMP_BACK_DOWN) {
						const f32 hl = sqrtf(n.x * n.x + n.z * n.z);
						if (hl > 0.3f) {
							m->mFaceAngle.y = (s16)(atan2f(-n.x / hl, -n.z / hl) * (32768.0f / 3.14159265f)); // facing the wall
							m->changePlayerStatus(MARIO_STATUS_JUMP_BACK_DOWN, 0, true);
							m->mForwardVel = -18.0f; // thrown back from it
							m->mVel.y      = 30.0f;
							SMSGetMSound()->startSoundActor(MSD_SE_MA_WALL_COL_HARD, &m->mPosition, 0, nullptr, 0, 4);
							s_glideX = s_glideZ = 0.0f;
							s_gliding = false;
							m->mPosition.x = px, m->mPosition.z = pz;
							return;
						}
					}
					if (into < 0.0f)
						s_glideX -= n.x * into, s_glideZ -= n.z * into;
				}
				hit = true;
			}
		}
		x = px, z = pz;
		if (hit) {
			const f32 ox = px - wx, oz = pz - wz;
			if (ox * ox + oz * oz > 4.0f)
				break; // stopped by it: no further this tick
		}
	}
	m->mPosition.x = x, m->mPosition.z = z;
}

void blackGoop(f32 dt)
{
	TMario* m = gpMarioOriginal;
	if (!m) {
		s_havePrev = s_gliding = false;
		return;
	}
	const f32 dx = m->mPosition.x - s_prevX, dz = m->mPosition.z - s_prevZ; // the game's step this tick
	const bool sane = s_havePrev && fabsf(dx) < 300.0f && fabsf(dz) < 300.0f;
	const bool air  = m->mPosition.y > m->mFloorPosition.y + 12.0f && !m->checkFlag(MARIO_FLAG_IN_WATER);
	if (sane && !air && onBlack(m)) {
		if (!s_gliding)
			s_glideX = dx, s_glideZ = dz, s_gliding = true;
		const f32 k = 1.0f - powf(0.5f, dt / 0.45f); // half way toward the game's step (times the speed) in 0.45 s
		s_glideX += (dx * s_blackSpeed - s_glideX) * k;
		s_glideZ += (dz * s_blackSpeed - s_glideZ) * k;
	} else if (sane && s_gliding && !m->checkFlag(MARIO_FLAG_IN_WATER)) {
		// off it, its speed carried on (through the air after a jump or a rocket jump, over other
		// ground) and easing back to his own (over 3 seconds in the air, 1.5 on the ground); its way
		// turned by the stick, readily (pulling back slows it down)
		f32 speed       = sqrtf(s_glideX * s_glideX + s_glideZ * s_glideZ);
		const f32 own   = sqrtf(dx * dx + dz * dz);
		f32 way         = atan2f(s_glideX, s_glideZ);
		if ((m->mInput & 1) && m->mIntendedMag > 1.0f) {
			const f32 want = (f32)m->mIntendedYaw * (3.14159265f / 32768.0f);
			f32 diff       = want - way;
			while (diff > 3.14159265f)
				diff -= 6.2831853f;
			while (diff < -3.14159265f)
				diff += 6.2831853f;
			const f32 push = m->mIntendedMag / 32.0f > 1.0f ? 1.0f : m->mIntendedMag / 32.0f;
			// radians this tick (in the air the mod's setting: 0 none, 50 percent 5 a second, 100 at once)
			const f32 turn = air ? (s_airTurn >= 0.999f ? 10.0f : 10.0f * s_airTurn * dt * push) : 7.0f * dt * push;
			way += diff > turn ? turn : diff < -turn ? -turn : diff;
			if (fabsf(diff) > 2.2f) // pulling back: slowing down
				speed *= powf(0.5f, dt * push / 0.5f);
		}
		speed += (own - speed) * (1.0f - powf(0.5f, dt / (air ? 3.0f : 1.5f)));
		s_glideX = sinf(way) * speed, s_glideZ = cosf(way) * speed;
		if (!air && speed <= own * 1.05f + 0.3f)
			s_gliding = false; // back to his own
	} else
		s_gliding = false;
	if (s_gliding)
		glideTo(m, s_prevX + s_glideX, s_prevZ + s_glideZ); // (from where the game put him, walls in the way)
	s_prevX = m->mPosition.x, s_prevZ = m->mPosition.z;
	s_havePrev = true;
}

// Mario standing in a puddle: hurt as by the game's own goop of its kind
void hurtMario(f32 dt)
{
	TMario* m = gpMarioOriginal;
	if (!m || m->onYoshi() || m->checkFlag(MARIO_FLAG_GAME_OVER) || m->checkFlag(MARIO_FLAG_IN_WATER))
		return;
	for (int i = 0; i < kWalls && !m->isInvincible(); i++) { // goop on a wall he touches
		const WallGoop& g = s_wallGoop[i];
		if (!g.on)
			continue;
		const f32 dx = m->mPosition.x - g.x, dz = m->mPosition.z - g.z, dy = m->mPosition.y + 80.0f - g.y;
		const f32 out = dx * g.nx + dz * g.nz, r = wallSizeNow(g);
		if (out < -10.0f || out > 50.0f || dx * dx + dz * dz - out * out > r * r || fabsf(dy) > r + 80.0f)
			continue;
		if (g.kind == kBlack)
			continue;
		if (g.kind == kFire)
			m->checkGraffitoFire();
		else if (m->mStatus != MARIO_STATUS_ELECTRIC_DAMAGE) {
			m->changePlayerStatus(MARIO_STATUS_ELECTRIC_DAMAGE, 0, false);
			SMSGetMSound()->startSoundActor(MSD_SE_MA_DAMAGE_ELEC, &m->mPosition, 0, nullptr, 0, 4);
		}
		return;
	}
	if (m->mPosition.y > m->mFloorPosition.y + 12.0f) // in the air
		return;
	if (s_grassBurn && port_grass_burning && port_grass_burning(m->mPosition.x, m->mPosition.z) && !m->isInvincible()) {
		m->checkGraffitoFire(); // the grass on fire under him
		return;
	}
	for (int i = 0; i < kPuddles; i++) {
		Puddle& p = s_puddles[i];
		if (!p.on)
			continue;
		const f32 r = sizeNow(p) * 0.85f, dx = m->mPosition.x - p.x, dz = m->mPosition.z - p.z;
		if (dx * dx + dz * dz > r * r || fabsf(m->mPosition.y - p.y) > 40.0f) {
			p.inside = 0.0f;
			continue;
		}
		p.inside += dt;
		if (p.kind == kBlack)
			continue; // (slippery: blackGoop)
		if (m->isInvincible())
			continue;
		if (p.kind == kFire) {
			const u32 before = m->mStatus;
			m->checkGraffitoFire(); // burning, a point of health...
			if (m->mStatus == MARIO_STATUS_FIRE_DOWN && before != MARIO_STATUS_FIRE_DOWN) {
				// ...launched up two and a half times his height, a random way
				f32 g = m->mJumpParams.mGravity.get();
				g     = g > 0.05f ? g : 2.0f;
				m->mVel.y       = sqrtf(2.0f * g * 2.5f * 160.0f);
				m->mFaceAngle.y = (s16)(random01() * 65535.0f);
				m->mForwardVel  = 12.0f;
			}
			return;
		}
		const u32 type = m->mStatus & MARIO_STATUS_TYPE_MASK;
		if (p.inside >= 0.15f && m->mStatus != MARIO_STATUS_ELECTRIC_DAMAGE
		    && (type == MARIO_STATUS_TYPE_WAITING || type == MARIO_STATUS_TYPE_RUNNING)) {
			m->changePlayerStatus(MARIO_STATUS_ELECTRIC_DAMAGE, 0, false); // shocked, a point of health
			SMSGetMSound()->startSoundActor(MSD_SE_MA_DAMAGE_ELEC, &m->mPosition, 0, nullptr, 0, 4);
			SMSGetMSound()->startSoundActor(MSD_SE_EF_LIGHTNING, &m->mPosition, 0, nullptr, 0, 4);
			return;
		}
	}
}

// enemies standing in a puddle (every tenth of a second)
// the fire goop puddle nearest (x, y, z) within `reach` of its edge (level with it), or null; `edge`
// how far outside its edge (less than 0: inside)
const Puddle* nearFire(f32 x, f32 y, f32 z, f32 reach, f32* edge)
{
	const Puddle* best = 0;
	f32 bestEdge       = 1e30f;
	for (int i = 0; i < kPuddles; i++) {
		const Puddle& p = s_puddles[i];
		if (!p.on || p.kind != kFire || fabsf(p.y - y) > 300.0f)
			continue;
		const f32 dx = p.x - x, dz = p.z - z;
		const f32 out = sqrtf(dx * dx + dz * dz) - sizeNow(p);
		if (out < reach && out < bestEdge)
			bestEdge = out, best = &p;
	}
	if (edge)
		*edge = bestEdge;
	return best;
}

bool flyingNow(TLiveActor* e)
{
	for (int i = 0; i < kFlights; i++)
		if (s_flights[i].a == e)
			return true;
	return false;
}
bool zappedNow(TLiveActor* e)
{
	for (int i = 0; i < kZaps; i++)
		if (s_zaps[i].e == e)
			return true;
	return false;
}

// Which enemies move by themselves: each small enemy's own movement across the ground (not counting
// the walk into the fire) added up since the stage began; one that never moves on its own (a
// Piranha Plant, say) is never drawn to the fire
const int kMovers = 256;
struct Mover {
	TLiveActor* e;
	f32 x, z, moved;
	bool held;   // walked to the fire by us last frame: its own moving since (after Mario) undone
	f32 hx, hz;  // where we left it
	f32 hitAt;   // when water last hit it (its push kept)
	f32 heldAt;  // when (held only from just before)
};
Mover s_movers[kMovers];
Mover* moverOf(TLiveActor* e)
{
	Mover* free = 0;
	for (int i = 0; i < kMovers; i++) {
		if (s_movers[i].e == e)
			return &s_movers[i];
		if (!free && !s_movers[i].e)
			free = &s_movers[i];
	}
	if (free) {
		free->e = e, free->x = e->mPosition.x, free->z = e->mPosition.z, free->moved = 0.0f;
		free->held = false, free->hitAt = -100.0f, free->heldAt = -100.0f;
	}
	return free;
}
void watchMovers(TLiveManager* mgr)
{
	if (!mgr)
		return;
	for (int i = 0; i < mgr->getObjNum(); i++) {
		TLiveActor* e = mgr->getObj(i);
		if (!e || !dynamic_cast<TSmallEnemy*>(e))
			continue;
		Mover* m = moverOf(e);
		if (!m)
			continue;
		const f32 dx = e->mPosition.x - m->x, dz = e->mPosition.z - m->z, d = sqrtf(dx * dx + dz * dz);
		if (d < 400.0f && !flyingNow(e) && !(m->held && s_time - m->heldAt < 0.2f)) // (not a jump back to where it began, nor a throw)
			m->moved += d;
		m->x = e->mPosition.x, m->z = e->mPosition.z;
	}
}
bool movesItself(TLiveActor* e)
{
	for (int i = 0; i < kMovers; i++)
		if (s_movers[i].e == e)
			return s_movers[i].moved > 60.0f;
	return false;
}

// Enemies drawn to fire goop (the ones that move by themselves): each small enemy within 2500 of a fire puddle walks into it (on top of
// its own moving, turned that way, kept out of walls), and stays in it once there
void walkToFire(TLiveManager* mgr, f32 dt)
{
	if (!mgr || !gpMap)
		return;
	for (int i = 0; i < mgr->getObjNum(); i++) {
		TLiveActor* e = mgr->getObj(i);
		if (!e || !dynamic_cast<TSmallEnemy*>(e))
			continue;
		Mover* mv = 0;
		for (int k = 0; k < kMovers && !mv; k++)
			if (s_movers[k].e == e)
				mv = &s_movers[k];
		const bool wasHeld = mv && mv->held && s_time - mv->heldAt < 0.2f;
		if (mv)
			mv->held = false;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN) || e->checkLiveFlag(LIVE_FLAG_AIRBORNE)
		    || flyingNow(e) || zappedNow(e) || !mv || !movesItself(e))
			continue;
		// drawn to the fire, Mario ignored: its own moving since last frame (chasing him, say) undone,
		// unless water has just pushed it
		if (wasHeld && s_time - mv->hitAt > 0.5f)
			e->mPosition.x = mv->hx, e->mPosition.z = mv->hz;
		f32 edge;
		const Puddle* p = nearFire(e->mPosition.x, e->mPosition.y, e->mPosition.z, 2500.0f, &edge);
		if (!p)
			continue;
		f32 dx = p->x - e->mPosition.x, dz = p->z - e->mPosition.z;
		const f32 d = sqrtf(dx * dx + dz * dz);
		const f32 r = sizeNow(*p);
		if (d < r * 0.4f || d < 1.0f) { // well in it: kept there
			mv->heldAt = s_time, mv->held = true, mv->hx = e->mPosition.x, mv->hz = e->mPosition.z;
			mv->x = mv->hx, mv->z = mv->hz;
			continue;
		}
		dx /= d, dz /= d;
		f32 step = 260.0f * dt;
		if (step > d - r * 0.4f)
			step = d - r * 0.4f;
		f32 px = e->mPosition.x + dx * step, pz = e->mPosition.z + dz * step;
		TBGWallCheckRecord wall(px, e->mPosition.y + 40.0f, pz, 40.0f, 4, TBGWallCheckRecord::IGNORE_WATER_THROUGH);
		if (gpMap->isTouchedWallsAndMoveXZ(&wall))
			px = wall.mCenter.x, pz = wall.mCenter.z;
		e->mPosition.x = px, e->mPosition.z = pz;
		e->mRotation.y = atan2f(dx, dz) * 57.29578f; // facing the fire
		mv->x = px, mv->z = pz; // (this walk not its own)
		mv->heldAt = s_time, mv->held = true, mv->hx = px, mv->hz = pz;
	}
}

// a Phantamanta on fire goop: burnt as by three hits of water (16, 8, 4, 2, 1 by its generation),
// half a second apart; `game`: the game makes the hit itself after this (a drop it counts)
void mantaFire(TBossManta* m, bool game)
{
	if (!m || m->checkLiveFlag(LIVE_FLAG_DEAD) || !m->isDamageable())
		return;
	if (gpMarioParticleManager)
		gpMarioParticleManager->emit(PARTICLE_MS_MOE_FIRE_OFF, &m->mPosition, 0, nullptr);
	static const int kHits[6] = {16, 8, 4, 2, 1, 1};
	const int most = kHits[m->mGeneration >= 0 && m->mGeneration < 6 ? m->mGeneration : 0];
	m->unk19C += 2;
	if (m->unk19C > most - 1)
		m->unk19C = most - 1;
	fprintf(stderr, "[goop] fire on a Phantamanta (generation %d): %d of %d hits\n", m->mGeneration, m->unk19C + 1, most);
	if (game)
		return;
	m->collidedWithWater();
	if (m->unk1A0 > 15)
		m->unk1A0 = 15;
}

void hurtEnemiesIn(TLiveManager* mgr)
{
	if (!mgr)
		return;
	for (int i = 0; i < mgr->getObjNum(); i++) {
		TLiveActor* e = mgr->getObj(i);
		if (TBossManta* m = dynamic_cast<TBossManta*>(e)) { // Phantamanta: hurt swimming over fire goop
			if (m->checkLiveFlag(LIVE_FLAG_DEAD) || m->checkLiveFlag(LIVE_FLAG_HIDDEN))
				continue;
			const f32 body = 120.0f * (m->mScaling.x > 0.05f ? m->mScaling.x : 1.0f); // (about half its width)
			for (int k = 0; k < kPuddles; k++) {
				const Puddle& p = s_puddles[k];
				if (!p.on || p.kind != kFire)
					continue;
				const f32 r = sizeNow(p) + body, dx = m->mPosition.x - p.x, dz = m->mPosition.z - p.z;
				if (dx * dx + dz * dz <= r * r && fabsf(m->mPosition.y - p.y) < 200.0f) {
					mantaFire(m, false);
					break;
				}
			}
			continue;
		}
		if (!e || (!dynamic_cast<TSmallEnemy*>(e) && !(s_npcs && isNpc(e))) || e->checkLiveFlag(LIVE_FLAG_DEAD)
		    || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
			continue;
		if (s_grassBurn && port_grass_burning && port_grass_burning(e->mPosition.x, e->mPosition.z)) {
			burn(e, kFire); // in the grass on fire
			continue;
		}
		for (int k = 0; k < kPuddles; k++) {
			const Puddle& p = s_puddles[k];
			if (!p.on || p.kind == kBlack)
				continue;
			const f32 r = sizeNow(p) + 20.0f, dx = e->mPosition.x - p.x, dz = e->mPosition.z - p.z;
			if (dx * dx + dz * dz <= r * r && fabsf(e->mPosition.y - p.y) < 60.0f) {
				burn(e, p.kind, true); // (fire goop: launched up)
				break;
			}
		}
	}
}

// a goop drop in the air (one fired just before another weapon was chosen too)
bool goopFlying()
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !port_spray_tagged_drops)
		return false;
	for (int i = 0; i < (int)w->mParticleCount; i++)
		if ((w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f && dropKind(i))
			return true;
	return false;
}

f32 s_enemyCheck;
bool anyPuddle()
{
	for (int i = 0; i < kPuddles; i++)
		if (s_puddles[i].on)
			return true;
	for (int i = 0; i < kWalls; i++)
		if (s_wallGoop[i].on)
			return true;
	return false;
}

void undress();
void dressFludd();

// the water a goop weapon uses: FLUDD's own (what left the tank this tick), times its setting
s32 s_lastWater = -1;
f32 s_waterCarry;
void drain()
{
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!g) {
		s_lastWater = -1;
		return;
	}
	s32 now = g->mCurrentWater;
	if (s_kind && s_lastWater >= 0 && now < s_lastWater) {
		const f32 more = (f32)(s_lastWater - now) * (statsOf(s_kind).water - 1.0f) + s_waterCarry;
		const s32 whole = (s32)more;
		s_waterCarry    = more - (f32)whole;
		now -= whole;
		const s32 most = g->mNozzleList[0] ? (s32)g->mNozzleList[0]->mEmitParams.mAmountMax.get() : now;
		now            = now < 0 ? 0 : now > most ? most : now;
		g->mCurrentWater = now;
	}
	s_lastWater = now;
}
void stageSetup()
{
	undress(); // (its textures' memory goes with the stage)
	memset(s_puddles, 0, sizeof s_puddles);
	memset(s_movers, 0, sizeof s_movers);
	memset(s_wallGoop, 0, sizeof s_wallGoop);
	memset(s_zaps, 0, sizeof s_zaps);
	memset(s_flights, 0, sizeof s_flights);
}

void afterMovement()
{
	const f32 dt = port_frame_scale() / 30.0f;
	s_time += dt;
	flights(dt);
	zaps();
	dressFludd();
	drain();
	blackGoop(dt);
	if (gpConductor) // (which enemies move by themselves, known before any fire)
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			watchMovers(*it);
	bool any = false;
	for (int i = 0; i < kPuddles; i++) {
		Puddle& p = s_puddles[i];
		if (p.on && s_time - p.born > lifeOf(p))
			p.on = false; // dried away
		any = any || p.on;
	}
	for (int i = 0; i < kWalls; i++) {
		WallGoop& g = s_wallGoop[i];
		if (g.on && s_time - g.born > s_wallLife)
			g.on = false; // dried away
		any = any || g.on;
	}
	const bool grassFire = s_grassBurn && port_grass_fires && port_grass_fires(0, 0) > 0;
	if (!any && !grassFire)
		return;
	hurtMario(dt);
	if (gpConductor)
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			walkToFire(*it, dt);
	if (port_grass_goop) // the grass under it: pressed flat, or burnt away
		for (int i = 0; i < kPuddles; i++) {
			const Puddle& p = s_puddles[i];
			if (p.on && (p.kind == kFire ? s_grassBurn : s_grassPress))
				port_grass_goop(p.x, p.y, p.z, sizeNow(p) * 0.95f, p.kind == kFire ? 1 : 0, (int)(dt * 1000.0f)); // (black: pressed, as electric)
		}
	if (s_time - s_enemyCheck >= 0.1f && gpConductor) {
		s_enemyCheck = s_time;
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			hurtEnemiesIn(*it);
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
			hurtEnemiesIn(*it);
	}
}

// an electric goop drop about to reach an enemy: its velocity kept, so the game's push of the
// water is taken back after (objectHitDone): shocked, not pushed
TLiveActor* s_unpushed;
JGeometry::TVec3<f32> s_unpushedV;
TBossManta* s_fireManta; // a Phantamanta fire goop just hit
int objectHit(void* object, void* sender, unsigned message)
{
	s_unpushed = 0;
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender)
		return 0;
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	if (drop && dropKind(drop->mParticleIndex) == kFire)
		if (TBossManta* m = PortSprayManta(object)) {
			// Phantamanta: fire goop hurts it as water does (a hit, then its own wait before the next),
			// where the game would not count the drop (only drops flagged 0x40 hurt it)
			// each fire hit counts three of water's (16, 8, 4, 2, 1 by its generation), and the next
			// comes half a second later (water's: a second). A drop the game counts (flagged 0x40)
			// gets its own hit after this; else the hit is made here
			if (m->isDamageable()) {
				const bool game = (gpModelWaterManager->mParticleFlagSOA[drop->mParticleIndex] & 0x40) != 0;
				mantaFire(m, game);
				if (game)
					s_fireManta = m; // (its wait shortened after the game's hit: objectHitDone)
			}
			return 0;
		}
	if (!drop || dropKind(drop->mParticleIndex) != kElectric)
		return 0; // (fire: pushed by the water as ever, or thrown into the fire)
	if (TLiveActor* e = dynamic_cast<TLiveActor*>(static_cast<THitActor*>(object))) {
		s_unpushed  = e;
		s_unpushedV = e->getVelocity();
	}
	return 0;
}

// a goop drop reached something: an enemy hit by the stream. Electric goop: shocked (held
// trembling with sparks for its seconds, a shock counted after), never pushed; fire goop: thrown
// up a random way burning, the burn counted at once
void objectHitDone(void* object, void* sender, unsigned message, int)
{
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender)
		return;
	if (s_fireManta) { // its next hit half a second away
		if (s_fireManta->unk1A0 > 15)
			s_fireManta->unk1A0 = 15;
		s_fireManta = 0;
	}
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	if (!drop)
		return;
	const int kind = dropKind(drop->mParticleIndex);
	TLiveActor* e = dynamic_cast<TLiveActor*>(static_cast<THitActor*>(object));
	if (e && !dynamic_cast<TSmallEnemy*>(e) && !(s_npcs && isNpc(e)))
		e = 0; // (an enemy or, with the setting, a character)
	if (e && e == s_unpushed)
		e->setVelocity(s_unpushedV); // (the water's push taken back)
	s_unpushed = 0;
	if (e)
		for (int k = 0; k < kMovers; k++)
			if (s_movers[k].e == e)
				s_movers[k].hitAt = s_time; // (its push by the water kept)
	if (!kind && e && dynamic_cast<TSmallEnemy*>(e) && !e->checkLiveFlag(LIVE_FLAG_DEAD)) {
		// water (FLUDD's or a weapon's) on an enemy near fire goop: thrown into the fire
		f32 edge;
		if (nearFire(e->mPosition.x, e->mPosition.y, e->mPosition.z, 700.0f, &edge) && edge > 0.0f)
			launchEnemy(e);
		return;
	}
	if (kind == kElectric)
		burn(e, kind); // the shock as from its goop
	else if (kind == kFire)
		burn(e, kind, true, true); // thrown up burning, at once
}

// ---- drawing: the stream's goop and the puddles, a packet of the scene's opaque draw buffer ----
//
// Both kinds are textured as the game's own goop of that kind looks: electric goop marbled teal
// and yellow, glossy; fire goop orange with darker red swirls, flames rising off it (soft, see-
// through, added to what is behind). The textures are made here once (tileable, 128 square), laid
// on the ground by its own place in the world (overlapping puddles join up seamlessly), and flow
// slowly.

Mtx s_view;
bool s_haveView;
int s_frame;
class GoopPacket : public J3DPacket {
public:
	virtual void draw();
};
GoopPacket s_packet;

const int kTex = 128;
u8 s_texElectric[kTex * kTex * 4] __attribute__((aligned(32)));
u8 s_texBlack[kTex * kTex * 4] __attribute__((aligned(32)));
u8 s_texFire[kTex * kTex * 4] __attribute__((aligned(32)));
u8 s_texWater[kTex * kTex * 4] __attribute__((aligned(32))); // the glints of the water over electric goop
u8 s_waterAlpha[kTex * kTex];                                   // (as rows, for FLUDD's look)
GXTexObj s_objElectric, s_objFire, s_objWater, s_objBlack;
bool s_madeTextures;
const f32 kTile = 300.0f; // world units a texture covers

// tileable value noise (period cells across the texture)
f32 lattice(int x, int y, int period, u32 salt)
{
	x = ((x % period) + period) % period;
	y = ((y % period) + period) % period;
	u32 h = (u32)x * 374761393u + (u32)y * 668265263u + salt * 2246822519u;
	h     = (h ^ (h >> 13)) * 1274126177u;
	return (f32)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
f32 vnoise(f32 x, f32 y, int period, u32 salt)
{
	const int ix = (int)floorf(x), iy = (int)floorf(y);
	f32 fx = x - (f32)ix, fy = y - (f32)iy;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const f32 a = lattice(ix, iy, period, salt), b = lattice(ix + 1, iy, period, salt);
	const f32 c = lattice(ix, iy + 1, period, salt), d = lattice(ix + 1, iy + 1, period, salt);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
// u, v in 0..1 (tileable): four octaves
f32 fbm(f32 u, f32 v, u32 salt)
{
	f32 sum = 0.0f, amp = 0.5f;
	int period = 3;
	for (int o = 0; o < 4; o++) {
		sum += amp * vnoise(u * (f32)period, v * (f32)period, period, salt + (u32)o * 17u);
		amp *= 0.5f;
		period *= 2;
	}
	return sum / 0.9375f;
}
f32 smooth(f32 a, f32 b, f32 x)
{
	f32 t = (x - a) / (b - a);
	t     = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

// a texture into the GameCube's RGBA8 layout (4x4 blocks: alpha and red, then green and blue)
u8 s_rgbaElectric[kTex * kTex * 4], s_rgbaFire[kTex * kTex * 4], s_rgbaBlack[kTex * kTex * 4]; // as rows, for FLUDD's own look

void store(u8* out, const f32* rgb)
{
	u8* lin = out == s_texFire ? s_rgbaFire : out == s_texBlack ? s_rgbaBlack : s_rgbaElectric;
	for (int i = 0; i < kTex * kTex; i++) {
		for (int ch = 0; ch < 3; ch++)
			lin[i * 4 + ch] = (u8)(rgb[i * 3 + ch] < 0.0f ? 0 : rgb[i * 3 + ch] > 255.0f ? 255 : (int)rgb[i * 3 + ch]);
		lin[i * 4 + 3] = 255;
	}
	for (int by = 0; by < kTex / 4; by++)
		for (int bx = 0; bx < kTex / 4; bx++) {
			u8* block = out + ((by * (kTex / 4) + bx) * 64);
			for (int k = 0; k < 16; k++) {
				const int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
				const f32* c = rgb + (y * kTex + x) * 3;
				u8 v[3];
				for (int ch = 0; ch < 3; ch++)
					v[ch] = (u8)(c[ch] < 0.0f ? 0 : c[ch] > 255.0f ? 255 : (int)c[ch]);
				block[k * 2]          = 255;
				block[k * 2 + 1]      = v[0];
				block[32 + k * 2]     = v[1];
				block[32 + k * 2 + 1] = v[2];
			}
		}
}

// noise of its own period and octaves
f32 fbmp(f32 u, f32 v, u32 salt, int period, int octaves)
{
	f32 sum = 0.0f, amp = 0.5f, total = 0.0f;
	for (int o = 0; o < octaves; o++) {
		sum += amp * vnoise(u * (f32)period, v * (f32)period, period, salt + (u32)o * 17u);
		total += amp;
		amp *= 0.5f;
		period *= 2;
	}
	return sum / total;
}

// electric goop as the game's: smooth teal paint with yellow marbled into it (long thin streaks and
// broad swirls), sharp edged, not shaded (its gloss is the water over it)
void makeElectric(u8* out, bool black = false)
{
	static f32 rgb[kTex * kTex * 3];
	for (int y = 0; y < kTex; y++)
		for (int x = 0; x < kTex; x++) {
			const f32 u = (f32)x / kTex, v = (f32)y / kTex;
			const f32 qx = fbmp(u, v, 11u, 2, 3), qy = fbmp(u + 0.37f, v + 0.11f, 12u, 2, 3);
			const f32 rx = fbmp(u + 1.2f * qx, v + 1.2f * qy, 13u, 2, 4), ry = fbmp(u + 1.2f * qx + 0.2f, v + 1.2f * qy + 0.6f, 14u, 2, 4);
			const f32 f      = fbmp(u + rx, v + ry, 15u, 2, 2);
			const f32 streak = smooth(0.90f, 0.94f, 0.5f + 0.5f * sinf(f * 30.0f));
			const f32 blob   = smooth(0.63f, 0.66f, fbmp(u + 0.6f * rx, v + 0.6f * ry, 16u, 2, 3));
			const f32 m      = streak > blob ? streak : blob;
			const f32 g      = fbmp(u + 0.3f * rx, v, 17u, 2, 3);
			// black goop: black where electric is teal, grey where it is yellow
			const f32 tr = black ? 10.0f + 16.0f * g : 25.0f + 20.0f * g, tg = black ? 10.0f + 15.0f * g : 175.0f + 30.0f * g,
			          tb = black ? 12.0f + 18.0f * g : 175.0f + 25.0f * g;
			const int i = y * kTex + x;
			// its streaks: yellow (black goop's grey)
			const f32 sr = black ? 140.0f : 245.0f, sg = black ? 140.0f : 235.0f, sb = black ? 145.0f : 30.0f;
			rgb[i * 3]     = tr + (sr - tr) * m;
			rgb[i * 3 + 1] = tg + (sg - tg) * m;
			rgb[i * 3 + 2] = tb + (sb - tb) * m;
		}
	store(out, rgb);
}

// the water over it: thin white glints, wavy lines here and there (in its alpha)
void makeWater()
{
	for (int y = 0; y < kTex; y++)
		for (int x = 0; x < kTex; x++) {
			const f32 u = (f32)x / kTex, v = (f32)y / kTex;
			const f32 wx = fbmp(u, v, 41u, 2, 3), wy = fbmp(u + 0.5f, v + 0.3f, 42u, 2, 3);
			const f32 c    = fbmp(u + 0.8f * wx, v + 0.8f * wy, 43u, 2, 2);
			const f32 line = 1.0f - smooth(0.0f, 0.07f, fabsf(sinf(c * 28.0f)));
			const f32 mask = smooth(0.52f, 0.62f, fbmp(u + 0.4f * wy, v + 0.4f * wx, 44u, 2, 4));
			const f32 a    = line * mask;
			s_waterAlpha[y * kTex + x] = (u8)(a * 255.0f);
		}
	for (int by = 0; by < kTex / 4; by++)
		for (int bx = 0; bx < kTex / 4; bx++) {
			u8* block = s_texWater + ((by * (kTex / 4) + bx) * 64);
			for (int k = 0; k < 16; k++) {
				const int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
				block[k * 2]          = s_waterAlpha[y * kTex + x];
				block[k * 2 + 1]      = 255;
				block[32 + k * 2]     = 255;
				block[32 + k * 2 + 1] = 255;
			}
		}
}

// marbled goop: swirled bands of two colours, glossy (a light's shine on its ripples)
void makeGoop(u8* out, bool electric)
{
	static f32 h[kTex * kTex], rgb[kTex * kTex * 3];
	static f32 mix[kTex * kTex];
	for (int y = 0; y < kTex; y++)
		for (int x = 0; x < kTex; x++) {
			const f32 u = (f32)x / kTex, v = (f32)y / kTex;
			const f32 qx = fbm(u, v, electric ? 11u : 31u), qy = fbm(u + 0.37f, v + 0.11f, electric ? 12u : 32u);
			const f32 rx = fbm(u + 0.9f * qx, v + 0.9f * qy, electric ? 13u : 33u);
			const f32 ry = fbm(u + 0.9f * qx + 0.2f, v + 0.9f * qy + 0.6f, electric ? 14u : 34u);
			const f32 f  = fbm(u + 0.8f * rx, v + 0.8f * ry, electric ? 15u : 35u);
			const f32 band = 0.5f + 0.5f * sinf(f * (electric ? 16.0f : 12.0f) + qx * 3.0f);
			mix[y * kTex + x] = electric ? smooth(0.74f, 0.86f, band) : smooth(0.62f, 0.9f, band);
			h[y * kTex + x]   = f + 0.08f * mix[y * kTex + x];
		}
	const f32 lx = -0.45f, ly = -0.55f, lz = 0.70f; // the light (normalised below)
	const f32 ll = sqrtf(lx * lx + ly * ly + lz * lz);
	for (int y = 0; y < kTex; y++)
		for (int x = 0; x < kTex; x++) {
			const int i = y * kTex + x;
			const f32 dx = h[y * kTex + ((x + 1) & (kTex - 1))] - h[y * kTex + ((x + kTex - 1) & (kTex - 1))];
			const f32 dy = h[((y + 1) & (kTex - 1)) * kTex + x] - h[((y + kTex - 1) & (kTex - 1)) * kTex + x];
			f32 nx = -dx * 4.5f, ny = -dy * 4.5f, nz = 1.0f;
			const f32 nl = sqrtf(nx * nx + ny * ny + nz * nz);
			nx /= nl, ny /= nl, nz /= nl;
			const f32 diffuse = nx * lx / ll + ny * ly / ll + nz * lz / ll;
			// the shine: halfway between the light and straight up (the viewer)
			f32 hx = lx / ll, hy = ly / ll, hz = lz / ll + 1.0f;
			const f32 hl = sqrtf(hx * hx + hy * hy + hz * hz);
			f32 sp = (nx * hx + ny * hy + nz * hz) / hl;
			sp = sp > 0.0f ? powf(sp, electric ? 70.0f : 50.0f) : 0.0f;
			const f32 m = mix[i], g = fbm((f32)x / kTex + 0.5f, (f32)y / kTex, 77u);
			f32 r, gr, b;
			if (electric) { // teal (lighter and darker) and yellow
				const f32 tr = 15.0f + 25.0f * g, tg = 140.0f + 60.0f * g, tb = 140.0f + 50.0f * g;
				r = tr + (235.0f - tr) * m, gr = tg + (225.0f - tg) * m, b = tb + (25.0f - tb) * m;
			} else { // orange, darker red swirls, a lighter yellow-orange here and there
				const f32 lite = smooth(0.55f, 0.8f, g);
				const f32 or_ = 255.0f, og = 120.0f + 40.0f * lite, ob = 20.0f + 20.0f * lite;
				r = or_ + (205.0f - or_) * m, gr = og + (55.0f - og) * m, b = ob + (12.0f - ob) * m;
			}
			const f32 shade = 0.8f + 0.25f * diffuse;
			const f32 shine = sp * (electric ? 260.0f : 120.0f);
			rgb[i * 3]     = r * shade + shine;
			rgb[i * 3 + 1] = gr * shade + shine;
			rgb[i * 3 + 2] = b * shade + shine;
		}
	store(out, rgb);
}

void makeTextures()
{
	if (s_madeTextures)
		return;
	makeElectric(s_texElectric);
	makeElectric(s_texBlack, true);
	GXInitTexObj(&s_objBlack, s_texBlack, kTex, kTex, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GXInitTexObjLOD(&s_objBlack, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	makeGoop(s_texFire, false);
	makeWater();
	GXInitTexObj(&s_objWater, s_texWater, kTex, kTex, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GXInitTexObjLOD(&s_objWater, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXInitTexObj(&s_objElectric, s_texElectric, kTex, kTex, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GXInitTexObjLOD(&s_objElectric, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXInitTexObj(&s_objFire, s_texFire, kTex, kTex, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GXInitTexObjLOD(&s_objFire, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	s_madeTextures = true;
}

void corner(f32 x, f32 y, f32 z, int r, int g, int b)
{
	GXPosition3f32(x, y, z);
	GXColor4u8((u8)(r < 0 ? 0 : r > 255 ? 255 : r), (u8)(g < 0 ? 0 : g > 255 ? 255 : g), (u8)(b < 0 ? 0 : b > 255 ? 255 : b), 255);
}
// a textured corner: its colour multiplies the texture
void tcorner(f32 x, f32 y, f32 z, int shade, f32 u, f32 v)
{
	GXPosition3f32(x, y, z);
	const u8 s = (u8)(shade < 0 ? 0 : shade > 255 ? 255 : shade);
	GXColor4u8(s, s, s, 255);
	GXTexCoord2f32(u, v);
}
// a see-through corner (the flames)
void acorner(f32 x, f32 y, f32 z, int r, int g, int b, int a)
{
	GXPosition3f32(x, y, z);
	GXColor4u8((u8)r, (u8)g, (u8)b, (u8)(a < 0 ? 0 : a > 255 ? 255 : a));
}

f32 flow(int kind) { return (f32)s_frame / 60.0f * (kind == kFire ? 10.0f : 5.0f); } // the goop's slow flow

void textured(int kind)
{
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXLoadTexObj(kind == kFire ? &s_objFire : kind == kElectric ? &s_objElectric : kind == kBlack ? &s_objBlack : &s_objWater, GX_TEXMAP0);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

// the stream's drops of one kind: each a smooth round blob of goop, drawn out along its flight so
// the blobs run into one another as a stream; lit from the view (its middle bright, its edges dim)
void drawDrops(int kind)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !port_spray_tagged_drops)
		return;
	const int kAround = 8, kBands = 4, kVerts = kAround * 6 * (kBands - 1); // (fans at both ends, quads between)
	const int kBatch = 60000 / kVerts;
	f32 ring[kBands + 1][2]; // each latitude's (along, across)
	for (int b = 0; b <= kBands; b++) {
		const f32 th = 3.14159265f * (f32)b / kBands;
		ring[b][0] = cosf(th), ring[b][1] = sinf(th);
	}
	f32 ca[kAround], sa[kAround];
	for (int k = 0; k < kAround; k++)
		ca[k] = cosf(6.2831853f * k / kAround), sa[k] = sinf(6.2831853f * k / kAround);
	const f32 vx = s_view[2][0], vy = s_view[2][1], vz = s_view[2][2]; // toward the viewer
	// FLUDD held in view (first person): a thinner stream, thinner still near the view
	const bool held = gpMarioOriginal && PortFluddHeld(gpMarioOriginal);
	f32 eye[3];
	for (int c = 0; c < 3; c++)
		eye[c] = -(s_view[0][c] * s_view[0][3] + s_view[1][c] * s_view[1][3] + s_view[2][c] * s_view[2][3]);
	const f32 fl0 = flow(kind);
	int i = 0;
	bool set = false;
	while (i < (int)w->mParticleCount) {
		int n = 0, j = i;
		for (; j < (int)w->mParticleCount && n < kBatch; j++)
			if ((w->mParticleFlagSOA[j] & 0xf) == 1 && w->mParticleLifetimeSOA[j] > 0.0f && dropKind(j) == kind)
				n++;
		if (!n)
			break;
		if (!set)
			textured(kind), set = true;
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * kVerts));
		for (; i < j; i++) {
			if ((w->mParticleFlagSOA[i] & 0xf) != 1 || w->mParticleLifetimeSOA[i] <= 0.0f || dropKind(i) != kind)
				continue;
			const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
			const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[i];
			f32 r = w->mParticleSizeSOA[i] * 0.55f * statsOf(kind).drop;
			if (held) {
				const f32 ex = p.x - eye[0], ey = p.y - eye[1], ez = p.z - eye[2];
				const f32 d  = sqrtf(ex * ex + ey * ey + ez * ez) / 500.0f;
				r *= 0.6f * (d < 0.5f ? 0.5f : d > 1.0f ? 1.0f : d);
			}
			f32 fx = v.x, fy = v.y, fz = v.z;
			f32 fl = sqrtf(fx * fx + fy * fy + fz * fz);
			if (fl < 1e-3f)
				fx = 0.0f, fy = 1.0f, fz = 0.0f, fl = 1.0f;
			fx /= fl, fy /= fl, fz /= fl;
			f32 ax = -fz, ay = 0.0f, az = fx; // across it
			f32 al = sqrtf(ax * ax + az * az);
			if (al < 1e-3f)
				ax = 1.0f, az = 0.0f, al = 1.0f;
			ax /= al, az /= al;
			const f32 bx = fy * az - fz * ay, by = fz * ax - fx * az, bz = fx * ay - fy * ax;
			// a round blob (a little longer along its flight): the tubes between the drops
			// (drawTubes) make the stream unbroken
			const f32 len = r * 1.3f;
			const f32 u0 = (f32)(i % 7) * 0.13f + fl0 / kTile, v0 = (f32)(i % 5) * 0.17f;
			f32 pts[kBands + 1][kAround][3];
			int shade[kBands + 1][kAround];
			f32 uv[kBands + 1][kAround][2];
			for (int b = 0; b <= kBands; b++)
				for (int k = 0; k < kAround; k++) {
					const f32 along = ring[b][0] * len, across = ring[b][1] * r;
					const f32 nx = ax * ca[k] + bx * sa[k], ny = ay * ca[k] + by * sa[k], nz = az * ca[k] + bz * sa[k];
					pts[b][k][0] = p.x + fx * along + nx * across;
					pts[b][k][1] = p.y + fy * along + ny * across;
					pts[b][k][2] = p.z + fz * along + nz * across;
					// its surface's way out (round, roughly), toward the viewer: brighter
					const f32 ox = nx * ring[b][1] + fx * ring[b][0], oy = ny * ring[b][1] + fy * ring[b][0], oz = nz * ring[b][1] + fz * ring[b][0];
					const f32 facing = ox * vx + oy * vy + oz * vz;
					shade[b][k] = (int)(150.0f + 105.0f * (facing > 0.0f ? facing : 0.0f));
					uv[b][k][0] = u0 + (f32)k / kAround * 0.35f;
					uv[b][k][1] = v0 + (f32)b / kBands * 0.25f;
				}
			for (int b = 0; b < kBands; b++)
				for (int k = 0; k < kAround; k++) {
					const int m = (k + 1) % kAround;
					if (b > 0) {
						tcorner(pts[b][k][0], pts[b][k][1], pts[b][k][2], shade[b][k], uv[b][k][0], uv[b][k][1]);
						tcorner(pts[b][m][0], pts[b][m][1], pts[b][m][2], shade[b][m], uv[b][m][0], uv[b][m][1]);
						tcorner(pts[b + 1][k][0], pts[b + 1][k][1], pts[b + 1][k][2], shade[b + 1][k], uv[b + 1][k][0], uv[b + 1][k][1]);
					}
					if (b < kBands - 1) {
						tcorner(pts[b][m][0], pts[b][m][1], pts[b][m][2], shade[b][m], uv[b][m][0], uv[b][m][1]);
						tcorner(pts[b + 1][m][0], pts[b + 1][m][1], pts[b + 1][m][2], shade[b + 1][m], uv[b + 1][m][0], uv[b + 1][m][1]);
						tcorner(pts[b + 1][k][0], pts[b + 1][k][1], pts[b + 1][k][2], shade[b + 1][k], uv[b + 1][k][0], uv[b + 1][k][1]);
					}
				}
		}
		GXEnd();
	}
}

// the stream between its drops: each drop joined to the one let out just before it (by their
// lifetimes, a tick or so apart) with a tube as thick as the drops, so the stream is one unbroken
// line out of the nozzle however Mario moves
int lifeOrder(const void* a, const void* b)
{
	const f32 la = gpModelWaterManager->mParticleLifetimeSOA[*(const int*)a];
	const f32 lb = gpModelWaterManager->mParticleLifetimeSOA[*(const int*)b];
	return la > lb ? -1 : la < lb ? 1 : 0;
}
void drawTubes(int kind)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !port_spray_tagged_drops)
		return;
	static int order[1024];
	int n = 0;
	for (int i = 0; i < (int)w->mParticleCount && n < 1024; i++)
		if ((w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f && dropKind(i) == kind)
			order[n++] = i;
	if (n < 2)
		return;
	qsort(order, n, sizeof *order, lifeOrder);
	const bool held = gpMarioOriginal && PortFluddHeld(gpMarioOriginal);
	f32 eye[3];
	for (int c = 0; c < 3; c++)
		eye[c] = -(s_view[0][c] * s_view[0][3] + s_view[1][c] * s_view[1][3] + s_view[2][c] * s_view[2][3]);
	const f32 vx = s_view[2][0], vy = s_view[2][1], vz = s_view[2][2];
	// each piece of tube: its two ends, how thick there, and a drop for its texture's place
	struct Piece { f32 p[2][3]; f32 rad[2]; int id; };
	static Piece pieces[1025];
	int m = 0;
	struct Thick {
		static f32 at(f32 size, int kind, bool held, const f32* eye, const f32* o)
		{
			f32 r = size * 0.55f * statsOf(kind).drop * 0.85f;
			if (held) {
				const f32 ex = o[0] - eye[0], ey = o[1] - eye[1], ez = o[2] - eye[2];
				const f32 d  = sqrtf(ex * ex + ey * ey + ez * ez) / 500.0f;
				r *= 0.6f * (d < 0.5f ? 0.5f : d > 1.0f ? 1.0f : d);
			}
			return r;
		}
	};
	// FLUDD held in view and firing: the stream starts right at its nozzle (joined to the newest drop)
	f32 scale = 1.0f;
	MtxPtr em = held && PortSprayWeaponTrigger() ? PortFluddHeldEmitMtx(&scale) : 0;
	if (em) {
		const int a = order[0];
		const JGeometry::TVec3<f32>& pa = w->mParticlePositionSOA[a];
		const f32 dx = pa.x - em[0][3], dy = pa.y - em[1][3], dz = pa.z - em[2][3];
		if (dx * dx + dy * dy + dz * dz < 900.0f * 900.0f) {
			Piece& pc = pieces[m++];
			pc.p[0][0] = em[0][3], pc.p[0][1] = em[1][3], pc.p[0][2] = em[2][3];
			pc.p[1][0] = pa.x, pc.p[1][1] = pa.y, pc.p[1][2] = pa.z;
			pc.rad[1] = Thick::at(w->mParticleSizeSOA[a], kind, held, eye, pc.p[1]);
			pc.rad[0] = pc.rad[1] * 0.7f; // (a little thinner out of the nozzle)
			pc.id = a;
		}
	}
	for (int k = 0; k + 1 < n && m < 1025; k++) {
		const int a = order[k], b = order[k + 1];
		if (w->mParticleLifetimeSOA[a] - w->mParticleLifetimeSOA[b] > 3.5f)
			continue; // not let out one after the other
		const JGeometry::TVec3<f32>& pa = w->mParticlePositionSOA[a];
		const JGeometry::TVec3<f32>& pb = w->mParticlePositionSOA[b];
		const JGeometry::TVec3<f32>& v  = w->mParticleVelocitySOA[a];
		const f32 dx = pb.x - pa.x, dy = pb.y - pa.y, dz = pb.z - pa.z;
		const f32 gap2 = 4.0f * (v.x * v.x + v.y * v.y + v.z * v.z) + 900.0f;
		if (dx * dx + dy * dy + dz * dz > gap2)
			continue; // too far apart: not the same stream
		Piece& pc = pieces[m++];
		pc.p[0][0] = pa.x, pc.p[0][1] = pa.y, pc.p[0][2] = pa.z;
		pc.p[1][0] = pb.x, pc.p[1][1] = pb.y, pc.p[1][2] = pb.z;
		pc.rad[0] = Thick::at(w->mParticleSizeSOA[a], kind, held, eye, pc.p[0]);
		pc.rad[1] = Thick::at(w->mParticleSizeSOA[b], kind, held, eye, pc.p[1]);
		pc.id = a;
	}
	if (!m)
		return;
	textured(kind);
	const int kSides = 8;
	f32 ca[kSides], sa[kSides];
	for (int q = 0; q < kSides; q++)
		ca[q] = cosf(6.2831853f * q / kSides), sa[q] = sinf(6.2831853f * q / kSides);
	const f32 fl0 = flow(kind);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(m * kSides * 6));
	for (int t = 0; t < m; t++) {
		const Piece& pc = pieces[t];
		const int a = pc.id;
		const f32* rad = pc.rad;
		const JGeometry::TVec3<f32> pa(pc.p[0][0], pc.p[0][1], pc.p[0][2]);
		const JGeometry::TVec3<f32> pb(pc.p[1][0], pc.p[1][1], pc.p[1][2]);
		const JGeometry::TVec3<f32>* ends[2] = {&pa, &pb};
		f32 fx = pb.x - pa.x, fy = pb.y - pa.y, fz = pb.z - pa.z;
		const f32 fl = sqrtf(fx * fx + fy * fy + fz * fz) + 1e-4f;
		fx /= fl, fy /= fl, fz /= fl;
		f32 ax = -fz, ay = 0.0f, az = fx;
		f32 al = sqrtf(ax * ax + az * az);
		if (al < 1e-3f)
			ax = 1.0f, az = 0.0f, al = 1.0f;
		ax /= al, az /= al;
		const f32 bx = fy * az - fz * ay, by = fz * ax - fx * az, bz = fx * ay - fy * ax;
		const f32 u0 = (f32)(a % 7) * 0.13f + fl0 / kTile;
		for (int q = 0; q < kSides; q++) {
			const int q2 = (q + 1) % kSides;
			f32 pt[4][3];
			int sh[4];
			const int sides[4] = {q, q2, q2, q};
			for (int c = 0; c < 4; c++) {
				const int e     = c < 2 ? 0 : 1;
				const f32 nx    = ax * ca[sides[c]] + bx * sa[sides[c]], ny = ay * ca[sides[c]] + by * sa[sides[c]],
				          nz    = az * ca[sides[c]] + bz * sa[sides[c]];
				const JGeometry::TVec3<f32>& o = *ends[e];
				pt[c][0] = o.x + nx * rad[e], pt[c][1] = o.y + ny * rad[e], pt[c][2] = o.z + nz * rad[e];
				const f32 facing = nx * vx + ny * vy + nz * vz;
				sh[c] = (int)(150.0f + 105.0f * (facing > 0.0f ? facing : 0.0f));
			}
			const f32 us[4] = {u0 + (f32)q / kSides * 0.35f, u0 + (f32)(q + 1) / kSides * 0.35f, u0 + (f32)(q + 1) / kSides * 0.35f, u0 + (f32)q / kSides * 0.35f};
			const f32 vs[4] = {0.0f, 0.0f, 0.2f, 0.2f};
			const int tri[6] = {0, 1, 2, 0, 2, 3};
			for (int c = 0; c < 6; c++)
				tcorner(pt[tri[c]][0], pt[tri[c]][1], pt[tri[c]][2], sh[tri[c]], us[tri[c]], vs[tri[c]]);
		}
	}
	GXEnd();
}

void basis(const Puddle& p, f32* t, f32* b)
{
	// two ways across the ground there
	f32 tx = p.ny, ty = -p.nx, tz = 0.0f; // across, from the normal
	f32 tl = sqrtf(tx * tx + ty * ty + tz * tz);
	if (tl < 1e-3f)
		tx = 1.0f, ty = 0.0f, tl = 1.0f;
	tx /= tl, ty /= tl;
	t[0] = tx, t[1] = ty, t[2] = tz;
	b[0] = p.ny * tz - p.nz * ty, b[1] = p.nz * tx - p.nx * tz, b[2] = p.nx * ty - p.ny * tx;
}

// a puddle's wavy edge at angle a (its radius r there): broad lobes and small bumps, as the
// game's own goop's edge
f32 edgeAt(const Puddle& p, f32 a, f32 r)
{
	return r * (1.0f + 0.11f * sinf(3.0f * a + p.seed) + 0.06f * sinf(5.0f * a + p.seed * 1.7f)
	            + 0.035f * sinf(9.0f * a + p.seed * 2.3f) + 0.022f * sinf(13.0f * a + p.seed * 3.1f)
	            + 0.014f * sinf(17.0f * a + p.seed * 4.3f));
}
// how thick goop lies (its top over the ground), and its rounded edge's width
f32 thickOf(f32 r) { return r < 60.0f ? r * 0.06f : 3.6f; }
f32 rimOf(f32 r) { return r < 60.0f ? r * 0.12f : 7.0f; }

// (x, y, z) well inside another puddle of the same kind (inside its edge by more than its rim):
// this puddle's edge there joins that one's goop, no rim (overlapping puddles look one)
bool underOther(int self, int kind, f32 x, f32 y, f32 z)
{
	for (int i = 0; i < kPuddles; i++) {
		const Puddle& o = s_puddles[i];
		if (i == self || !o.on || o.kind != kind)
			continue;
		const f32 dx = x - o.x, dy = y - o.y, dz = z - o.z;
		if (dy > 80.0f || dy < -80.0f)
			continue;
		const f32 r = sizeNow(o);
		if (r < 1.0f)
			continue;
		const f32 d2 = dx * dx + dz * dz;
		if (d2 > r * r * 1.5f)
			continue;
		const f32 sure = r * 0.7f - rimOf(r) * 1.5f; // (its wavy edge never comes in further than this)
		if (sure > 0.0f && d2 < sure * sure)
			return true;
		f32 t[3], b[3];
		basis(o, t, b);
		const f32 a = atan2f(dx * b[0] + dy * b[1] + dz * b[2], dx * t[0] + dy * t[1] + dz * t[2]);
		const f32 in = edgeAt(o, a, r) - rimOf(r) * 1.5f;
		if (in > 0.0f && d2 < in * in)
			return true;
	}
	return false;
}

// a puddle: a round of the goop's texture with a wavy edge, lying a little thick on the ground with
// a rounded, darker edge (as the game's goop), joining other puddles of its kind where they overlap
// (a web of rings, so the thin layer of water over it can ripple). `gloss`: that water's shine
// instead (untextured, added on top), else the goop (textured() set)
void drawPuddle(const Puddle& p, int index, bool gloss = false)
{
	const f32 r = sizeNow(p);
	if (r < 1.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const f32 lift = 1.0f + 0.04f * (f32)index + (gloss ? 0.3f : 0.0f); // over the ground (and over each other)
	const f32 cx = p.x + p.nx * lift, cy = p.y + p.ny * lift, cz = p.z + p.nz * lift;
	const f32 time   = (f32)s_frame / 60.0f;
	const bool water = true; // every kind under a thin layer of moving water, bending it a little
	// fire goop flickers and flows; electric goop lies still, the water over it bending it a little
	const int shade = p.kind == kFire ? (int)(255.0f * (0.93f + 0.07f * sinf(time * 9.0f + p.seed))) : 255;
	const f32 fl    = p.kind == kFire ? flow(p.kind) : 0.0f;
	// the light the water shines with, and the way to the viewer
	const f32 lx = 0.35f, ly = 0.8f, lz = 0.45f;
	f32 hx = lx + s_view[2][0], hy = ly + s_view[2][1], hz = lz + s_view[2][2];
	const f32 hl = sqrtf(hx * hx + hy * hy + hz * hz);
	hx /= hl, hy /= hl, hz /= hl;
	// rings out from the middle: the flat top (by the radius less the rim), then its rounded edge
	// (down to the ground, darker: the goop's side)
	const int kAround = 56, kRings = 7;
	const f32 thick = thickOf(r), rim = rimOf(r);
	// each ring: how far in from the edge (in rims), how high (of its thickness), how lit
	const f32 inRim[kRings + 1]  = {0, 0, 0, 0, 0, 1.0f, 0.45f, 0.0f};
	const f32 outF[kRings + 1]   = {0.0f, 0.3f, 0.55f, 0.75f, 0.9f, 1.0f, 1.0f, 1.0f};
	const f32 high[kRings + 1]   = {1, 1, 1, 1, 1, 1.0f, 0.8f, 0.0f};
	const f32 lit[kRings + 1]    = {1, 1, 1, 1, 1, 0.97f, 0.78f, 0.5f};
	static f32 pos[kRings + 1][kAround][3], uv[kRings + 1][kAround][2];
	static int col[kRings + 1][kAround];
	// where its edge runs under another puddle: worked out every 8th frame for each puddle (in turn),
	// or when it is new or has grown, and kept (the check against every other puddle is costly: it
	// slowed the game with many puddles together)
	static bool s_joined[kPuddles][kAround];
	static f32 s_joinedBorn[kPuddles], s_joinedSize[kPuddles];
	const int slot = index >= 0 && index < kPuddles ? index : 0;
	if (!gloss && (s_joinedBorn[slot] != p.born || fabsf(s_joinedSize[slot] - r) > 6.0f || (s_frame + slot) % 8 == 0)) {
		s_joinedBorn[slot] = p.born, s_joinedSize[slot] = r;
		for (int k = 0; k < kAround; k++) {
			const f32 a  = 6.2831853f * (f32)k / kAround;
			const f32 rr = edgeAt(p, a, r);
			const f32 ex = cx + (t[0] * cosf(a) + b[0] * sinf(a)) * rr, ey = cy + (t[1] * cosf(a) + b[1] * sinf(a)) * rr,
			          ez = cz + (t[2] * cosf(a) + b[2] * sinf(a)) * rr;
			s_joined[slot][k] = underOther(index, p.kind, ex, ey, ez);
		}
	}
	const bool* joined = s_joined[slot];
	for (int j = 0; j <= kRings; j++)
		for (int k = 0; k < kAround; k++) {
			const f32 a   = 6.2831853f * (f32)k / kAround;
			const f32 rr  = edgeAt(p, a, r);
			const bool jn = joined[k] || joined[(k + 1) % kAround] || joined[(k + kAround - 1) % kAround];
			// the flat top reaches its edge less the rim; joined, it runs on flat under the other
			const f32 top = rr - rim > rr * 0.4f ? rr - rim : rr * 0.4f;
			f32 d         = j <= 4 ? top * outF[j] : top + (rr - top) * (1.0f - inRim[j]);
			f32 h  = thick * high[j];
			int lt = (int)(255.0f * lit[j]);
			if (jn && j > 4)
				h = thick, lt = 255, d = rr * 1.04f; // (into the other puddle, flat)
			const f32 ca = cosf(a) * d, sa = sinf(a) * d;
			const f32 x = cx + t[0] * ca + b[0] * sa + p.nx * h, y = cy + t[1] * ca + b[1] * sa + p.ny * h,
			          z = cz + t[2] * ca + b[2] * sa + p.nz * h;
			pos[j][k][0] = x, pos[j][k][1] = y, pos[j][k][2] = z;
			f32 du = 0.0f, dv = 0.0f, sx = 0.0f, sz = 0.0f;
			if (water) { // the water's ripples: the goop under it shifted a little, its shine moving
				const f32 w1 = x * 0.045f + time * 1.6f, w2 = z * 0.06f - time * 1.2f, w3 = z * 0.05f + time * 1.4f, w4 = x * 0.07f + time * 1.9f;
				du = 3.5f * sinf(w1) + 2.5f * sinf(w2);
				dv = 3.5f * sinf(w3) + 2.5f * sinf(w4);
				sx = 0.16f * cosf(w1) + 0.17f * cosf(w4); // its slope across
				sz = 0.15f * cosf(w2) + 0.17f * cosf(w3);
			}
			uv[j][k][0] = (x + fl + du) / kTile, uv[j][k][1] = (z + dv) / kTile;
			if (gloss) {
				f32 nx = p.nx - sx, ny = p.ny, nz = p.nz - sz;
				const f32 nl = sqrtf(nx * nx + ny * ny + nz * nz);
				f32 sp = (nx * hx + ny * hy + nz * hz) / nl;
				sp = sp > 0.0f ? powf(sp, 24.0f) : 0.0f;
				const f32 edge = j == kRings && !jn ? 0.3f : 1.0f; // fading at its edge
				col[j][k] = (int)(sp * 200.0f * edge + 18.0f * edge); // a faint sheen everywhere, bright where it catches the light
			} else
				col[j][k] = shade * lt / 255;
		}
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(kAround * 3 + kAround * (kRings - 1) * 6));
	for (int k = 0; k < kAround; k++) { // the middle: ring 1 round it
		const int m = (k + 1) % kAround;
		const int c[3][2] = {{0, k}, {1, k}, {1, m}};
		for (int v = 0; v < 3; v++) {
			const int j = c[v][0], q = c[v][1];
			if (gloss)
				acorner(pos[j][q][0], pos[j][q][1], pos[j][q][2], 255, 255, 255, col[j][q]);
			else
				tcorner(pos[j][q][0], pos[j][q][1], pos[j][q][2], col[j][q], uv[j][q][0], uv[j][q][1]);
		}
	}
	for (int j = 1; j < kRings; j++)
		for (int k = 0; k < kAround; k++) {
			const int m = (k + 1) % kAround;
			const int c[6][2] = {{j, k}, {j + 1, k}, {j + 1, m}, {j, k}, {j + 1, m}, {j, m}};
			for (int v = 0; v < 6; v++) {
				const int jj = c[v][0], q = c[v][1];
				if (gloss)
					acorner(pos[jj][q][0], pos[jj][q][1], pos[jj][q][2], 255, 255, 255, col[jj][q]);
				else
					tcorner(pos[jj][q][0], pos[jj][q][1], pos[jj][q][2], col[jj][q], uv[jj][q][0], uv[jj][q][1]);
			}
		}
	GXEnd();
}

f32 hash01(u32 h);
// goop on walls of a kind: a blob on the wall with drips running down from it, growing longer
void drawWalls(int kind)
{
	bool any = false;
	for (int i = 0; i < kWalls && !any; i++)
		any = s_wallGoop[i].on && s_wallGoop[i].kind == kind;
	if (!any)
		return;
	textured(kind);
	const f32 time = (f32)s_frame / 60.0f;
	for (int i = 0; i < kWalls; i++) {
		const WallGoop& g = s_wallGoop[i];
		if (!g.on || g.kind != kind)
			continue;
		const f32 r = wallSizeNow(g);
		if (r < 1.0f)
			continue;
		// across the wall (level), and down it
		const f32 tx = -g.nz, tz = g.nx;
		const f32 lift = 1.2f + 0.03f * (f32)i;
		const f32 cx = g.x + g.nx * lift, cy = g.y, cz = g.z + g.nz * lift;
		const int kAround = 20;
		GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(kAround + 2));
		tcorner(cx, cy, cz, 255, (cx * tx + cz * tz) / kTile, -cy / kTile);
		for (int k = 0; k <= kAround; k++) {
			const f32 a  = 6.2831853f * (f32)(k % kAround) / kAround;
			const f32 rr = r * (1.0f + 0.14f * sinf(3.0f * a + g.seed) + 0.08f * sinf(5.0f * a + g.seed * 1.3f));
			const f32 ox = cosf(a) * rr, oy = sinf(a) * rr;
			const f32 x = cx + tx * ox, y = cy + oy, z = cz + tz * ox;
			tcorner(x, y, z, 235, (x * tx + z * tz) / kTile, -y / kTile);
		}
		GXEnd();
		// the drips: running down from its lower half, longer with time, each its own
		const int drips   = 3 + (int)(r / 25.0f);
		const f32 running = (s_time - g.since) / 5.0f; // reaching their length in five seconds
		const f32 most    = (r * 1.4f + 40.0f) * (running < 1.0f ? running : 1.0f);
		const int kBulb = 12; // (the round drop at its end)
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(drips * (6 + kBulb * 3)));
		for (int d = 0; d < drips; d++) {
			const u32 key  = (u32)i * 131u + (u32)d * 977u + (u32)(g.seed * 10.0f);
			const f32 off  = (hash01(key) * 2.0f - 1.0f) * r * 0.65f;
			const f32 wide = r * (0.08f + 0.08f * hash01(key + 1u));
			const f32 len  = most * (0.35f + 0.65f * hash01(key + 2u)) * (0.97f + 0.03f * sinf(time * 2.0f + (f32)d));
			const f32 top  = cy - r * 0.2f, bottom = cy - r * 0.55f - len;
			const f32 bx = cx + tx * off, bz = cz + tz * off;
			const f32 u0 = ((bx - tx * wide) * tx + (bz - tz * wide) * tz) / kTile, u1 = ((bx + tx * wide) * tx + (bz + tz * wide) * tz) / kTile;
			tcorner(bx - tx * wide, top, bz - tz * wide, 245, u0, -top / kTile);
			tcorner(bx + tx * wide, top, bz + tz * wide, 245, u1, -top / kTile);
			tcorner(bx + tx * wide, bottom, bz + tz * wide, 225, u1, -bottom / kTile);
			tcorner(bx - tx * wide, top, bz - tz * wide, 245, u0, -top / kTile);
			tcorner(bx + tx * wide, bottom, bz + tz * wide, 225, u1, -bottom / kTile);
			tcorner(bx - tx * wide, bottom, bz - tz * wide, 225, u0, -bottom / kTile);
			// its end: a round drop hanging, a little wider than the drip (a circle, a bit taller than wide)
			const f32 br = wide * 1.25f, by = bottom - br * 0.35f;
			for (int k = 0; k < kBulb; k++) {
				const f32 a0 = 6.2831853f * (f32)k / kBulb, a1 = 6.2831853f * (f32)(k + 1) / kBulb;
				const f32 x0 = cosf(a0) * br, y0 = sinf(a0) * br * 1.15f, x1 = cosf(a1) * br, y1 = sinf(a1) * br * 1.15f;
				tcorner(bx, by, bz, 225, ((bx) * tx + (bz) * tz) / kTile, -by / kTile);
				tcorner(bx + tx * x0, by + y0, bz + tz * x0, 215, ((bx + tx * x0) * tx + (bz + tz * x0) * tz) / kTile, -(by + y0) / kTile);
				tcorner(bx + tx * x1, by + y1, bz + tz * x1, 215, ((bx + tx * x1) * tx + (bz + tz * x1) * tz) / kTile, -(by + y1) / kTile);
			}
		}
		GXEnd();
	}
}

f32 hash01(u32 h)
{
	h = (h ^ 61u) ^ (h >> 16);
	h *= 9u;
	h = h ^ (h >> 4);
	h *= 0x27d4eb2du;
	h = h ^ (h >> 15);
	return (f32)(h & 0xFFFF) / 65535.0f;
}

// a flame at (x, y, z): a soft teardrop (a fan of six) `h` high now, `tall` at most, facing the view
void flame(f32 x, f32 y, f32 z, f32 tall, f32 h, f32 sway)
{
	const f32 rx = s_view[0][0], rz = s_view[0][2]; // the view's right
	const f32 w = tall * 0.32f;
	const f32 pts[7][2] = {{0.0f, 0.0f},  {-w, 0.3f * h},          {-0.5f * w, 0.7f * h}, {sway, 1.05f * h},
	                       {0.5f * w, 0.7f * h}, {w, 0.3f * h}, {0.0f, 0.0f}};
	const int al[7] = {120, 110, 70, 0, 70, 110, 120};
	const f32 my    = 0.35f * h;
	for (int k = 0; k < 6; k++) {
		acorner(x, y + my, z, 255, 150, 70, 210);
		acorner(x + rx * pts[k][0], y + pts[k][1], z + rz * pts[k][0], 255, 80, 60, al[k]);
		acorner(x + rx * pts[k + 1][0], y + pts[k + 1][1], z + rz * pts[k + 1][0], 255, 80, 60, al[k + 1]);
	}
}

// grass on fire (fire goop's fire running through it, port_grass.cpp): a flame at each burning place
void drawGrassFires()
{
	static float at[300 * 3];
	const int n = port_grass_fires ? port_grass_fires(at, 300) : 0;
	if (!n)
		return;
	const f32 time = (f32)s_frame / 60.0f;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * 18));
	for (int i = 0; i < n; i++) {
		const u32 key  = (u32)((int)(at[i * 3] / 12.0f) * 73856093) ^ (u32)((int)(at[i * 3 + 2] / 12.0f) * 19349663);
		const f32 tall = 30.0f + 25.0f * hash01(key);
		const f32 h    = tall * (0.6f + 0.4f * sinf(time * 9.0f + hash01(key + 1u) * 6.28f));
		flame(at[i * 3], at[i * 3 + 1], at[i * 3 + 2], tall, h, 0.12f * tall * sinf(time * 6.0f + (f32)i));
	}
	GXEnd();
}

// fire: flames rising off it, soft and see-through, each one flaring up and dying away somewhere
// new; turned to face the view
void drawFlames(const Puddle& p, int index)
{
	const f32 r = sizeNow(p);
	if (r < 10.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const int count = 2 + (int)(r / 45.0f);
	const f32 time  = (f32)s_frame / 60.0f;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(count * 18));
	for (int i = 0; i < count; i++) {
		const f32 phase = time * 1.3f + (f32)i * 0.618f + p.seed;
		const int cycle = (int)floorf(phase);
		const f32 life  = phase - (f32)cycle;
		const u32 key   = (u32)index * 977u + (u32)i * 131u + (u32)cycle * 7919u;
		const f32 a = hash01(key) * 6.2831853f, d = r * 0.8f * sqrtf(hash01(key + 1u));
		const f32 ox = cosf(a) * d, oz = sinf(a) * d;
		const f32 x = p.x + t[0] * ox + b[0] * oz, y = p.y + t[1] * ox + b[1] * oz, z = p.z + t[2] * ox + b[2] * oz;
		const f32 tall = (45.0f + r * 0.12f) * (0.7f + 0.6f * hash01(key + 2u));
		const f32 h    = tall * sinf(3.14159f * life);
		flame(x, y, z, tall, h, 0.12f * tall * sinf(time * 6.0f + (f32)i * 2.1f));
	}
	GXEnd();
}

void GoopPacket::draw()
{
	if (!s_haveView)
		return;
	makeTextures();
	s_frame++;
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(s_view, GX_PNMTX0);
	GXLoadNrmMtxImm(identity, GX_PNMTX0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetNumIndStages(0);
	GXSetNumTevStages(1);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor){0, 0, 0, 0});
	GXSetColorUpdate(GX_TRUE);
	for (int kind = kFire; kind <= kBlack; kind++) {
		bool any = false;
		for (int i = 0; i < kPuddles && !any; i++)
			any = s_puddles[i].on && s_puddles[i].kind == kind;
		if (any) {
			textured(kind);
			for (int i = 0; i < kPuddles; i++)
				if (s_puddles[i].on && s_puddles[i].kind == kind)
					drawPuddle(s_puddles[i], i);
			if (false && kind == kElectric) { // (the water's shine added on top: too white, left out)
				GXClearVtxDesc();
				GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
				GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
				GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
				GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
				GXSetNumTexGens(0);
				GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
				GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
				GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
				GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
				for (int i = 0; i < kPuddles; i++)
					if (s_puddles[i].on && s_puddles[i].kind == kind)
						drawPuddle(s_puddles[i], i, true);
				GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
				GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
			}
		}
		drawWalls(kind);
		drawDrops(kind);
		drawTubes(kind);
	}
	// the flames last: soft, added to what is behind, not hiding what is behind them
	bool fire = port_grass_fires && port_grass_fires(0, 0) > 0;
	for (int i = 0; i < kWalls && !fire; i++)
		fire = s_wallGoop[i].on && s_wallGoop[i].kind == kFire;
	for (int i = 0; i < kPuddles && !fire; i++)
		fire = s_puddles[i].on && s_puddles[i].kind == kFire;
	if (fire) {
		GXClearVtxDesc();
		GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
		GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		GXSetNumTexGens(0);
		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
		for (int i = 0; i < kPuddles; i++)
			if (s_puddles[i].on && s_puddles[i].kind == kFire)
				drawFlames(s_puddles[i], i);
		drawGrassFires();
		for (int i = 0; i < kWalls; i++) { // fire goop on walls: a flame or two off it
			const WallGoop& g = s_wallGoop[i];
			const f32 r       = g.on && g.kind == kFire ? wallSizeNow(g) : 0.0f;
			if (r < 8.0f)
				continue;
			const f32 time = (f32)s_frame / 60.0f;
			GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(2 * 18));
			for (int k = 0; k < 2; k++) {
				const f32 tall = 30.0f + r * 0.3f;
				flame(g.x + g.nx * 8.0f - g.nz * (k ? 0.3f : -0.3f) * r, g.y - r * 0.2f, g.z + g.nz * 8.0f + g.nx * (k ? 0.3f : -0.3f) * r,
				      tall, tall * (0.6f + 0.4f * sinf(time * 8.0f + g.seed + (f32)k * 2.0f)), 0.1f * tall * sinf(time * 5.0f + (f32)k));
			}
			GXEnd();
		}
		GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	}
}

// ---- FLUDD dressed in its goop: its textures (body and nozzles) shown as the goop's, throbbing
// brighter and dimmer and flowing (a picture in place of each, renewed twelve times a second);
// its own again when another weapon is chosen. Cut-out textures (alpha-tested) are left as they are.

struct Dressed {
	const void* data;
	u32 bytes;
};
const int kDressMost = 48;
Dressed s_dressed[kDressMost];
int s_dressedCount;
f32 s_dressNext;
u8 s_pulse[kTex * kTex * 4];
bool s_namesTold;

void undress()
{
	if (GXPC_SetTextureOverride)
		for (int i = 0; i < s_dressedCount; i++)
			GXPC_SetTextureOverride(s_dressed[i].data, s_dressed[i].bytes, 0, 0, 0);
	s_dressedCount = 0;
}

void dressModel(J3DModel* model)
{
	J3DModelData* data   = model ? model->getModelData() : 0;
	J3DTexture* textures = data ? data->getTexture() : 0;
	JUTNameTab* names    = data ? data->getTextureName() : 0;
	if (!textures)
		return;
	for (u16 i = 0; i < textures->getNum(); i++) {
		const ResTIMG* t = textures->getResTIMG(i);
		const char* name = names ? names->getName(i) : 0;
		if (!s_namesTold && t)
			fprintf(stderr, "[goop] FLUDD texture %s (%dx%d, format %d, alpha %d)\n", name ? name : "?", t->width, t->height,
			        t->format, t->alphaEnabled);
		if (!t || t->alphaEnabled == 1)
			continue; // a cut-out
		// where GX reads it: the header's offset added in 32 bits, as on the console
		const void* at  = (const void*)(uintptr_t)(u32)((uintptr_t)t + t->imageDataOffset);
		const u32 bytes = GXGetTexBufferSize(t->width, t->height, t->format, GX_FALSE, 0);
		bool known = false;
		for (int k = 0; k < s_dressedCount && !known; k++)
			known = s_dressed[k].data == at;
		if (!known && s_dressedCount < kDressMost) {
			GXPC_SetTextureOverride(at, bytes, s_pulse, kTex, kTex);
			s_dressed[s_dressedCount].data  = at;
			s_dressed[s_dressedCount].bytes = bytes;
			s_dressedCount++;
		}
	}
}

int s_dressedKind;
void dressFludd()
{
	if (!GXPC_SetTextureOverride)
		return;
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!s_kind || !g) {
		if (s_dressedCount)
			undress();
		s_dressedKind = 0;
		return;
	}
	if (s_dressedKind != s_kind) { // the goop's texture, still, in place of FLUDD's own
		undress();
		s_dressedKind = s_kind;
		makeTextures();
		memcpy(s_pulse, s_kind == kFire ? s_rgbaFire : s_kind == kBlack ? s_rgbaBlack : s_rgbaElectric, sizeof s_pulse);
		s_dressNext = 0.0f;
	}
	if (s_time < s_dressNext)
		return;
	s_dressNext = s_time + 1.0f; // (a texture of FLUDD's not yet dressed: another nozzle's, a new stage's)
	const int before = s_dressedCount;
	dressModel(g->mFluddModel ? g->mFluddModel->getModel() : 0);
	for (int n = 0; n < 6; n++) {
		TNozzleBase* nozzle = g->mNozzleList[n];
		if (nozzle && nozzle->getMActor())
			dressModel(nozzle->getMActor()->getModel());
	}
	s_namesTold = true;
	if (s_dressedCount != before)
		fprintf(stderr, "[goop] FLUDD dressed in %s goop (%d textures)\n", s_kind == kFire ? "fire" : s_kind == kBlack ? "black" : "electric", s_dressedCount);
}

PortExtension s_extension;
struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "goop weapons";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.objectHit     = objectHit;
		s_extension.objectHitDone = objectHitDone;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// The goop weapon chosen (modg_spray_weapon_set, modapi_spray_weapons.cpp): SMS_SPRAY_FIRE_GOOP,
// SMS_SPRAY_ELECTRIC_GOOP, or 0 none; and whether it may defeat enemies.
void PortGoopSetWeapon(int kind, bool defeat, int flags)
{
	s_grassPress = (flags & (1 << 28)) == 0; // set: not
	s_grassBurn  = (flags & (1 << 29)) == 0;
	s_wallsOn    = (flags & (1 << 30)) == 0;
	s_npcs       = ((unsigned)flags & 0x80000000u) == 0; // set: characters left alone
	const int now = isKind(kind) ? kind : 0;
	if (now != s_kind)
		fprintf(stderr, "[goop] %s\n", now == kFire ? "fire goop" : now == kElectric ? "electric goop" : now == kBlack ? "black goop" : "no goop");
	s_kind   = now;
	s_defeat = defeat;
}

// A goop weapon's numbers (set_weapon_stats): `every` seconds of it defeat an enemy, `range` how
// long its goop lasts (times a minute), `burst` its puddles' size, `drop` its stream's blobs' (each
// times its own); has 0: its own again. Returns 1 for a goop weapon.
int PortGoopStats(int kind, int has, float every, float range, float burst, float drop, int hits, float wall, int water, float push, float lift)
{
	if (!isKind(kind))
		return 0;
	Stats& st = statsOf(kind);
	st        = kDefaults;
	if (!has)
		return 1;
	st.defeat = every >= 0.1f ? every : 0.1f;
	st.life   = range >= 0.1f ? range : 0.1f;
	st.size   = burst >= 0.2f ? burst : 0.2f;
	st.drop   = drop >= 0.2f ? drop : 0.2f;
	st.hits   = hits >= 1 ? (hits <= 50 ? hits : 50) : 1;
	if (kind == kBlack)
		s_blackSpeed = push >= 0.5f ? (push <= 10.0f ? push : 10.0f) : 1.0f; // (push: its speed)
	if (kind == kBlack)
		s_airTurn = has ? (lift < 0.0f ? 0.0f : lift > 1.0f ? 1.0f : lift) : 0.5f; // (lift: turning in the air)
	st.water  = water > 0 ? (f32)(water <= 1000 ? water : 1000) / 100.0f : 1.0f; // (percent of FLUDD's)
	s_wallLife = wall > 0.05f ? wall * 60.0f : 30.0f; // (both kinds': minutes)
	return 1;
}

// Every request for water drops (TModelWaterManager::emitRequest, from PortSprayWeaponRequest):
// with a goop weapon chosen, FLUDD's own stream from its nozzle is made as goop (its drops tagged).
bool PortGoopEmit(const TWaterEmitInfo& asked, int* made)
{
	if (s_emitting || !s_kind || !gpModelWaterManager)
		return false;
	TMario* m    = gpMarioOriginal;
	TWaterGun* g = m ? m->mWaterGun : 0;
	if (!g || g->mCurrentNozzle != TWaterGun::Spray || m->onYoshi() || PortIsPlayer2(m))
		return false;
	const JGeometry::TVec3<f32>& at = asked.mPos.value;
	const f32 dx = at.x - g->mEmitPos[0].x, dy = at.y - g->mEmitPos[0].y, dz = at.z - g->mEmitPos[0].z;
	if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f)
		return false; // not FLUDD's stream (Shadow Mario's, a fountain's)
	TWaterEmitInfo info(asked);
	const s16 base = asked.mAttack.get();
	info.mAttack.set((s16)(((base > 0 && base < 256) ? base : 1) | (s_kind << kTagShift)));
	s_emitting = true;
	*made      = gpModelWaterManager->emitRequest(info);
	s_emitting = false;
	port_spray_tagged_drops = 1; // the game reads only the lower bits of a drop's attack from now on
	return true;
}

// Patch zzzzzzzzz-goop-weapons: a goop drop is drawn here, not as water
bool PortGoopDropHidden(int i) { return dropKind(i) != 0; }

// ...and any drop landing on the ground (its first touch): a goop drop leaves goop there and is gone
// (true); FLUDD's water washes the goop it lands in
bool PortGoopDropLands(int i, const TBGCheckData* ground)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || i < 0 || i >= (int)w->mParticleCount)
		return false;
	const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
	const int kind = dropKind(i);
	if (kind && ground && ground->mActor && PortSprayManta((void*)(THitActor*)ground->mActor))
		return false; // landing on a Phantamanta's back: the game's hit on it, no goop there (objectHit)
	if (kind) {
		addGoop(kind, p.x, p.y, p.z, ground);
		return true;
	}
	const int weapon = port_spray_tagged_drops ? ((unsigned short)w->mParticleAttackSOA[i] >> kTagShift) & 0xf : 0;
	if (weapon == 3 && PortSprayBazookaNoClean())
		return false; // the bazooka's water with the rocket jump keys held: the goop left
	if (weapon == 4) // the SMG's shot: a good splash of it
		washAt(p.x, p.y, p.z, 40.0f, 28.0f);
	else
		washAt(p.x, p.y, p.z, w->mParticleSizeSOA[i] * 0.7f, kWash);
	if (port_grass_extinguish) // and the grass on fire there put out
		port_grass_extinguish(p.x, p.z, weapon == 4 ? 70.0f : 30.0f + w->mParticleSizeSOA[i] * 0.6f);
	return false;
}

// Patch zzzzzzzzz-goop-weapons: any drop on a wall: a goop drop leaves goop there (with "Goop
// sticks to walls") and is gone (true); FLUDD's water washes the wall's goop
bool PortGoopDropWall(int i, const TBGCheckData* wall)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || i < 0 || i >= (int)w->mParticleCount)
		return false;
	const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
	const int kind = dropKind(i);
	if (kind) {
		if (s_wallsOn)
			addWallGoop(kind, p.x, p.y, p.z, wall);
		return true;
	}
	const int weapon = port_spray_tagged_drops ? ((unsigned short)w->mParticleAttackSOA[i] >> kTagShift) & 0xf : 0;
	if (weapon == 3 && PortSprayBazookaNoClean())
		return false; // (the rocket jump keys held)
	washWallAt(p.x, p.y, p.z, weapon == 4 ? 40.0f : w->mParticleSizeSOA[i] * 0.7f, weapon == 4 ? 28.0f : kWash);
	return false;
}

// The bazooka's burst washing goop off (modapi_spray_weapons.cpp's cleanGoop): the puddles in it too
void PortGoopWash(float x, float y, float z, float r)
{
	washWallAt(x, y, z, r, 1000.0f);
	for (int i = 0; i < kPuddles; i++) {
		Puddle& p = s_puddles[i];
		if (!p.on || fabsf(p.y - y) > 300.0f)
			continue;
		const f32 dx = p.x - x, dz = p.z - z;
		if (dx * dx + dz * dz <= r * r)
			p.on = false;
	}
}

// The view's matrix of this frame (modapi_weapon_models.cpp's, as FLUDD is placed)
void PortGoopView(MtxPtr view)
{
	MTXCopy(view, s_view);
	s_haveView = true;
}

// As FLUDD is entered for drawing: the goop entered too, when there is any
bool s_entered; // entered with FLUDD this frame (else drawn with the water: PortGoopWaterDraw)
bool goopToDraw()
{
	return (s_kind && port_spray_tagged_drops) || anyPuddle() || goopFlying() || (port_grass_fires && port_grass_fires(0, 0) > 0);
}
void PortGoopEntry()
{
	if (!s_haveView || !j3dSys.getDrawBuffer(0))
		return;
	if (goopToDraw()) {
		j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
		s_entered = true;
	}
}

// As the water drops are drawn (TModelWaterManager::perform's draw, patch zzzzzzzzz-goop-weapons):
// the goop drawn here when FLUDD was not drawn this frame to enter it (first person with its other
// nozzles, FLUDD hidden with Mario's body: the goop was gone)
void PortGoopWaterDraw(MtxPtr view)
{
	const bool entered = s_entered;
	s_entered          = false;
	if (entered || !view || !goopToDraw())
		return;
	PortGoopView(view);
	s_packet.draw();
}
