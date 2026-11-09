// 3D Grass (port_grass.h): blades of grass standing on the stages' grass, as each place would have
// it. Delfino Plaza's lawns are the town's, and the Piantas mow them: short, even, in stripes the
// mower leaves. Pinna Park is kept the same. Ricco's verges and the Airstrip are tended now and
// then: a little longer. Sirena's grass has grown long round the hotel. Bianco Hills, Pianta Village, Noki Bay, Gelato's dunes,
// Corona Mountain and any stage it does not know (a mod's) grow wild: tall, uneven, in clumps,
// with bare patches.
//
// Where: the ground the stage's collision calls grass (material 0x10, as the footsteps' sound and
// Reflections by Surface take it), flat enough to stand on, open to the sky above it. The blades
// are laid out once on a grid across the world (each in its cell, its place, height and look from
// the cell alone, so they are the same every time) in 256-unit tiles round Mario, a few tiles a
// tick, and forgotten when he is far away.
//
// Their colour is the ground's under each (drawn first: its texture, its light, the time of day,
// gx_shader.cpp's grass program), and they grow only where that is green: bare earth stays bare.
//
// How they move: each sways with the wind (a breeze that rolls across the stage); bends away from
// Mario as he goes through it, and back over a second and a half along the way he went; and lies
// flat under FLUDD's puddles. Near Mario they are whole; farther out, fewer and shorter, gone at
// the edge.
#include "port_ext.h"
#include "port_grass.h"

// The renderer's (platform/gx/src/gx_render.cpp): the blades drawn on the GPU.
extern "C" unsigned GXPC_GrassBuffer(unsigned buf, const float* data, int blades);
extern "C" void GXPC_GrassBegin(const float view[3][4], const float* params, const float* press,
                                const float* strength, int count);
extern "C" void GXPC_GrassDraw(unsigned buf, int blades, int segments, int pressed);
extern "C" void GXPC_GrassEnd(void);
// The flattened grass (rgba: size by size cells, x0/z0 the first's corner, span across; null: none).
extern "C" void GXPC_GrassFlatMap(const unsigned char* rgba, int size, float x0, float z0, float span);

#include <Camera/Camera.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/MapData.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <System/Application.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ------------------------------------------------------------------ the kinds of grass
struct Style {
	const char* name;
	f32 spacing;     // between blades, units (Mario is about 160 tall)
	f32 low, high;   // the blades' height
	f32 width;       // at the base
	f32 wind;        // how far the breeze bends the tip, times the height
	f32 reach;       // how far from Mario the blades go
	int segments;    // 1 straight, 2 bending
	bool stripes;    // the mower's stripes
	bool clumps;     // uneven: taller and shorter patches, bare spots
	u8 base[3], tip[3];
};

const Style kCut    = {"cut short (the Piantas mow it)", 8.0f, 5.0f, 9.0f, 1.7f, 0.05f, 650.0f, 1, true, false,
                       {58, 146, 36}, {112, 210, 58}};
const Style kTended = {"tended now and then", 9.0f, 9.0f, 17.0f, 1.9f, 0.09f, 700.0f, 1, false, false,
                       {56, 140, 38}, {122, 210, 62}};
const Style kWild   = {"wild", 11.0f, 14.0f, 44.0f, 2.4f, 0.17f, 850.0f, 2, false, true,
                       {50, 132, 34}, {140, 214, 66}};

// The grass of a world (SMS_getShineStage's order), by location.
const Style* styleOfWorld(int world)
{
	switch (world) {
	case 1: return &kCut;     // Delfino Plaza: the town's lawns
	case 5: return &kCut;     // Pinna Park: a park
	case 6: return &kWild;    // Sirena Beach: grown long round the hotel
	case 0:                   // the Airstrip
	case 3: return &kTended;  // Ricco Harbor's verges
	default: return &kWild;   // Bianco Hills, Gelato Beach, Pianta Village, Noki Bay, Corona, mods' stages
	}
}

const char* const kWorldNames[] = {"the Airstrip", "Delfino Plaza", "Bianco Hills", "Ricco Harbor", "Gelato Beach",
                                   "Pinna Park", "Sirena Beach", "Pianta Village", "Noki Bay", "Corona Mountain"};

const Style* s_style;  // this stage's, or 0 (off)
int s_styleSetting = -1;
int s_styleArea    = -1;

