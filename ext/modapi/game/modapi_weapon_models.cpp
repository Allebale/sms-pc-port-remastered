// The rifle's, submachine gun's and bazooka's own models, held in FLUDD's place while the First Person mod's rifle or
// bazooka is chosen (modapi_held_fludd.cpp places them; the spray weapons are
// modapi_spray_weapons.cpp). A brown rifle, a blue submachine gun and a green pipe bazooka with FLUDD's tank, straps,
// nozzle and pump handle, made in Blender from simple shapes, low-poly with their light baked into
// their colours, as the games of the GameCube's time looked (rifle_model.inc, smg_model.inc, bazooka_model.inc:
// each triangle's corners and colours). In third person (SMS_SPRAY_THIRD_PERSON_MODEL) all of FLUDD
// on his back is drawn as a pack of the weapon's own colours with the weapon on it over his right
// shoulder (rifle_pack.inc, bazooka_pack.inc, smg_pack.inc: in game units, from FLUDD's
// nozzle_center joint), the weapon on it a model of its own (*_pack_gun.inc) that kicks as it fires.
//
// Drawn straight with GX, as a packet of the scene's opaque draw buffer entered as FLUDD would be
// (TWaterGun::perform's CUE_ENTRY, patch zzzzzzzz-held-fludd-weapon-model, which then leaves FLUDD
// out), so it is drawn with the scene, the scene's depth in front of and behind it. Without the
// mod holding one, nothing is entered and FLUDD is drawn as ever.
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <math.h>

bool PortBazookaBalloon(float* at, float* way, float* scale); // modapi_spray_weapons.cpp
int PortBazookaBalloonPieces(float* out, int most);
int PortWeaponHearts(float* out, int most); // modapi_spray_weapons.cpp

namespace {

#include "rifle_model.inc"
#include "bazooka_model.inc"
#include "smg_model.inc"
#include "rifle_pack.inc"
#include "bazooka_pack.inc"
#include "smg_pack.inc"
#include "rifle_pack_gun.inc"
#include "bazooka_pack_gun.inc"
#include "smg_pack_gun.inc"

struct Model {
	const float* pos;
	const unsigned char* col;
	int corners;
};
const Model kModels[9] = {{kRiflePos, kRifleCol, kRifleCorners}, {kBazookaPos, kBazookaCol, kBazookaCorners}, {kSmgPos, kSmgCol, kSmgCorners},
                          {kRiflePackPos, kRiflePackCol, kRiflePackCorners}, {kBazookaPackPos, kBazookaPackCol, kBazookaPackCorners},
                          {kSmgPackPos, kSmgPackCol, kSmgPackCorners},
                          {kRiflePackGunPos, kRiflePackGunCol, kRiflePackGunCorners},
                          {kBazookaPackGunPos, kBazookaPackGunCol, kBazookaPackGunCorners},
                          {kSmgPackGunPos, kSmgPackGunCol, kSmgPackGunCorners}};

Mtx s_viewModel;  // the view's matrix and the model's place in the world, this frame
int s_shown = -1; // the model placed this frame (0 the rifle, 1 the bazooka, 2 the submachine gun; 3 to 5 their packs), or none
Mtx s_viewModel2; // and a second with it (6 to 8: the packs' weapons), or none
int s_shown2 = -1;

class WeaponPacket : public J3DPacket {
public:
	virtual void draw();
};
WeaponPacket s_packet;

void drawModel(const Model& m, MtxPtr viewModel);

void WeaponPacket::draw()
{
	if (s_shown < 0 || s_shown > 8)
		return;
	drawModel(kModels[s_shown], s_viewModel);
	if (s_shown2 >= 0 && s_shown2 <= 8)
		drawModel(kModels[s_shown2], s_viewModel2);
}

void drawModel(const Model& m, MtxPtr viewModel)
{
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(viewModel, GX_PNMTX0);
	GXLoadNrmMtxImm(identity, GX_PNMTX0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetNumChans(1); // its colours as they are (lit in them)
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
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)m.corners);
	for (int i = 0; i < m.corners; i++) {
		GXPosition3f32(m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]);
		GXColor4u8(m.col[i * 4], m.col[i * 4 + 1], m.col[i * 4 + 2], m.col[i * 4 + 3]);
	}
	GXEnd();
}

// The bazooka's green water balloon (modapi_spray_weapons.cpp flies it: PortBazookaBalloon), drawn
// as a packet of the scene's opaque draw buffer too: a small round balloon (the bazooka's muzzle's size) a little longer along the way
// it flies, wobbling, with a knot at its back, its light baked into its corners' colours (a light
// from above), a shine on top. Built each frame from rings of corners, in the world, with the
// view's matrix of the frame (PortWeaponModelView).
Mtx s_view;
bool s_haveView;
class BalloonPacket : public J3DPacket {
public:
	virtual void draw();
};
BalloonPacket s_balloonPacket;
int s_balloonFrame; // its wobble

