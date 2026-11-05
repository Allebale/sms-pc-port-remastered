// Walls for the underground passages (the manholes' tunnels under Delfino Plaza, and any ground the
// game marks as an underground pathway, BG_TYPE_UNDERGROUND_PATHWAY / _SUPER_JUMP) in first person.
// The game draws only their floor: its camera looks straight down at it there, so walls and a ceiling
// were never made, and a first-person camera down there sees the stage above from beneath. While the
// first-person camera is on (a first-person mod's set_view) and Mario stands on such ground, the
// tunnel's collision walls near him are drawn as walls up to a ceiling, and its floor again as that
// ceiling, in a blue metal panel look like Hotel Delfino's maze (made here, no file of the game's).
// Without a first-person mod, or anywhere else, nothing is drawn.
#include "port_ext.h"

#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapCollisionData.hpp>
#include <Player/Mario.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" void* port_low_alloc(unsigned long size);
bool PortFirstPersonView(); // modapi_held_fludd.cpp: a first-person mod's camera is on

namespace {

const f32 kReach      = 4096.0f; // the walls this far round Mario (the collision grid's 1024 cells)
const f32 kHeight     = 420.0f;  // the tunnel's height, unless a roof or the ground above is lower
const f32 kTileWidth  = 300.0f;  // a wall panel's width
const int kMostWalls  = 600;
const int kMostRoof   = 600;  // the floor's collision triangles taken for the ceiling
const int kMostPieces = 600;  // the ceiling's pieces
const int kTex        = 64;

struct Quad {
	f32 x0, z0, x1, z1; // its ends across the ground
	f32 shade;
};
Quad s_walls[kMostWalls];
int s_wallCount;
struct Tri {
	f32 p[3][3];
};
Tri s_roof[kMostPieces];
int s_roofCount, s_floorCount, s_openCount;
int s_keyX = -99999, s_keyZ = -99999; // the grid cell and heights the walls and ceiling were made for
f32 s_keyBottom, s_keyTop;
f32 s_bottom, s_top; // this frame's floor and ceiling heights
bool s_on;
const TBGCheckData* s_seen[kMostWalls + kMostRoof]; // each collision triangle once
int s_seenCount;

u8* s_tex; // the panel texture, GX RGBA8 tiles (below 4 GiB)

// The panel look: dark navy, a light rail along the top and a handrail near the bottom, a post at
// each panel's edge, rivets, a little grain.
void texel(int x, int y, u8 out[4])
{
	const f32 u = (x + 0.5f) / kTex, v = (y + 0.5f) / kTex; // v 0 at the top
	f32 r = 24, g = 36, b = 112;
	const f32 grain = (f32)(((x * 73856093) ^ (y * 19349663)) & 7) - 3.5f;
	r += grain * 0.3f, g += grain * 0.4f, b += grain * 0.6f;
	b += (0.5f - fabsf(v - 0.5f)) * 22.0f; // lighter towards the middle
	if (v < 0.07f) // the top rail
		r = 48, g = 70, b = 176;
	else if (v < 0.10f)
		r = 10, g = 14, b = 52;
	if (v > 0.80f && v < 0.86f) // the handrail
		r = 66, g = 96, b = 206;
	else if (v >= 0.86f && v < 0.88f)
		r = 12, g = 16, b = 58;
	else if (v >= 0.88f) // the skirting below it
		r *= 0.8f, g *= 0.8f, b *= 0.85f;
	if (u < 0.05f || u > 0.97f) { // the post between panels
		r = 40, g = 60, b = 160;
		if (u > 0.035f && u < 0.05f)
			r = 8, g = 12, b = 46;
	}
	const f32 du = u - 0.12f, dv1 = v - 0.17f, dv2 = v - 0.74f; // rivets
	if (du * du + dv1 * dv1 < 0.0006f || du * du + dv2 * dv2 < 0.0006f)
		r = 80, g = 110, b = 210;
	out[0] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
	out[1] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
	out[2] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
	out[3] = 255;
}

bool makeTexture()
{
	if (s_tex)
		return true;
	s_tex = (u8*)port_low_alloc(kTex * kTex * 4);
	if (!s_tex)
		return false;
	size_t o = 0;
	for (int by = 0; by < kTex; by += 4)
		for (int bx = 0; bx < kTex; bx += 4) {
			u8 px[16][4];
			for (int i = 0; i < 16; i++)
				texel(bx + (i & 3), by + (i >> 2), px[i]);
			for (int i = 0; i < 16; i++) // a GX RGBA8 block: AR, then GB
				s_tex[o++] = px[i][3], s_tex[o++] = px[i][0];
			for (int i = 0; i < 16; i++)
				s_tex[o++] = px[i][1], s_tex[o++] = px[i][2];
		}
	DCFlushRange(s_tex, kTex * kTex * 4);
	return true;
}

bool seen(const TBGCheckData* d)
{
	for (int i = 0; i < s_seenCount; i++)
		if (s_seen[i] == d)
			return true;
	if (s_seenCount < kMostWalls + kMostRoof)
		s_seen[s_seenCount++] = d;
	return false;
}

void addWall(const TBGCheckData* d)
{
	if (s_wallCount >= kMostWalls || d->mMaxY < s_bottom - 40.0f || d->mMinY > s_top + 40.0f || seen(d))
		return;
	// its two corners furthest apart across the ground: the wall's ends
	const JGeometry::TVec3<f32>* p[3] = {&d->mPoint1, &d->mPoint2, &d->mPoint3};
	int a = 0, b = 1;
	f32 most = -1.0f;
	for (int i = 0; i < 3; i++)
		for (int j = i + 1; j < 3; j++) {
			const f32 dx = p[i]->x - p[j]->x, dz = p[i]->z - p[j]->z, l = dx * dx + dz * dz;
			if (l > most)
				most = l, a = i, b = j;
		}
	if (most < 4.0f)
		return;
	Quad& q = s_walls[s_wallCount++];
	q.x0 = p[a]->x, q.z0 = p[a]->z, q.x1 = p[b]->x, q.z1 = p[b]->z;
	// lit from one side, so the corners show
	const JGeometry::TVec3<f32>& n = d->getNormal();
	q.shade = 0.62f + 0.38f * fabsf(n.x * 0.55f + n.z * 0.83f);
}

void addRoof(const TBGCheckData* d)
{
	if (s_roofCount >= kMostPieces || !d->isUnderground() || fabsf(d->mMaxY - s_bottom) > 150.0f || seen(d))
		return;
	s_floorCount++;
	if (d->mBGType == BG_TYPE_UNDERGROUND_SUPER_JUMP) { // the floor under a manhole's shaft: open above
		s_openCount++;
		return;
	}
	Tri& t = s_roof[s_roofCount++];
	const JGeometry::TVec3<f32>* p[3] = {&d->mPoint1, &d->mPoint2, &d->mPoint3};
	for (int i = 0; i < 3; i++)
		t.p[i][0] = p[i]->x, t.p[i][1] = s_top, t.p[i][2] = p[i]->z;
}

void walk(const TBGCheckList* l, bool walls)
{
	for (int n = 0; l && n < 4000; l = l->getNext(), n++)
		if (const TBGCheckData* d = l->unk8)
			walls ? addWall(d) : addRoof(d);
}

// This frame's walls and ceiling round Mario, if he is down there in first person.
void gather()
{
	s_on = false;
	TMario* m = gpMarioOriginal;
	const TMapCollisionData* c = gpMapCollisionData;
	if (!m || !c || !gpMap || !PortFirstPersonView())
		return;
	const TBGCheckData* g = m->getGroundPlane();
	static unsigned s_lastType = 0;
	if (g && g->mBGType != s_lastType && m->mPosition.y < s_keyBottom + 600.0f && s_keyX != -99999) {
		s_lastType = g->mBGType; // what he stands on, as it changes down there
		fprintf(stderr, "[tunnel] ground 0x%x at (%.0f, %.0f, %.0f)\n", s_lastType, m->mPosition.x, m->mPosition.y, m->mPosition.z);
	}
	if (!g || !g->isUnderground())
		return;
	const JGeometry::TVec3<f32>& at = m->mPosition;
	const TBGCheckData* under = 0;
	s_bottom = gpMap->checkGround(at.x, at.y + 80.0f, at.z, &under);
	if (s_bottom < -30000.0f)
		return;
	s_top = s_bottom + kHeight;
	const TBGCheckData* roof = 0;
	const f32 r = gpMap->checkRoof(at.x, s_bottom + 40.0f, at.z, &roof);
	if (roof && r > s_bottom + 160.0f && r < s_top)
		s_top = r;
	// the manholes' tops up on the surface are marked underground too: there the ground found from
	// high above is his own (nothing over him); down in a tunnel it is the stage's above, or none
	const TBGCheckData* above = 0;
	const f32 up = gpMap->checkGround(at.x, s_bottom + 3000.0f, at.z, &above);
	if (above && up < s_bottom + 60.0f)
		return;
	if (above && up - 6.0f < s_top)
		s_top = up - 6.0f;
	const int cells = (int)(kReach / 1024.0f);
	const int gx = (int)((at.x + c->mGridExtentX) / 1024.0f), gz = (int)((at.z + c->mGridExtentY) / 1024.0f);
	if (gx == s_keyX && gz == s_keyZ && fabsf(s_bottom - s_keyBottom) < 20.0f && fabsf(s_top - s_keyTop) < 20.0f) {
		s_bottom = s_keyBottom, s_top = s_keyTop; // as made (they change only with the cell he is in)
		s_on     = s_wallCount > 0 || s_roofCount > 0;
		return;
	}
	s_keyX = gx, s_keyZ = gz, s_keyBottom = s_bottom, s_keyTop = s_top;
	s_wallCount = s_roofCount = s_seenCount = s_floorCount = s_openCount = 0;
	for (int z = gz - cells; z <= gz + cells; z++)
		for (int x = gx - cells; x <= gx + cells; x++) {
			if (x < 0 || z < 0 || x >= c->unk8 || z >= c->unkC)
				continue;
			const TBGCheckListRoot& root = c->getGridRoot14(x, z);
			walk(root.getWallList(), true);
			walk(root.getGroundList(), false);
		}
	s_on = s_wallCount > 0 || s_roofCount > 0;
	static int s_logged = -1;
	if (s_on && s_logged != s_wallCount) {
		s_logged = s_wallCount;
		fprintf(stderr, "[tunnel] first person underground (ground 0x%x): %d walls, %d ceiling pieces (%d left open under a shaft), floor %.0f, ceiling %.0f\n",
		        (unsigned)g->mBGType, s_wallCount, s_roofCount, s_openCount, s_bottom, s_top);
	}
}

void vertex(f32 x, f32 y, f32 z, f32 s, f32 t, f32 shade)
{
	const u8 k = (u8)(255.0f * (shade > 1.0f ? 1.0f : shade));
	GXPosition3f32(x, y, z);
	GXColor4u8(k, k, k, 0); // alpha 0 into the picture: the stage's specular pass (its shine, the surface
	                        // seen mirrored) takes the picture's alpha for where things shine
	GXTexCoord2f32(s, t);
}

void drawWalls();

class TunnelPacket : public J3DPacket {
public:
	virtual void draw();
};
TunnelPacket s_packet;

void TunnelPacket::draw()
{
	if (s_on)
		drawWalls();
}

void drawWalls()
{
	if (!makeTexture())
		return;
	GXTexObj tex;
	GXInitTexObj(&tex, s_tex, kTex, kTex, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
	GXInitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&tex, GX_TEXMAP0);
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(j3dSys.getViewMtx(), GX_PNMTX0);
	GXLoadNrmMtxImm(identity, GX_PNMTX0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetNumIndStages(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor){0, 0, 0, 0});

	const f32 y0 = s_bottom - 30.0f, y1 = s_top; // a little below the floor, so no gap shows at its edge
	for (int i = 0; i < s_wallCount; i++) {
		const Quad& q = s_walls[i];
		const f32 dx = q.x1 - q.x0, dz = q.z1 - q.z0, len = sqrtf(dx * dx + dz * dz);
		// along the wall from a place fixed in the world, so panels line up from wall to wall
		const f32 s0 = (q.x0 * 0.7f + q.z0 * 0.7f) / kTileWidth, s1 = s0 + len / kTileWidth;
		GXBegin(GX_QUADS, GX_VTXFMT0, 4);
		vertex(q.x0, y1, q.z0, s0, 0.0f, q.shade);
		vertex(q.x1, y1, q.z1, s1, 0.0f, q.shade);
		vertex(q.x1, y0, q.z1, s1, 1.0f, q.shade * 0.8f);
		vertex(q.x0, y0, q.z0, s0, 1.0f, q.shade * 0.8f);
		GXEnd();
	}
	if (s_roofCount > 0) {
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(s_roofCount * 3));
		for (int i = 0; i < s_roofCount; i++)
			for (int k = 0; k < 3; k++) {
				const f32* p = s_roof[i].p[k];
				vertex(p[0], p[1], p[2], p[0] / (kTileWidth * 2.0f), p[2] / (kTileWidth * 2.0f), 0.45f);
			}
		GXEnd();
	}
}

} // namespace

// Mario down in a tunnel in first person, its walls drawn (MarDirectorDirect.cpp: the see-through
// pass the game draws there for its camera above, the stage over him faintly, is left out then).
bool PortTunnelWallsOn() { return s_on; }

// As the stage's map is entered for drawing (TMap::perform's opaque CUE_ENTRY): the tunnel's walls
// with it, when Mario is down there in first person.
void PortTunnelWallsEntry()
{
	gather();
	if (s_on && j3dSys.getDrawBuffer(0))
		j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
}
