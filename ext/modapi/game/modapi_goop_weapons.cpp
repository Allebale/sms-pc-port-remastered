// FLUDD's goop weapons (api->held_fludd->set_weapon SMS_SPRAY_FIRE_GOOP and SMS_SPRAY_ELECTRIC_GOOP,
// a weapons mod's): FLUDD sprays as ever (its own model, animation, pump, sound and water), but its
// stream is fire goop or electric goop, not water. Its drops carry the kind in their attack's upper
// bits (as the spray weapons' do, modapi_spray_weapons.cpp), are drawn here as goop (glowing orange,
// or yellow and crackling) instead of water (patch zzzzzzzzz-goop-weapons leaves them out of the
// water's drawing), and where one lands on the ground it leaves goop: a puddle, or a puddle there
// grown bigger. A drop on a wall or in water is just gone.
//
// The puddles are the port's own (any ground of any stage, not only the stage's goop surfaces):
// drawn here, flat on the ground, burning (flames) or crackling (sparks). Mario stepping in one gets
// hurt as by the game's own goop: fire throws him up burning (TMario::checkGraffitoFire), electric
// shocks him (MARIO_STATUS_ELECTRIC_DAMAGE). Enemies (with "can defeat enemies") are burned or
// shocked by the stream and by standing in a puddle: defeated after so many seconds of it. FLUDD's
// water landing in a puddle washes it away bit by bit (and the bazooka's burst); left alone a puddle
// lasts a minute (the mod's setting), shrinking away at its end.
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
const f32 kStartSize = 34.0f, kMostSize = 190.0f, kGrow = 1.8f, kWash = 0.9f, kGone = 12.0f;
const f32 kLife = 60.0f, kFade = 4.0f; // seconds, and the last of them shrinking away

// enemies burned or shocked lately: since when, and the last moment
struct Burn {
	TSmallEnemy* e;
	f32 start, last;
};
const int kBurns = 24;
Burn s_burns[kBurns];
const f32 kBurnGap = 0.5f; // out of it longer than this: counted afresh

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
		if (d < p.r * p.r * 0.85f && d < bestD)
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

Burn* burnOf(TSmallEnemy* e)
{
	Burn* free = 0;
	for (int i = 0; i < kBurns; i++) {
		if (s_burns[i].e == e)
			return &s_burns[i];
		if (!free && (!s_burns[i].e || s_time - s_burns[i].last > 2.0f))
			free = &s_burns[i];
	}
	if (free) {
		free->e     = e;
		free->start = free->last = s_time;
	}
	return free;
}

