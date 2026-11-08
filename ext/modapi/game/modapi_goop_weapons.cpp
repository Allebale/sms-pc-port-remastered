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
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <Enemy/SmallEnemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/Conductor.hpp>
#include <Map/MapCollisionData.hpp>
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

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int port_spray_tagged_drops; // modapi_spray_weapons.cpp

namespace {

const int kTagShift = 8;  // the kind in a drop's attack, above the game's own (as the spray weapons')
const int kFire     = 5;  // SMS_SPRAY_FIRE_GOOP
const int kElectric = 6;  // SMS_SPRAY_ELECTRIC_GOOP

int s_kind;           // the goop weapon chosen: kFire, kElectric, or 0 none
bool s_defeat;        // it may defeat enemies
bool s_emitting;      // our own request, let through
f32 s_time;           // seconds played
// each kind's numbers (the mod's menu): seconds of its stream, or of standing in its goop, that
// defeat an enemy; how long its goop lasts, times a minute; its puddles' size, times their own
struct Stats {
	f32 defeat, life, size, drop;
};
const Stats kDefaults = {1.0f, 1.0f, 1.0f, 1.0f};
Stats s_stats[2]      = {kDefaults, kDefaults};
Stats& statsOf(int kind) { return s_stats[kind == kElectric ? 1 : 0]; }

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
const f32 kStartSize = 95.0f, kMostSize = 380.0f, kGrow = 5.0f, kWash = 1.6f, kGone = 14.0f;
const f32 kLife = 60.0f, kFade = 4.0f; // seconds, and the last of them shrinking away

// enemies touched by the goop: shocked (held still, trembling, sparks on them) or burning (the
// burning flames of the game's on them) for the kind's seconds, then defeated
struct Zap {
	TSmallEnemy* e;
	int kind;
	f32 start;
	f32 bx, by, bz;               // where it was shocked (held there)
	JGeometry::TVec3<f32> centre; // the sparks' place (on it)
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
	return k == kFire || k == kElectric ? k : 0;
}

f32 lifeOf(const Puddle& p) { return kLife * statsOf(p.kind).life; }

// its size now: shrinking away at the end of its life
f32 sizeNow(const Puddle& p)
{
	const f32 left = lifeOf(p) - (s_time - p.born);
	return left >= kFade ? p.r : left > 0.0f ? p.r * left / kFade : 0.0f;
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
		const f32 most = kMostSize * st.size;
		best->r        = best->r + kGrow * st.size < most ? best->r + kGrow * st.size : most;
		best->born     = s_time;
		return;
	}
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
	p->r      = kStartSize * st.size;
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

// an enemy in the stream or the goop: shocked or set burning (once), defeated when that is over
void burn(TSmallEnemy* e, int kind)
{
	if (!s_defeat || !e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
		return;
	Zap* free = 0;
	for (int i = 0; i < kZaps; i++) {
		if (s_zaps[i].e == e)
			return; // already
		if (!free && !s_zaps[i].e)
			free = &s_zaps[i];
	}
	if (!free)
		return;
	free->e     = e;
	free->kind  = kind;
	free->start = s_time;
	free->bx = e->mPosition.x, free->by = e->mPosition.y, free->bz = e->mPosition.z;
	free->centre = e->mPosition;
	SMSGetMSound()->startSoundActor(kind == kFire ? MSD_SE_EF_FIRE : MSD_SE_EN_DENNOKO_ELEC_DOWN, &e->mPosition, 0, nullptr, 0, 4);
	fprintf(stderr, "[goop] an enemy %s\n", kind == kFire ? "set burning" : "shocked");
}

// each frame: the shocked held still and trembling with sparks, the burning in flames; defeated at the end
void zaps()
{
	for (int i = 0; i < kZaps; i++) {
		Zap& z = s_zaps[i];
		if (!z.e)
			continue;
		TSmallEnemy* e = z.e;
		if (e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN)) {
			z.e = 0;
			continue;
		}
		const f32 size = e->mScaling.y > 0.1f ? e->mScaling.y : 1.0f;
		if (s_time - z.start >= statsOf(z.kind).defeat) {
			z.e = 0;
			if (gpMarioParticleManager)
				gpMarioParticleManager->emit(PARTICLE_MS_MOE_FIRE_OFF, &e->mPosition, 0, nullptr); // a puff of smoke
			e->kill(); // its own defeat, coins and all
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

// Mario standing in a puddle: hurt as by the game's own goop of its kind
void hurtMario(f32 dt)
{
	TMario* m = gpMarioOriginal;
	if (!m || m->onYoshi() || m->checkFlag(MARIO_FLAG_GAME_OVER) || m->checkFlag(MARIO_FLAG_IN_WATER))
		return;
	if (m->mPosition.y > m->mFloorPosition.y + 12.0f) // in the air
		return;
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
		if (m->isInvincible())
			continue;
		if (p.kind == kFire) {
			m->checkGraffitoFire(); // thrown up burning, a point of health
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
void hurtEnemiesIn(TLiveManager* mgr)
{
	if (!mgr)
		return;
	for (int i = 0; i < mgr->getObjNum(); i++) {
		TSmallEnemy* e = dynamic_cast<TSmallEnemy*>(mgr->getObj(i));
		if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
			continue;
		for (int k = 0; k < kPuddles; k++) {
			const Puddle& p = s_puddles[k];
			if (!p.on)
				continue;
			const f32 r = sizeNow(p) + 20.0f, dx = e->mPosition.x - p.x, dz = e->mPosition.z - p.z;
			if (dx * dx + dz * dz <= r * r && fabsf(e->mPosition.y - p.y) < 60.0f) {
				burn(e, p.kind);
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
	return false;
}

void stageSetup()
{
	memset(s_puddles, 0, sizeof s_puddles);
	memset(s_zaps, 0, sizeof s_zaps);
}

void afterMovement()
{
	const f32 dt = port_frame_scale() / 30.0f;
	s_time += dt;
	zaps();
	bool any = false;
	for (int i = 0; i < kPuddles; i++) {
		Puddle& p = s_puddles[i];
		if (p.on && s_time - p.born > lifeOf(p))
			p.on = false; // dried away
		any = any || p.on;
	}
	if (!any)
		return;
	hurtMario(dt);
	if (s_time - s_enemyCheck >= 0.1f && s_defeat && gpConductor) {
		s_enemyCheck = s_time;
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			hurtEnemiesIn(*it);
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
			hurtEnemiesIn(*it);
	}
}

// a goop drop reached something: an enemy burned or shocked by it
void objectHitDone(void* object, void* sender, unsigned message, int)
{
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER || !object || !sender || !s_defeat)
		return;
	TWaterHitActor* drop = dynamic_cast<TWaterHitActor*>(static_cast<THitActor*>(sender));
	if (!drop)
		return;
	const int kind = dropKind(drop->mParticleIndex);
	if (kind)
		burn(dynamic_cast<TSmallEnemy*>(static_cast<THitActor*>(object)), kind);
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
u8 s_texFire[kTex * kTex * 4] __attribute__((aligned(32)));
GXTexObj s_objElectric, s_objFire;
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
void store(u8* out, const f32* rgb)
{
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
	makeGoop(s_texElectric, true);
	makeGoop(s_texFire, false);
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
	GXLoadTexObj(kind == kFire ? &s_objFire : &s_objElectric, GX_TEXMAP0);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

// the stream's drops of one kind: each a small blob of goop, longer along its flight
void drawDrops(int kind)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !port_spray_tagged_drops)
		return;
	int n = 0;
	for (int i = 0; i < (int)w->mParticleCount; i++)
		if ((w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f && dropKind(i) == kind)
			n++;
	if (!n)
		return;
	n = n < 600 ? n : 600;
	textured(kind);
	const f32 fl0 = flow(kind);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * 24));
	int done = 0;
	for (int i = 0; i < (int)w->mParticleCount && done < n; i++) {
		if ((w->mParticleFlagSOA[i] & 0xf) != 1 || w->mParticleLifetimeSOA[i] <= 0.0f || dropKind(i) != kind)
			continue;
		done++;
		const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
		const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[i];
		const f32 r = w->mParticleSizeSOA[i] * 0.5f * statsOf(kind).drop;
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
		const f32 len = r * 1.8f;
		const f32 tip[2][3] = {{p.x + fx * len, p.y + fy * len, p.z + fz * len}, {p.x - fx * len, p.y - fy * len, p.z - fz * len}};
		f32 ring[4][3];
		for (int k = 0; k < 4; k++) {
			const f32 c = k == 0 ? 1.0f : k == 2 ? -1.0f : 0.0f, s = k == 1 ? 1.0f : k == 3 ? -1.0f : 0.0f;
			ring[k][0] = p.x + (ax * c + bx * s) * r;
			ring[k][1] = p.y + (ay * c + by * s) * r;
			ring[k][2] = p.z + (az * c + bz * s) * r;
		}
		// the texture over it, a piece of it each drop (by its place and number)
		const f32 u0 = (f32)(i % 7) * 0.13f + fl0 / kTile, v0 = (f32)(i % 5) * 0.17f;
		const f32 tu[2] = {u0, u0 + 0.25f}, ru[4] = {u0 + 0.12f, u0 + 0.12f, u0 + 0.12f, u0 + 0.12f};
		const f32 rv[4] = {v0, v0 + 0.06f, v0 + 0.12f, v0 + 0.18f};
		for (int t = 0; t < 2; t++)
			for (int k = 0; k < 4; k++) {
				const int j = (k + 1) & 3;
				tcorner(tip[t][0], tip[t][1], tip[t][2], 200, tu[t], v0 + 0.09f);
				tcorner(ring[k][0], ring[k][1], ring[k][2], 255, ru[k], rv[k]);
				tcorner(ring[j][0], ring[j][1], ring[j][2], 255, ru[j], rv[j]);
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

// a puddle: a round of the goop's texture with a wavy edge, flat on the ground (textured() set)
void drawPuddle(const Puddle& p, int index)
{
	const f32 r = sizeNow(p);
	if (r < 1.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const f32 lift = 1.5f + 0.04f * (f32)index; // over the ground (and over each other)
	const f32 cx = p.x + p.nx * lift, cy = p.y + p.ny * lift, cz = p.z + p.nz * lift;
	const f32 time = (f32)s_frame / 60.0f;
	// electric goop's glow throbs; fire goop's flickers
	const f32 pulse = p.kind == kFire ? 0.93f + 0.07f * sinf(time * 9.0f + p.seed) : 0.86f + 0.14f * sinf(time * 5.0f + p.seed);
	const int shade = (int)(255.0f * pulse);
	const f32 fl = flow(p.kind);
	const int kAround = 28;
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(kAround + 2));
	tcorner(cx, cy, cz, shade, (cx + fl) / kTile, cz / kTile);
	for (int k = 0; k <= kAround; k++) {
		const f32 a  = 6.2831853f * (f32)(k % kAround) / kAround;
		const f32 rr = r * (1.0f + 0.12f * sinf(3.0f * a + p.seed) + 0.07f * sinf(5.0f * a + p.seed * 1.7f) + 0.04f * sinf(9.0f * a + p.seed * 2.3f));
		const f32 ca = cosf(a) * rr, sa = sinf(a) * rr;
		const f32 x = cx + t[0] * ca + b[0] * sa, y = cy + t[1] * ca + b[1] * sa, z = cz + t[2] * ca + b[2] * sa;
		tcorner(x, y, z, shade - 25, (x + fl) / kTile, z / kTile);
	}
	GXEnd();
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

// fire: flames rising off it, soft and see-through, each one flaring up and dying away somewhere
// new; turned to face the view
void drawFlames(const Puddle& p, int index)
{
	const f32 r = sizeNow(p);
	if (r < 10.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const f32 rx = s_view[0][0], ry = s_view[0][1], rz = s_view[0][2]; // the view's right
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
		const f32 w    = tall * 0.32f;
		const f32 sway = 0.12f * tall * sinf(time * 6.0f + (f32)i * 2.1f);
		// a flame: a soft teardrop (a fan of six), brightest low in the middle, gone at its tip
		const f32 pts[7][2] = {{0.0f, 0.0f},  {-w, 0.3f * h},          {-0.5f * w, 0.7f * h}, {sway, 1.05f * h},
		                       {0.5f * w, 0.7f * h}, {w, 0.3f * h}, {0.0f, 0.0f}};
		const int al[7] = {120, 110, 70, 0, 70, 110, 120};
		const f32 mx = 0.0f, my = 0.35f * h;
		for (int k = 0; k < 6; k++) {
			acorner(x + rx * mx, y + my, z + rz * mx, 255, 150, 70, 210);
			acorner(x + rx * pts[k][0], y + pts[k][1], z + rz * pts[k][0], 255, 80, 60, al[k]);
			acorner(x + rx * pts[k + 1][0], y + pts[k + 1][1], z + rz * pts[k + 1][0], 255, 80, 60, al[k + 1]);
		}
		(void)ry;
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
	for (int kind = kFire; kind <= kElectric; kind++) {
		bool any = false;
		for (int i = 0; i < kPuddles && !any; i++)
			any = s_puddles[i].on && s_puddles[i].kind == kind;
		if (any) {
			textured(kind);
			for (int i = 0; i < kPuddles; i++)
				if (s_puddles[i].on && s_puddles[i].kind == kind)
					drawPuddle(s_puddles[i], i);
		}
		drawDrops(kind);
	}
	// the flames last: soft, added to what is behind, not hiding what is behind them
	bool fire = false;
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
		GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	}
}

PortExtension s_extension;
struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "goop weapons";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.objectHitDone = objectHitDone;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// The goop weapon chosen (modg_spray_weapon_set, modapi_spray_weapons.cpp): SMS_SPRAY_FIRE_GOOP,
// SMS_SPRAY_ELECTRIC_GOOP, or 0 none; and whether it may defeat enemies.
void PortGoopSetWeapon(int kind, bool defeat)
{
	const int now = kind == kFire || kind == kElectric ? kind : 0;
	if (now != s_kind)
		fprintf(stderr, "[goop] %s\n", now == kFire ? "fire goop" : now == kElectric ? "electric goop" : "no goop");
	s_kind   = now;
	s_defeat = defeat;
}

// A goop weapon's numbers (set_weapon_stats): `every` seconds of it defeat an enemy, `range` how
// long its goop lasts (times a minute), `burst` its puddles' size, `drop` its stream's blobs' (each
// times its own); has 0: its own again. Returns 1 for a goop weapon.
int PortGoopStats(int kind, int has, float every, float range, float burst, float drop)
{
	if (kind != kFire && kind != kElectric)
		return 0;
	Stats& st = statsOf(kind);
	st        = kDefaults;
	if (!has)
		return 1;
	st.defeat = every >= 0.1f ? every : 0.1f;
	st.life   = range >= 0.1f ? range : 0.1f;
	st.size   = burst >= 0.2f ? burst : 0.2f;
	st.drop   = drop >= 0.2f ? drop : 0.2f;
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
	if (!g || g->mCurrentNozzle != TWaterGun::Spray || m->onYoshi())
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
	if (kind) {
		addGoop(kind, p.x, p.y, p.z, ground);
		return true;
	}
	washAt(p.x, p.y, p.z, w->mParticleSizeSOA[i] * 0.5f, kWash);
	return false;
}

// The bazooka's burst washing goop off (modapi_spray_weapons.cpp's cleanGoop): the puddles in it too
void PortGoopWash(float x, float y, float z, float r)
{
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
void PortGoopEntry()
{
	if (!s_haveView || !j3dSys.getDrawBuffer(0))
		return;
	if ((s_kind && port_spray_tagged_drops) || anyPuddle() || goopFlying())
		j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
}
