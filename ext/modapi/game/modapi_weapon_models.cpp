// The rifle's, submachine gun's and bazooka's own models, held in FLUDD's place while the First Person mod's rifle or
// bazooka is chosen (modapi_held_fludd.cpp places them; the spray weapons are
// modapi_spray_weapons.cpp). A brown rifle, a blue submachine gun and a green pipe bazooka with FLUDD's tank, straps,
// nozzle and pump handle, made in Blender from simple shapes, low-poly with their light baked into
// their colours, as the games of the GameCube's time looked (rifle_model.inc, smg_model.inc, bazooka_model.inc:
// each triangle's corners and colours).
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

namespace {

#include "rifle_model.inc"
#include "bazooka_model.inc"
#include "smg_model.inc"

struct Model {
	const float* pos;
	const unsigned char* col;
	int corners;
};
const Model kModels[3] = {{kRiflePos, kRifleCol, kRifleCorners}, {kBazookaPos, kBazookaCol, kBazookaCorners}, {kSmgPos, kSmgCol, kSmgCorners}};

Mtx s_viewModel;  // the view's matrix and the model's place in the world, this frame
int s_shown = -1; // the model placed this frame (0 the rifle, 1 the bazooka, 2 the submachine gun), or none
int s_onBack;     // 1 held in view (all of FLUDD left out), 2 on his back (only its nozzle left out)

class WeaponPacket : public J3DPacket {
public:
	virtual void draw();
};
WeaponPacket s_packet;

void WeaponPacket::draw()
{
	if (s_shown < 0 || s_shown > 2)
		return;
	const Model& m = kModels[s_shown];
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(s_viewModel, GX_PNMTX0);
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

} // namespace

// The muzzle's place in a model's own units, forward (0 the rifle, 1 the bazooka, 2 the submachine gun).
float PortWeaponModelMuzzle(int which) { return which == 0 ? kRifleMuzzle : which == 1 ? kBazookaMuzzle : kSmgMuzzle; }

// A model's place this frame (modapi_held_fludd.cpp): the view's matrix times its place in the
// world (x right, y forward to the muzzle, z up, in its own units).
void PortWeaponModelShow(int which, MtxPtr viewModel)
{
	MTXCopy(viewModel, s_viewModel);
	s_shown  = which;
	s_onBack = 1;
}

// The same with FLUDD on his back (third person): the model in its nozzle's place.
void PortWeaponModelShowOnBack(int which, MtxPtr viewModel)
{
	MTXCopy(viewModel, s_viewModel);
	s_shown  = which;
	s_onBack = 2;
}

void PortWeaponModelHide() { s_shown = -1; }

// As FLUDD is entered for drawing (TWaterGun::perform's CUE_ENTRY): with a weapon's model entered
// in its place, 1 (all of FLUDD then left out) or 2 (only its nozzle); 0 none.
int PortWeaponModelEntry()
{
	if (s_shown < 0 || !j3dSys.getDrawBuffer(0))
		return 0;
	j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
	return s_onBack == 2 ? 2 : 1;
}