void balloonCorner(const float* c, const float* fwd, const float* rt, const float* up, float r, float len,
                   float lat, float lon, float wob)
{
	// lat from -1 (its back, the knot) to 1 (its front); round, a little longer along the way
	const float a  = lat * 1.5707963f;
	const float ring = cosf(a), along = sinf(a);
	const float sx = ring * cosf(lon), sy = ring * sinf(lon);
	const float rr = r * (1.0f + 0.06f * sinf(wob + 3.0f * lon)); // its skin wobbling
	float p[3], n[3];
	for (int k = 0; k < 3; k++) {
		n[k] = fwd[k] * along + rt[k] * sx + up[k] * sy;
		p[k] = c[k] + fwd[k] * along * len + (rt[k] * sx + up[k] * sy) * rr;
	}
	// light from above and a little in front of the view (baked), a shine on its top
	const float l = n[1] * 0.8f + 0.35f;
	const float d = l > 0.0f ? (l < 1.0f ? l : 1.0f) : 0.0f;
	const float shine = n[1] > 0.88f ? (n[1] - 0.88f) * 6.0f : 0.0f;
	int g = (int)(70.0f + 170.0f * d + 120.0f * shine), rd = (int)(20.0f + 50.0f * d + 200.0f * shine),
	    b = (int)(25.0f + 45.0f * d + 200.0f * shine);
	GXPosition3f32(p[0], p[1], p[2]);
	GXColor4u8((u8)(rd > 255 ? 255 : rd), (u8)(g > 255 ? 255 : g), (u8)(b > 255 ? 255 : b), 255);
}

// its rubber's pieces: each a small curled scrap (two triangles, folded), green, tumbling
void drawPieces()
{
	float q[12 * 8];
	const int n = PortBazookaBalloonPieces(q, 12);
	if (!n)
		return;
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * 6));
	for (int i = 0; i < n; i++) {
		const float* o = q + i * 8;
		const float ax = o[3], ay = o[4], az = o[5], a = o[6], sz = o[7];
		// two edges across it, turned about its axis by its spin (Rodrigues)
		const float cs = cosf(a), sn = sinf(a);
		float e[2][3];
		const float base[2][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
		for (int k = 0; k < 2; k++) {
			const float* v = base[k];
			const float d  = ax * v[0] + ay * v[1] + az * v[2];
			const float cx = ay * v[2] - az * v[1], cy = az * v[0] - ax * v[2], cz = ax * v[1] - ay * v[0];
			e[k][0] = (v[0] * cs + cx * sn + ax * d * (1.0f - cs)) * sz;
			e[k][1] = (v[1] * cs + cy * sn + ay * d * (1.0f - cs)) * sz;
			e[k][2] = (v[2] * cs + cz * sn + az * d * (1.0f - cs)) * sz;
		}
		const float bend[3] = {ax * sz * 0.5f, ay * sz * 0.5f, az * sz * 0.5f}; // its fold
		const float p0[3] = {o[0] - e[0][0], o[1] - e[0][1], o[2] - e[0][2]};
		const float p1[3] = {o[0] + e[0][0], o[1] + e[0][1], o[2] + e[0][2]};
		const float p2[3] = {o[0] + e[1][0] + bend[0], o[1] + e[1][1] + bend[1], o[2] + e[1][2] + bend[2]};
		const float p3[3] = {o[0] - e[1][0] + bend[0], o[1] - e[1][1] + bend[1], o[2] - e[1][2] + bend[2]};
		const u8 lit = (u8)(150 + (int)(60.0f * sn)); // catching the light as it turns
		GXPosition3f32(p0[0], p0[1], p0[2]);
		GXColor4u8(30, lit, 45, 255);
		GXPosition3f32(p1[0], p1[1], p1[2]);
		GXColor4u8(30, lit, 45, 255);
		GXPosition3f32(p2[0], p2[1], p2[2]);
		GXColor4u8(40, (u8)(lit - 40), 50, 255);
		GXPosition3f32(p0[0], p0[1], p0[2]);
		GXColor4u8(30, lit, 45, 255);
		GXPosition3f32(p1[0], p1[1], p1[2]);
		GXColor4u8(30, lit, 45, 255);
		GXPosition3f32(p3[0], p3[1], p3[2]);
		GXColor4u8(25, (u8)(lit - 60), 35, 255);
	}
	GXEnd();
}