// an enemy in the stream or the goop this moment: defeated after its seconds of it
void burn(TSmallEnemy* e, int kind)
{
	if (!s_defeat || !e || e->checkLiveFlag(LIVE_FLAG_DEAD) || e->checkLiveFlag(LIVE_FLAG_HIDDEN))
		return;
	Burn* b = burnOf(e);
	if (!b)
		return;
	if (s_time - b->last > kBurnGap)
		b->start = s_time;
	b->last = s_time;
	if (s_time - b->start >= statsOf(kind).defeat) {
		b->e = 0;
		fprintf(stderr, "[goop] an enemy %s\n", kind == kFire ? "burned" : "shocked");
		e->kill(); // as a stomp: its own defeat, coins and all
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
	memset(s_burns, 0, sizeof s_burns);
}

void afterMovement()
{
	const f32 dt = port_frame_scale() / 30.0f;
	s_time += dt;
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

Mtx s_view;
bool s_haveView;
int s_frame;
class GoopPacket : public J3DPacket {
public:
	virtual void draw();
};
GoopPacket s_packet;

void corner(f32 x, f32 y, f32 z, int r, int g, int b)
{
	GXPosition3f32(x, y, z);
	GXColor4u8((u8)(r < 0 ? 0 : r > 255 ? 255 : r), (u8)(g < 0 ? 0 : g > 255 ? 255 : g), (u8)(b < 0 ? 0 : b > 255 ? 255 : b), 255);
}

// the stream's drops: each a small blob of goop, longer along its flight
void drawDrops()
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !port_spray_tagged_drops)
		return;
	int n = 0;
	for (int i = 0; i < (int)w->mParticleCount; i++)
		if ((w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f && dropKind(i))
			n++;
	if (!n)
		return;
	n = n < 600 ? n : 600;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * 24));
	int done = 0;
	for (int i = 0; i < (int)w->mParticleCount && done < n; i++) {
		const int kind = (w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f ? dropKind(i) : 0;
		if (!kind)
			continue;
		done++;
		const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
		const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[i];
		const f32 r = w->mParticleSizeSOA[i] * 0.42f * statsOf(kind).drop;
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
		const f32 flick = 0.8f + 0.2f * sinf((f32)(s_frame + i * 7) * 0.9f);
		int mr, mg, mb, er, eg, eb; // its middle's colour and its tips'
		if (kind == kFire)
			mr = 255, mg = (int)(150 * flick), mb = 30, er = 220, eg = 60, eb = 10;
		else
			mr = 255, mg = 250, mb = (int)(150 * flick), er = 200, eg = 170, eb = 20;
		for (int t = 0; t < 2; t++)
			for (int k = 0; k < 4; k++) {
				const int j = (k + 1) & 3;
				corner(tip[t][0], tip[t][1], tip[t][2], er, eg, eb);
				corner(ring[k][0], ring[k][1], ring[k][2], mr, mg, mb);
				corner(ring[j][0], ring[j][1], ring[j][2], mr, mg, mb);
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

// a puddle: a round of goop with a wavy edge, flat on the ground, its middle glowing
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
	const f32 pulse = 0.85f + 0.15f * sinf(time * (p.kind == kFire ? 9.0f : 23.0f) + p.seed);
	int ir, ig, ib, orr, og, ob;
	if (p.kind == kFire)
		ir = (int)(255 * pulse), ig = (int)(130 * pulse), ib = 20, orr = 120, og = 25, ob = 10;
	else {
		const bool flash = ((s_frame / 3 + (int)p.seed) % 11) == 0;
		ir = flash ? 255 : (int)(235 * pulse), ig = flash ? 255 : (int)(215 * pulse), ib = flash ? 200 : 40, orr = 70, og = 60, ob = 15;
	}
	const int kAround = 24;
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(kAround + 2));
	corner(cx, cy, cz, ir, ig, ib);
	for (int k = 0; k <= kAround; k++) {
		const f32 a  = 6.2831853f * (f32)(k % kAround) / kAround;
		const f32 rr = r * (1.0f + 0.14f * sinf(3.0f * a + p.seed) + 0.08f * sinf(5.0f * a + p.seed * 1.7f) + 0.03f * sinf(time * 2.0f + a * 4.0f));
		const f32 ca = cosf(a) * rr, sa = sinf(a) * rr;
		corner(cx + t[0] * ca + b[0] * sa, cy + t[1] * ca + b[1] * sa, cz + t[2] * ca + b[2] * sa, orr, og, ob);
	}
	GXEnd();
}

// fire: flames rising off it, each two crossed tongues, flickering
void drawFlames(const Puddle& p)
{
	const f32 r = sizeNow(p);
	if (r < 6.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const int count = 3 + (int)(r / 30.0f);
	const f32 time  = (f32)s_frame / 60.0f;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(count * 12));
	for (int i = 0; i < count; i++) {
		const f32 a  = p.seed * 3.1f + (f32)i * 2.39996f; // spread over it (the golden angle)
		const f32 d  = r * 0.7f * sqrtf(((f32)i + 0.5f) / (f32)count);
		const f32 ox = cosf(a) * d, oz = sinf(a) * d;
		const f32 x = p.x + t[0] * ox + b[0] * oz, y = p.y + t[1] * ox + b[1] * oz, z = p.z + t[2] * ox + b[2] * oz;
		const f32 h = (22.0f + r * 0.18f) * (0.7f + 0.3f * sinf(time * 11.0f + (f32)i * 1.7f + p.seed));
		const f32 w = h * 0.28f;
		const f32 sway = 3.0f * sinf(time * 7.0f + (f32)i);
		for (int c = 0; c < 2; c++) {
			const f32 wx = c ? w : 0.0f, wz = c ? 0.0f : w;
			// a tongue: its foot wide and low, its tip up high (a diamond), red to yellow
			corner(x - wx, y + h * 0.3f, z - wz, 255, 90, 0);
			corner(x + wx, y + h * 0.3f, z + wz, 255, 90, 0);
			corner(x + sway, y + h, z, 255, 230, 90);
			corner(x - wx, y + h * 0.3f, z - wz, 255, 90, 0);
			corner(x + wx, y + h * 0.3f, z + wz, 255, 90, 0);
			corner(x, y, z, 230, 50, 0);
		}
	}
	GXEnd();
}

// electric: sparks crackling over it, zigzags made afresh every few frames
void drawSparks(const Puddle& p)
{
	const f32 r = sizeNow(p);
	if (r < 6.0f)
		return;
	f32 t[3], b[3];
	basis(p, t, b);
	const int count = 2 + (int)(r / 45.0f);
	const int kSteps = 6;
	u32 seed = (u32)(p.seed * 1000.0f) + (u32)(s_frame / 4) * 2654435761u;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(count * kSteps * 12));
	for (int i = 0; i < count; i++) {
		f32 pts[kSteps + 1][3];
		for (int s = 0; s <= kSteps; s++) {
			seed = seed * 1664525u + 1013904223u;
			const f32 ra = (f32)((seed >> 8) & 0xFFFF) / 65535.0f;
			seed = seed * 1664525u + 1013904223u;
			const f32 rb = (f32)((seed >> 8) & 0xFFFF) / 65535.0f;
			const f32 a = ra * 6.2831853f, d = r * 0.8f * sqrtf(rb);
			const f32 ox = cosf(a) * d, oz = sinf(a) * d, up = 4.0f + 14.0f * rb;
			pts[s][0] = p.x + t[0] * ox + b[0] * oz + p.nx * up;
			pts[s][1] = p.y + t[1] * ox + b[1] * oz + p.ny * up;
			pts[s][2] = p.z + t[2] * ox + b[2] * oz + p.nz * up;
		}
		for (int s = 0; s < kSteps; s++) {
			const f32* a = pts[s];
			const f32* c = pts[s + 1];
			// a thin ribbon, standing up and lying flat (seen from the side and from above)
			const f32 hw = 1.6f;
			for (int k = 0; k < 2; k++) {
				const f32 ux = k ? hw : 0.0f, uy = k ? 0.0f : hw, uz = k ? hw : 0.0f;
				corner(a[0] - ux, a[1] - uy, a[2] - uz, 255, 255, 170);
				corner(a[0] + ux, a[1] + uy, a[2] + uz, 255, 255, 230);
				corner(c[0] + ux, c[1] + uy, c[2] + uz, 255, 255, 230);
				corner(a[0] - ux, a[1] - uy, a[2] - uz, 255, 255, 170);
				corner(c[0] + ux, c[1] + uy, c[2] + uz, 255, 255, 230);
				corner(c[0] - ux, c[1] - uy, c[2] - uz, 255, 255, 170);
			}
		}
	}
	GXEnd();
}

void GoopPacket::draw()
{
	if (!s_haveView)
		return;
	s_frame++;
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(s_view, GX_PNMTX0);
	GXLoadNrmMtxImm(identity, GX_PNMTX0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetNumTexGens(0);
	GXSetNumIndStages(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor){0, 0, 0, 0});
	for (int i = 0; i < kPuddles; i++)
		if (s_puddles[i].on)
			drawPuddle(s_puddles[i], i);
	for (int i = 0; i < kPuddles; i++)
		if (s_puddles[i].on) {
			if (s_puddles[i].kind == kFire)
				drawFlames(s_puddles[i]);
			else
				drawSparks(s_puddles[i]);
		}
	drawDrops();
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
