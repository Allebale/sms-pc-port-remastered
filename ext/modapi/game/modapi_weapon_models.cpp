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
// as a packet of the scene's opaque draw buffer too: a round balloon a little longer along the way
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

void BalloonPacket::draw()
{
	float c[3], way[3], scale;
	if (!s_haveView || !PortBazookaBalloon(c, way, &scale))
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
	const float r = 42.0f * scale, len = 52.0f * scale;
	const float wob = (float)(s_balloonFrame++) * 0.9f;

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
	if (scale <= 1.0f) {
		const float back[3] = {c[0] - fwd[0] * len, c[1] - fwd[1] * len, c[2] - fwd[2] * len};
		const float tip[3]  = {back[0] - fwd[0] * 14.0f, back[1] - fwd[1] * 14.0f, back[2] - fwd[2] * 14.0f};
		GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 10);
		GXPosition3f32(tip[0], tip[1], tip[2]);
		GXColor4u8(20, 90, 30, 255);
		for (int j = 0; j <= 8; j++) {
			const float lon = 6.2831853f * (float)j / 8.0f;
			const float sx = cosf(lon) * 9.0f, sy = sinf(lon) * 9.0f;
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
	if (s_haveView && j3dSys.getDrawBuffer(0) && PortBazookaBalloon(at, way, &scale))
		j3dSys.getDrawBuffer(0)->entryImm(&s_balloonPacket, 0);
	if (s_shown < 0 || !j3dSys.getDrawBuffer(0))
		return s_noFludd;
	j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
	return true;
}