// A heart (the health defeated enemies drop): a puffy red heart standing up, turning about its
// upright, about 50 tall, lit from above (baked, by the turn), a shine on its upper left lobe. Built
// from its outline (the classic heart curve) and two rings inside it pushed out to the front and back.
void heartCorner(const float* c, float cs, float sn, float x, float y, float z, float nx, float ny, float nz)
{
	const float k = 1.7f; // the curve's units (about 32 across) to the game's
	// turned about the upright
	const float wx = x * cs + z * sn, wz = -x * sn + z * cs;
	const float mx = nx * cs + nz * sn, mz = -nx * sn + nz * cs;
	const float l = 0.25f + 0.55f * (ny * 0.7f + mz * 0.45f + mx * 0.2f > 0.0f ? ny * 0.7f + mz * 0.45f + mx * 0.2f : 0.0f);
	const float shine = (ny > 0.55f && nz * nz < 0.6f) ? 0.35f : 0.0f;
	int r = (int)(255.0f * (l + 0.35f) + 255.0f * shine), g = (int)(50.0f * l + 230.0f * shine), b = (int)(60.0f * l + 230.0f * shine);
	GXPosition3f32(c[0] + wx * k, c[1] + y * k, c[2] + wz * k);
	GXColor4u8((u8)(r > 255 ? 255 : r), (u8)(g > 255 ? 255 : g), (u8)(b > 255 ? 255 : b), 255);
}

void heartOutline(float t, float* x, float* y, float* nx, float* ny)
{
	const float st = sinf(t);
	*x = 16.0f * st * st * st;
	*y = 13.0f * cosf(t) - 5.0f * cosf(2.0f * t) - 2.0f * cosf(3.0f * t) - cosf(4.0f * t);
	// its outward normal from the curve's way
	const float dx = 48.0f * st * st * cosf(t), dy = -13.0f * st + 10.0f * sinf(2.0f * t) + 6.0f * sinf(3.0f * t) + 4.0f * sinf(4.0f * t);
	const float l  = sqrtf(dx * dx + dy * dy) + 1e-4f;
	*nx = dy / l, *ny = -dx / l;
}

void drawHearts()
{
	float hs[8 * 4];
	const int n = PortWeaponHearts(hs, 8);
	const int kAround = 28;
	const float kDepth = 9.0f; // its puff, front and back
	for (int h = 0; h < n; h++) {
		const float c[3] = {hs[h * 4], hs[h * 4 + 1], hs[h * 4 + 2]};
		const float cs = cosf(hs[h * 4 + 3]), sn = sinf(hs[h * 4 + 3]);
		for (int side = -1; side <= 1; side += 2) {
			// three bands from the rim in: the rim (flat), a ring at 70% pushed out, one at 35% further, the middle
			const float ring[4] = {1.0f, 0.72f, 0.38f, 0.0f}, deep[4] = {0.0f, 0.62f, 0.9f, 1.0f};
			for (int band = 0; band < 3; band++) {
				GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)((kAround + 1) * 2));
				for (int j = 0; j <= kAround; j++) {
					const float t = 6.2831853f * (float)j / kAround;
					float x, y, nx, ny;
					heartOutline(t, &x, &y, &nx, &ny);
					for (int k = 0; k < 2; k++) {
						const int b2 = band + k;
						const float f = ring[b2], z = deep[b2] * kDepth * side;
						const float flat = 1.0f - deep[b2]; // the rim faces out, the middle front or back
						heartCorner(c, cs, sn, x * f, y * f + 2.0f * (1.0f - f), z, nx * flat, ny * flat, deep[b2] * side);
					}
				}
				GXEnd();
			}
		}
	}
}