const Style* wantedStyle(int* world)
{
	const int setting = port_grass();
	*world            = SMS_getShineStage(gpApplication.mCurrArea.getStage());
	switch (setting) {
	case 1: return styleOfWorld(*world);
	case 2: return &kCut;
	case 3: return &kWild;
	default: return 0;
	}
}

// ------------------------------------------------------------------ randomness from a place
u32 hash2(int x, int z, u32 salt)
{
	u32 h = (u32)x * 0x8DA6B343u ^ (u32)z * 0xD8163841u ^ salt * 0xCB1AB31Fu;
	h ^= h >> 13;
	h *= 0x5BD1E995u;
	h ^= h >> 15;
	return h;
}
f32 rand01(u32 h) { return (f32)(h & 0xFFFFFF) * (1.0f / 16777216.0f); }

// A smooth noise, 0 to 1, varying over `size` units: clumps of taller grass and bare patches.
f32 noise(f32 x, f32 z, f32 size, u32 salt)
{
	const f32 fx = x / size, fz = z / size;
	const int ix = (int)floorf(fx), iz = (int)floorf(fz);
	f32 tx = fx - ix, tz = fz - iz;
	tx = tx * tx * (3.0f - 2.0f * tx);
	tz = tz * tz * (3.0f - 2.0f * tz);
	const f32 a = rand01(hash2(ix, iz, salt)), b = rand01(hash2(ix + 1, iz, salt));
	const f32 c = rand01(hash2(ix, iz + 1, salt)), d = rand01(hash2(ix + 1, iz + 1, salt));
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

// ------------------------------------------------------------------ the ground
bool isGrass(const TBGCheckData* d)
{
	return d && d->unk6 == 0x10 && d->getNormal().y >= 0.6f && !d->isWaterSurface();
}

f32 heightAt(const TBGCheckData* d, f32 x, f32 z)
{
	const JGeometry::TVec3<f32>& n = d->getNormal();
	return -(d->getPlaneDistance() + n.x * x + n.z * z) / n.y;
}

bool over(const TBGCheckData* d, f32 x, f32 z)
{
	const JGeometry::TVec3<f32>& a = d->mPoint1;
	const JGeometry::TVec3<f32>& b = d->mPoint2;
	const JGeometry::TVec3<f32>& c = d->mPoint3;
	const f32 s1 = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
	const f32 s2 = (c.x - b.x) * (z - b.z) - (c.z - b.z) * (x - b.x);
	const f32 s3 = (a.x - c.x) * (z - c.z) - (a.z - c.z) * (x - c.x);
	return (s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0);
}

// The static map's grass triangles within radius of x, z (the game's 1024-unit grid), at most n.
int grassNear(f32 x, f32 z, f32 radius, const TBGCheckData** out, int n)
{
	const TMapCollisionData* map = gpMapCollisionData;
	if (!map)
		return 0;
	const int across = map->unk8, down = map->unkC;
	if (across <= 0 || down <= 0)
		return 0;
	int x0 = (int)((x - radius + map->mGridExtentX) * (1.0f / 1024));
	int x1 = (int)((x + radius + map->mGridExtentX) * (1.0f / 1024));
	int z0 = (int)((z - radius + map->mGridExtentY) * (1.0f / 1024));
	int z1 = (int)((z + radius + map->mGridExtentY) * (1.0f / 1024));
	x0     = x0 < 0 ? 0 : x0;
	z0     = z0 < 0 ? 0 : z0;
	x1     = x1 >= across ? across - 1 : x1;
	z1     = z1 >= down ? down - 1 : z1;
	int count = 0;
	for (int gz = z0; gz <= z1; gz++)
		for (int gx = x0; gx <= x1; gx++)
			for (const TBGCheckList* l = map->getGridRoot14(gx, gz).getGroundList(); l; l = l->getNext()) {
				const TBGCheckData* d = l->unk8;
				if (!isGrass(d))
					continue;
				bool seen = false;
				for (int i = 0; i < count && !seen; i++)
					seen = out[i] == d;
				if (!seen && count < n)
					out[count++] = d;
			}
	return count;
}

// ------------------------------------------------------------------ the tiles
// The tiles round Mario live in a 128 by 128 window that wraps round the world (a tile's place in
// it is its x and z modulo 128): every tile Grass Distance can reach fits in it at once, so a tile
// is found at its place, or replaced there. Each tile's blades are in a buffer of the renderer's
// (GXPC_GrassBuffer), drawn on the GPU.
const f32 kTile       = 256.0f;
const int kWindow     = 128;
const int kMostTiles  = kWindow * kWindow;
const int kMostBlades = 1400;  // in a tile: (256 / 8)^2 at the closest spacing, and more
// Nearer than this, a blade must be open to the sky (no bench, step or bridge over it); farther,
// that is not looked for (it costs the most, and is not seen from there).
const f32 kSkyCheck = 1600.0f;

struct Tile {
	int tx, tz;
	int count;
	bool used, distant;  // distant: made without the sky looked for
	f32 made;         // the share of its blades it was made with (far away, fewer: keepShare)
	unsigned buf;     // the renderer's buffer of its blades
	f32 minY, maxY;
};

Tile s_tiles[kMostTiles];
int s_bladeTotal;

// Grass Distance: how far the blades go, times the kind's own reach; every blade all the way.
// s_whole: within it, blades bend in two segments (farther, one).
f32 s_reach, s_whole;

// The share of the blades drawn at distance d (blade.keep below 256 times it): all of them.
f32 keepShare(f32) { return 1.0f; }

void dropTile(Tile& t)
{
	if (t.used)
		s_bladeTotal -= t.count;
	t.buf   = GXPC_GrassBuffer(t.buf, 0, 0);
	t.used  = false;
	t.count = 0;
}

void forgetTiles()
{
	for (int i = 0; i < kMostTiles; i++)
		dropTile(s_tiles[i]);
	s_bladeTotal = 0;
}

Tile& tileAt(int tx, int tz)
{
	const int wx = ((tx % kWindow) + kWindow) % kWindow, wz = ((tz % kWindow) + kWindow) % kWindow;
	return s_tiles[wz * kWindow + wx];
}

// How near Mario a tile comes (0 when he is on it).
f32 tileDistance(int tx, int tz, f32 mx, f32 mz)
{
	const f32 x0 = tx * kTile, z0 = tz * kTile;
	const f32 dx = mx < x0 ? x0 - mx : mx > x0 + kTile ? mx - x0 - kTile : 0.0f;
	const f32 dz = mz < z0 ? z0 - mz : mz > z0 + kTile ? mz - z0 - kTile : 0.0f;
	return sqrtf(dx * dx + dz * dz);
}

// A blade for the renderer: its root and height; its facing, its phase (0 to 1), its shade and
// keep (shade + 256 keep).
f32 s_blades[kMostBlades * 8];

// The tile's blades, as many as are drawn as near Mario as `share` says (the rest of a far tile's
// are not made at all, until he comes nearer). distance: how near Mario the tile comes.
void makeTile(Tile* t, int tx, int tz, f32 share, f32 distance)
{
	const Style& st = *s_style;
	dropTile(*t);
	t->tx = tx, t->tz = tz;
	t->used = true;
	t->made = share;
	t->distant = distance >= kSkyCheck;
	t->minY = 1e9f, t->maxY = -1e9f;
	const f32 x0 = tx * kTile, z0 = tz * kTile;
	const TBGCheckData* tris[96];
	const int n = grassNear(x0 + kTile * 0.5f, z0 + kTile * 0.5f, kTile * 0.75f, tris, 96);
	if (!n)
		return;
	int count = 0;
	// the world's grid of cells, so that a blade is the same whichever tile finds it
	const int c0x = (int)ceilf(x0 / st.spacing), c1x = (int)ceilf((x0 + kTile) / st.spacing);
	const int c0z = (int)ceilf(z0 / st.spacing), c1z = (int)ceilf((z0 + kTile) / st.spacing);
	const TBGCheckData* last = tris[0];
	for (int cz = c0z; cz < c1z && count < kMostBlades; cz++)
		for (int cx = c0x; cx < c1x && count < kMostBlades; cx++) {
			const u8 keep = (u8)(hash2(cx, cz, 0x3Cu) >> 24);
			if ((keep + 0.5f) * (1.0f / 256) >= share)
				continue;  // drawn only nearer than this tile comes
			const u32 h = hash2(cx, cz, 0x9E37u);
			const f32 x = (cx + rand01(h)) * st.spacing;
			const f32 z = (cz + rand01(h * 0x2545F491u + 7u)) * st.spacing;
			// wild grass: bare where the clump noise is low
			f32 clump = 1.0f;
			if (st.clumps) {
				clump = noise(x, z, 140.0f, 0x51u) * 0.7f + noise(x, z, 45.0f, 0x77u) * 0.3f;
				if (clump < 0.28f || rand01(h >> 3) > clump * 1.6f)
					continue;
			}
			const TBGCheckData* tri = over(last, x, z) ? last : 0;
			for (int i = 0; i < n && !tri; i++)
				if (over(tris[i], x, z))
					tri = tris[i];
			if (!tri)
				continue;
			last        = tri;
			const f32 y = heightAt(tri, x, z);
			// open to the sky: the ground found from a little above is this grass, not a bench or
			// a step on it
			if (!t->distant) {
				const TBGCheckData* top = 0;
				const f32 g             = gpMap->checkGroundIgnoreWaterSurface(x, y + 60.0f, z, &top);
				if (top != tri && fabsf(g - y) > 2.0f)
					continue;
			}
			f32 hgt = st.low + (st.high - st.low) * rand01(h >> 5);
			if (st.clumps)
				hgt *= 0.55f + 0.7f * clump;
			const f32 yaw = rand01(h >> 9) * 6.2831853f;
			f32* b = &s_blades[count++ * 8];
			b[0] = x, b[1] = y - 1.0f, b[2] = z, b[3] = hgt;
			b[4] = cosf(yaw), b[5] = sinf(yaw);
			b[6] = (f32)((h >> 7) & 0xFFFF) * (1.0f / 65536);
			b[7] = (f32)((h >> 17) & 0xFF) + 256.0f * keep;
			if (y - 1.0f < t->minY)
				t->minY = y - 1.0f;
			if (y + hgt > t->maxY)
				t->maxY = y + hgt;
		}
	t->count = count;
	t->buf   = GXPC_GrassBuffer(0, s_blades, count);
	s_bladeTotal += count;
}

// ------------------------------------------------------------------ what presses the grass down
// Mario where he is now, and his way over the last second and a half (each point fading), and the
// puddles FLUDD has made: each pushes the blades near it away, and down.
struct Press {
	f32 x, y, z, r, strength;
};
const int kTrail = 24;
const s32 kTrailMs = 1500;
struct TrailPoint {
	f32 x, y, z;
	s32 at;
};
TrailPoint s_trail[kTrail];
int s_trailCount, s_trailNext;

s32 nowMs() { return (s32)OSTicksToMilliseconds(OSGetTime()); }

const int kMostPress = 160;
Press s_press[kMostPress];
int s_pressCount;

// ------------------------------------------------------------------ the flattened grass
// Where FLUDD's spray came down into the grass, and where Mario stepped on it, the blades lie flat
// for a while (Grass Flattening: how wide and how long), then rise again: a map round Mario
// (256 by 256 cells of 12 units, about 3000 units across, following him), each cell how flat and
// which way, stamped as drops land and Mario walks, fading each tick, and given to the renderer
// (GXPC_GrassFlatMap), whose blades read it. No limit on how much is flat.
const f32 kSprayRadius[] = {45.0f, 70.0f, 95.0f, 140.0f, 220.0f};  // Low .. Insane
const s32 kSprayLife[]   = {3000, 5000, 7000, 11000, 18000};
const int kFlat          = 256;
const f32 kFlatCell      = 12.0f;
f32 s_flat[kFlat * kFlat];               // how flat: 1 just pressed, 0 standing
f32 s_flatDir[kFlat * kFlat][2];         // which way it lies (x, z)
u8 s_flatMap[kFlat * kFlat * 4];
int s_flatX0, s_flatZ0;                  // the map's first cell, in cells across the world
bool s_flatUsed, s_flatShown;
s32 s_flatAt;
f32 s_sprayRadius = 95.0f;
s32 s_sprayMs     = 7000;

void flatClear()
{
	memset(s_flat, 0, sizeof s_flat);
	s_flatUsed = false;
}

// The map's middle near Mario: moved by whole cells when he is a quarter of it away.
void flatFollow(f32 mx, f32 mz)
{
	const int cx = (int)floorf(mx / kFlatCell) - kFlat / 2, cz = (int)floorf(mz / kFlatCell) - kFlat / 2;
	const int dx = cx - s_flatX0, dz = cz - s_flatZ0;
	if (abs(dx) < kFlat / 4 && abs(dz) < kFlat / 4)
		return;
	if (abs(dx) >= kFlat || abs(dz) >= kFlat || !s_flatUsed) {
		flatClear();
	} else {
		static f32 s_tmp[kFlat * kFlat];
		static f32 s_tmpDir[kFlat * kFlat][2];
		memset(s_tmp, 0, sizeof s_tmp);
		for (int z = 0; z < kFlat; z++)
			for (int x = 0; x < kFlat; x++) {
				const int ox = x + dx, oz = z + dz;
				if (ox < 0 || oz < 0 || ox >= kFlat || oz >= kFlat)
					continue;
				s_tmp[z * kFlat + x]       = s_flat[oz * kFlat + ox];
				s_tmpDir[z * kFlat + x][0] = s_flatDir[oz * kFlat + ox][0];
				s_tmpDir[z * kFlat + x][1] = s_flatDir[oz * kFlat + ox][1];
			}
		memcpy(s_flat, s_tmp, sizeof s_flat);
		memcpy(s_flatDir, s_tmpDir, sizeof s_flatDir);
	}
	s_flatX0 = cx, s_flatZ0 = cz;
}

// A circle pressed flat round (x, z), each blade away from its middle.
void flatStamp(f32 x, f32 z, f32 r)
{
	const f32 fx = x / kFlatCell - (f32)s_flatX0, fz = z / kFlatCell - (f32)s_flatZ0;
	const f32 rc = r / kFlatCell;
	const int x0 = (int)floorf(fx - rc), x1 = (int)ceilf(fx + rc);
	const int z0 = (int)floorf(fz - rc), z1 = (int)ceilf(fz + rc);
	for (int cz = z0 < 0 ? 0 : z0; cz <= z1 && cz < kFlat; cz++)
		for (int cx = x0 < 0 ? 0 : x0; cx <= x1 && cx < kFlat; cx++) {
			const f32 ddx = (f32)cx + 0.5f - fx, ddz = (f32)cz + 0.5f - fz;
			const f32 d = sqrtf(ddx * ddx + ddz * ddz);
			if (d >= rc)
				continue;
			const f32 a = d < rc * 0.7f ? 1.0f : (rc - d) / (rc * 0.3f);
			f32& f = s_flat[cz * kFlat + cx];
			if (a <= f)
				continue;
			f = a;
			s_flatDir[cz * kFlat + cx][0] = d > 0.01f ? ddx / d : 0.0f;
			s_flatDir[cz * kFlat + cx][1] = d > 0.01f ? ddz / d : 0.0f;
			s_flatUsed = true;
		}
}

// Each tick: fading (flat for three quarters of its life, then rising over the last quarter),
// and to the renderer while any of it is flat.
void flatTick()
{
	const s32 now = nowMs();
	const s32 dt  = s_flatAt ? now - s_flatAt : 0;
	s_flatAt      = now;
	if (!s_flatUsed) {
		if (s_flatShown) {
			GXPC_GrassFlatMap(0, 0, 0.0f, 0.0f, 0.0f);
			s_flatShown = false;
		}
		return;
	}
	const f32 fade = dt > 0 ? (f32)dt / (f32)s_sprayMs : 0.0f;
	bool any       = false;
	for (int i = 0; i < kFlat * kFlat; i++) {
		f32& f = s_flat[i];
		if (f <= 0.0f) {
			s_flatMap[i * 4] = 0;
			continue;
		}
		f -= fade;
		if (f < 0.0f)
			f = 0.0f;
		any            = any || f > 0.0f;
		const f32 show = f * 4.0f > 1.0f ? 1.0f : f * 4.0f;
		s_flatMap[i * 4]     = (u8)(show * 255.0f + 0.5f);
		s_flatMap[i * 4 + 1] = (u8)((s_flatDir[i][0] * 0.5f + 0.5f) * 255.0f + 0.5f);
		s_flatMap[i * 4 + 2] = (u8)((s_flatDir[i][1] * 0.5f + 0.5f) * 255.0f + 0.5f);
		s_flatMap[i * 4 + 3] = 255;
	}
	s_flatUsed = any;
	GXPC_GrassFlatMap(s_flatMap, kFlat, (f32)s_flatX0 * kFlatCell, (f32)s_flatZ0 * kFlatCell, kFlat * kFlatCell);
	s_flatShown = true;
}

// FLUDD's spray: each drop coming down into the grass (from 250 units above it), a circle flat
void noteSpray()
{
	TModelWaterManager* water = gpModelWaterManager;
	const int count           = water ? water->getParticleCount() : 0;
	const int level           = port_grass_flatten();
	s_sprayRadius             = kSprayRadius[level < 0 || level > 4 ? 2 : level];
	s_sprayMs                 = kSprayLife[level < 0 || level > 4 ? 2 : level];
	for (int i = 0; i < count; i++) {
		const JGeometry::TVec3<f32>& p = water->mParticlePositionSOA[i];
		const int tx = (int)floorf(p.x / kTile), tz = (int)floorf(p.z / kTile);
		const Tile& t = tileAt(tx, tz);
		if (!t.used || t.tx != tx || t.tz != tz || !t.count || p.y < t.minY - 30.0f || p.y > t.maxY + 250.0f)
			continue;  // not down in grass, nor coming down on it
		flatStamp(p.x, p.z, s_sprayRadius * 0.55f);
	}
}

// Mario on the grass (Mario Grass Flattening): 0 where he lands from a jump, a fall or a dive,
// and where he slides (his belly after a dive or on wet ground, his seat down a slope, crouched) or
// skids (braking, turning round); 1 also each step (every 30 units he walks); 2 never. The circles,
// times Grass Flattening's width: a belly slide 0.75 every 12 units (the widest, so it shows), a
// landing 0.55, another slide 0.45 every 20, a skid 0.4 every 25, a step 0.4 every 30.
const u32 kStatusTypeMask = 0x1C0, kStatusTypeAir = 0x080;  // MarioStatus.hpp: in the air
bool bellySlide(u32 s) { return s == 0x800456 || s == 0x84045D; }  // a dive's belly slide; on oil/wet
bool slide(u32 s)
{
	return s == 0x50 || s == 0x840452 || s == 0x840453 || s == 0x4808459 || s == 0x44C || s == 0x4045E;
}  // down a slope (forwards, backwards), crouched, at speed, down a wet slope
bool skid(u32 s) { return s == 0x4000445 || s == 0x443; }  // braking; turning round
u32 s_marioWas;  // his status last tick
f32 s_slideX = 1e9f, s_slideZ = 1e9f, s_stepX = 1e9f, s_stepZ = 1e9f;

// a circle every `every` units along his way
void along(f32 mx, f32 mz, f32* lastX, f32* lastZ, f32 every, f32 width)
{
	const f32 dx = mx - *lastX, dz = mz - *lastZ;
	if (dx * dx + dz * dz < every * every)
		return;
	// a slide covers the whole way since the last circle (one each half circle's width)
	const f32 len = sqrtf(dx * dx + dz * dz), r = s_sprayRadius * width;
	const int n = len < 1e6f ? (int)(len / (r * 0.5f)) : 0;
	for (int k = 1; k <= n && k < 16; k++)
		flatStamp(*lastX + dx * (f32)k / (f32)(n + 1), *lastZ + dz * (f32)k / (f32)(n + 1), r);
	flatStamp(mx, mz, r);
	*lastX = mx, *lastZ = mz;
}

void noteMario(f32 mx, f32 my, f32 mz)
{
	const int mode = port_grass_mario();
	const u32 now  = SMS_GetMarioStatus();
	const u32 was  = s_marioWas;
	s_marioWas     = now;
	if (mode == 2)
		return;
	const int tx = (int)floorf(mx / kTile), tz = (int)floorf(mz / kTile);
	const Tile& t = tileAt(tx, tz);
	if (!t.used || t.tx != tx || t.tz != tz || !t.count || my < t.minY - 30.0f || my > t.maxY + 20.0f) {
		s_slideX = s_stepX = 1e9f;  // off the grass: the next slide or step starts afresh
		return;  // not on the ground among blades
	}
	const bool air = (now & kStatusTypeMask) == kStatusTypeAir;
	if (!air && (was & kStatusTypeMask) == kStatusTypeAir)  // landed: from a jump, a fall, a dive
		flatStamp(mx, mz, s_sprayRadius * 0.55f);
	if (bellySlide(now))
		along(mx, mz, &s_slideX, &s_slideZ, 12.0f, 0.75f);
	else if (slide(now))
		along(mx, mz, &s_slideX, &s_slideZ, 20.0f, 0.45f);
	else if (skid(now))
		along(mx, mz, &s_slideX, &s_slideZ, 25.0f, 0.4f);
	else
		s_slideX = 1e9f;  // not sliding: the next slide starts where it starts
	if (mode == 1 && !air)  // every step
		along(mx, mz, &s_stepX, &s_stepZ, 30.0f, 0.4f);
}

// The renderer takes 64: Mario, his way, the puddles (the flattened grass is the map's).
void gatherPress(f32 mx, f32 my, f32 mz)
{
	s_pressCount = 0;
	const s32 now = nowMs();
	Press& m = s_press[s_pressCount++];
	m.x = mx, m.y = my, m.z = mz, m.r = 36.0f, m.strength = 1.0f;
	for (int i = 0; i < s_trailCount && s_pressCount < kMostPress; i++) {
		const TrailPoint& p = s_trail[i];
		const s32 age       = now - p.at;
		if (age < 0 || age >= kTrailMs)
			continue;
		Press& q = s_press[s_pressCount++];
		q.x = p.x, q.y = p.y, q.z = p.z, q.r = 32.0f;
		const f32 left = 1.0f - (f32)age / kTrailMs;
		q.strength     = left * left * 0.9f;
	}
	TModelWaterManager* water = gpModelWaterManager;
	const int count           = water ? water->getParticleCount() : 0;
	for (int i = 0; i < count && s_pressCount < kMostPress; i++) {
		if ((water->mParticleFlagSOA[i] & 0xf) != 2)  // landed: a puddle
			continue;
		const JGeometry::TVec3<f32>& p = water->mParticlePositionSOA[i];
		const f32 dx = p.x - mx, dz = p.z - mz;
		if (dx * dx + dz * dz > 1000.0f * 1000.0f)
			continue;
		Press& q = s_press[s_pressCount++];
		q.x = p.x, q.y = p.y, q.z = p.z, q.r = 28.0f, q.strength = 0.75f;
	}
}

// ------------------------------------------------------------------ each tick
void afterMovement()
{
	int world          = -1;
	const Style* want  = gpMarioPos && gpMap ? wantedStyle(&world) : 0;
	const int area     = gpApplication.mCurrArea.getStage();
	if (want != s_style || port_grass() != s_styleSetting || area != s_styleArea) {
		forgetTiles();
		s_style        = want;
		s_styleSetting = port_grass();
		s_styleArea    = area;
		s_trailCount = s_trailNext = 0;
		flatClear();
		if (want)
			fprintf(stderr, "[grass] %s: %s grass\n",
			        world >= 0 && world < 10 ? kWorldNames[world] : "this stage", want->name);
		else
			fprintf(stderr, "[grass] off\n");
	}
	if (!s_style || !gpMarioPos)
		return;
	const f32 mx = gpMarioPos->x, my = gpMarioPos->y, mz = gpMarioPos->z;
	flatFollow(mx, mz);
	noteSpray();
	noteMario(mx, my, mz);
	flatTick();
	// Mario's way, a point each time he has moved 14 units
	const TrailPoint* lastPoint = s_trailCount ? &s_trail[(s_trailNext + kTrail - 1) % kTrail] : 0;
	const f32 ldx = lastPoint ? mx - lastPoint->x : 1e9f, ldz = lastPoint ? mz - lastPoint->z : 0.0f;
	if (ldx * ldx + ldz * ldz > 14.0f * 14.0f) {
		TrailPoint& p = s_trail[s_trailNext];
		p.x = mx, p.y = my, p.z = mz, p.at = nowMs();
		s_trailNext = (s_trailNext + 1) % kTrail;
		if (s_trailCount < kTrail)
			s_trailCount++;
	}
	// the tiles round Mario, nearest first: those missing, and those he has come nearer to than
	// they were made for; for at most 1.5 ms a tick
	s_reach = s_style->reach * port_grass_distance();
	s_whole = s_style->reach * 0.5f;
	const f32 reach = s_reach + kTile;
	const int tx0 = (int)floorf((mx - reach) / kTile), tx1 = (int)floorf((mx + reach) / kTile);
	const int tz0 = (int)floorf((mz - reach) / kTile), tz1 = (int)floorf((mz + reach) / kTile);
	const OSTime start = OSGetTime();
	for (int made = 0; made < 64 && OSTicksToMicroseconds(OSGetTime() - start) < 1500;) {
		int bx = 0, bz = 0;
		f32 best = 1e30f, bestShare = 0.0f;
		for (int tz = tz0; tz <= tz1; tz++)
			for (int tx = tx0; tx <= tx1; tx++) {
				const f32 d = tileDistance(tx, tz, mx, mz);
				if (d >= best || d > s_reach)
					continue;
				const Tile& t     = tileAt(tx, tz);
				const f32 share   = keepShare(d > kTile ? d - kTile : 0.0f);  // a tile's margin
				const bool needed = !t.used || t.tx != tx || t.tz != tz || share > t.made * 1.3f
				                    || (t.distant && d < kSkyCheck - kTile);
				if (needed)
					best = d, bx = tx, bz = tz, bestShare = share;
			}
		if (best >= 1e30f)
			break;
		makeTile(&tileAt(bx, bz), bx, bz, bestShare, best);
		made++;
	}
}

void stageSetup()
{
	forgetTiles();
	s_styleArea  = -1;
	s_trailCount = s_trailNext = 0;
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "grass";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		port_ext_register(&s_extension);
	}
} s_register;

