// The bazooka's own model, held in FLUDD's place while the First Person mod's bazooka is chosen
// (modapi_held_fludd.cpp places it; the spray weapons are modapi_spray_weapons.cpp). A green pipe
// with FLUDD's tank, straps, nozzle and pump handle, made in Blender from simple shapes, low-poly
// with its light baked into its colours, as the games of the GameCube's time looked
// (bazooka_model.inc: each triangle's corners and colours).
//
// Drawn straight with GX, as a packet of the scene's opaque draw buffer entered as FLUDD would be
// (TWaterGun::perform's CUE_ENTRY, patch zzzzzzzz-held-fludd-bazooka-model, which then leaves FLUDD out),
// so it is drawn with the scene, the scene's depth in front of and behind it. Without the mod
// holding the bazooka, nothing is entered and FLUDD is drawn as ever.
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

namespace {

#include "bazooka_model.inc"

Mtx s_viewModel; // the view's matrix and the model's place in the world, this frame
bool s_shown;    // placed this frame (held, the bazooka chosen)

class BazookaPacket : public J3DPacket {
public:
	virtual void draw();
};
BazookaPacket s_packet;

void BazookaPacket::draw()
{
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
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)kBazookaCorners);
	for (int i = 0; i < kBazookaCorners; i++) {
		GXPosition3f32(kBazookaPos[i * 3], kBazookaPos[i * 3 + 1], kBazookaPos[i * 3 + 2]);
		GXColor4u8(kBazookaCol[i * 4], kBazookaCol[i * 4 + 1], kBazookaCol[i * 4 + 2], kBazookaCol[i * 4 + 3]);
	}
	GXEnd();
}

} // namespace

// The model's place this frame (modapi_held_fludd.cpp): the view's matrix times its place in the
// world (x right, y forward to the muzzle, z up, in its own units).
void PortBazookaShow(MtxPtr viewModel)
{
	MTXCopy(viewModel, s_viewModel);
	s_shown = true;
}

void PortBazookaHide() { s_shown = false; }

// As FLUDD is entered for drawing (TWaterGun::perform's CUE_ENTRY): true when the bazooka is
// entered in its place (FLUDD then left out).
bool PortBazookaEntry()
{
	if (!s_shown || !j3dSys.getDrawBuffer(0))
		return false;
	j3dSys.getDrawBuffer(0)->entryImm(&s_packet, 0);
	return true;
}