void BalloonPacket::draw()
{
	if (!s_haveView)
		return;
	float c[3], way[3], scale;
	const bool flying = PortBazookaBalloon(c, way, &scale);
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
	drawPieces();
	drawHearts();
	if (!flying)
		return;
	// its own frame: forward the way it flies, right and up across it
	float fwd[3] = {way[0], way[1], way[2]};
	float upw[3] = {0.0f, 1.0f, 0.0f};
	if (fabsf(fwd[1]) > 0.95f)
		upw[0] = 1.0f, upw[1] = 0.0f;
	float rt[3] = {fwd[1] * upw[2] - fwd[2] * upw[1], fwd[2] * upw[0] - fwd[0] * upw[2], fwd[0] * upw[1] - fwd[1] * upw[0]};
	float rl = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
	if (rl < 1e-4f)
		return;
	rt[0] /= rl, rt[1] /= rl, rt[2] /= rl;
	float up[3] = {rt[1] * fwd[2] - rt[2] * fwd[1], rt[2] * fwd[0] - rt[0] * fwd[2], rt[0] * fwd[1] - rt[1] * fwd[0]};
	// its size: one that fits in the bazooka's muzzle (about 7 across its middle in first person, 12
	// on his back); the big one 6.5 times that
	const float r = 6.5f * scale, len = 8.0f * scale;
	const float wob = (float)(s_balloonFrame++) * 0.9f;

	const int kRings = 10, kAround = 16;
	for (int i = 0; i < kRings; i++) { // a strip round it for each band, back to front
		const float a0 = -1.0f + 2.0f * (float)i / kRings, a1 = -1.0f + 2.0f * (float)(i + 1) / kRings;
		GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)((kAround + 1) * 2));
		for (int j = 0; j <= kAround; j++) {
			const float lon = 6.2831853f * (float)j / kAround;
			balloonCorner(c, fwd, rt, up, r, len, a1, lon, wob);
			balloonCorner(c, fwd, rt, up, r, len, a0, lon, wob);
		}
		GXEnd();
	}
	// its knot: a small dark green cone at its back
	{
		const float back[3] = {c[0] - fwd[0] * len, c[1] - fwd[1] * len, c[2] - fwd[2] * len};
		const float tip[3]  = {back[0] - fwd[0] * 2.2f * scale, back[1] - fwd[1] * 2.2f * scale, back[2] - fwd[2] * 2.2f * scale};
		GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 10);
		GXPosition3f32(tip[0], tip[1], tip[2]);
		GXColor4u8(20, 90, 30, 255);
		for (int j = 0; j <= 8; j++) {
			const float lon = 6.2831853f * (float)j / 8.0f;
			const float sx = cosf(lon) * 1.4f * scale, sy = sinf(lon) * 1.4f * scale;
			GXPosition3f32(back[0] + rt[0] * sx + up[0] * sy, back[1] + rt[1] * sx + up[1] * sy, back[2] + rt[2] * sx + up[2] * sy);
			GXColor4u8(30, 120, 40, 255);
		}
		GXEnd();
	}
}

} // namespace

// The view's matrix of this frame (the balloon is built in the world)
void PortWeaponModelView(MtxPtr view)
{
	MTXCopy(view, s_view);
	s_haveView = true;
}

// The muzzle's place in a model's own units, forward (0 the rifle, 1 the bazooka, 2 the submachine gun).
float PortWeaponModelMuzzle(int which) { return which == 0 ? kRifleMuzzle : which == 1 ? kBazookaMuzzle : kSmgMuzzle; }

// A pack's muzzle (third person: 0 the rifle's, 1 the bazooka's, 2 the submachine gun's), in game units
// from FLUDD's nozzle_center (x right, y forward, z up).
const float* PortWeaponPackMuzzle(int which) { return which == 0 ? kRiflePackGunMuzzleAt : which == 1 ? kBazookaPackGunMuzzleAt : kSmgPackGunMuzzleAt; }

// A model's place this frame (modapi_held_fludd.cpp): the view's matrix times its place in the
// world (x right, y forward to the muzzle, z up, in its own units).
void PortWeaponModelShow(int which, MtxPtr viewModel)
{
	MTXCopy(viewModel, s_viewModel);
	s_shown = which;
}

// A second model with it this frame (a pack's weapon, in its own place).
void PortWeaponModelShowSecond(int which, MtxPtr viewModel)
{
	MTXCopy(viewModel, s_viewModel2);
	s_shown2 = which;
}

int s_lastShown = -1, s_lastShown2 = -1; // as last shown, to keep (PortWeaponModelKeep)
bool s_noFludd; // FLUDD held with a weapon chosen: never drawn itself, its model or not

// This frame, FLUDD held in view with a weapon (not the normal spray) chosen: FLUDD itself is left
// out however its placing goes (a frame it is not placed showed all of it in view).
void PortWeaponModelNoFludd(bool on) { s_noFludd = on; }

void PortWeaponModelHide()
{
	if (s_shown >= 0)
		s_lastShown = s_shown, s_lastShown2 = s_shown2;
	s_shown = s_shown2 = -1;
}

// Shown again where it was last (FLUDD not placed again this frame: Mario frozen).
void PortWeaponModelKeep()
{
	s_shown  = s_lastShown;
	s_shown2 = s_lastShown2;
}

// As FLUDD is entered for drawing (TWaterGun::perform's CUE_ENTRY): true when a weapon's model is
// entered in its place (FLUDD then left out).
bool PortWeaponModelEntry()
{
	float at[3], way[3], scale; // the bazooka's balloon flying: entered too
	float pieces[8], hearts[4];
	if (s_haveView && j3dSys.getDrawBuffer(0)
	    && (PortBazookaBalloon(at, way, &scale) || PortBazookaBalloonPieces(pieces, 1) || PortWeaponHearts(hearts, 1)))
		j3dSys.getDrawBuffer(0)->entryImm(&s_balloonPacket, 0);
	if (s_shown < 0 || !j3dSys.getDrawBuffer(0))
		return s_noFludd;
	j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
	return true;
}