// ------------------------------------------------------------------ drawing
} // namespace

namespace {
// The blades near Mario, seen from the main camera.
void drawGrass(float (*view)[4])
{
	if (!s_style || !gpMarioPos || !view || !s_bladeTotal || s_reach <= 0.0f)
		return;
	const f32 reach = s_reach;
	const Style& st = *s_style;
	const f32 mx = gpMarioPos->x, my = gpMarioPos->y, mz = gpMarioPos->z;
	gatherPress(mx, my, mz);

	// the GX state the blades are drawn with: depth tested and written, both sides, solid
	GXSetCullMode(GX_CULL_NONE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetZCompLoc(GX_TRUE);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetColorUpdate(GX_TRUE);
	GXSetAlphaUpdate(GX_FALSE);
	GXSetDstAlpha(GX_FALSE, 0);

	const int n = s_pressCount > 64 ? 64 : s_pressCount;
	f32 press[64 * 4], strength[64];
	for (int i = 0; i < n; i++) {
		press[i * 4] = s_press[i].x, press[i * 4 + 1] = s_press[i].y, press[i * 4 + 2] = s_press[i].z;
		press[i * 4 + 3] = s_press[i].r;
		strength[i]      = s_press[i].strength;
	}
	const f32 params[24] = {
		(f32)nowMs() * 0.001f, st.wind, st.width, port_grass_height(s_style == &kWild),
		(f32)st.base[0], (f32)st.base[1], (f32)st.base[2], s_whole,
		(f32)st.tip[0], (f32)st.tip[1], (f32)st.tip[2], reach,
		mx, my, mz, 0.0f,
		0.8f, 0.6f, st.stripes ? 1.0f : 0.0f, st.clumps ? 1.0f : 0.0f,  // the breeze's way
		0.0f, 0.0f, 0.0f, 0.0f,  // the renderer's
	};
	GXPC_GrassBegin(view, params, press, strength, n);
	for (int i = 0; i < kMostTiles; i++) {
		const Tile& tile = s_tiles[i];
		if (!tile.used || !tile.count || !tile.buf)
			continue;
		const f32 d = tileDistance(tile.tx, tile.tz, mx, mz);
		if (d > reach)
			continue;
		// behind the camera (view space z > 0 is behind it), with a tile's margin
		const f32 cx = (tile.tx + 0.5f) * kTile, cz = (tile.tz + 0.5f) * kTile;
		const f32 cy = (tile.minY + tile.maxY) * 0.5f;
		const f32 vz = view[2][0] * cx + view[2][1] * cy + view[2][2] * cz + view[2][3];
		if (vz > kTile)
			continue;
		// whether anything that presses reaches it
		bool pressed = false;
		for (int k = 0; k < n && !pressed; k++) {
			const Press& p = s_press[k];
			pressed = p.x + p.r >= tile.tx * kTile && p.x - p.r <= (tile.tx + 1) * kTile
			          && p.z + p.r >= tile.tz * kTile && p.z - p.r <= (tile.tz + 1) * kTile
			          && p.y >= tile.minY - 60.0f && p.y <= tile.maxY + 60.0f;
		}
		// curved two-segment blades near Mario, straight ones farther
		const int segments = st.segments > 1 && d < s_whole ? 2 : 1;
		GXPC_GrassDraw(tile.buf, tile.count, segments, pressed ? 1 : 0);
	}
	GXPC_GrassEnd();
}
} // namespace

void port_grass_draw(float (*view)[4]) { drawGrass(view); }
