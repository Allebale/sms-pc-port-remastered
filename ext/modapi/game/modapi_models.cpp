// A mod's own models (API 1.8, api->models): triangles with their colours (light baked in), drawn
// with the scene in each view, in the world or on a player's FLUDD (held in view, or on his back:
// its frames, worked out as FLUDD is placed, modapi_held_fludd.cpp). Asked for each tick; drawn as
// a packet of the scene's opaque draw buffer, entered as FLUDD is (TWaterGun::perform's CUE_ENTRY,
// the patch zzzzzzzz-held-fludd-weapon-model). Nothing is drawn or left out till a mod asks.
#include "../modapi_bridge.h"

#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <stdlib.h>
#include <string.h>

int PortViewer();                  // modapi_game.cpp: the view being worked out (co-op: 1 the second)
int PortPlayerIndex(const void* m);
int PortPlayerNow();
bool PortFluddHeld(TMario* mario); // modapi_held_fludd.cpp
bool PortPacketEntered(J3DPacket* p); // modapi_weapon_models.cpp

namespace {

const int kMeshes = 64, kDraws = 96, kTris = 24000;

struct Mesh {
	SmsMod* mod;
	SmsCorner* corners;
	int count;
};
Mesh s_meshes[kMeshes];

struct Draw {
	SmsMesh mesh;
	int frame, player;
	Mtx place;
};
Draw s_draws[kDraws];
int s_drawCount;
SmsCorner s_tris[kTris];
int s_triCount;
int s_hide[2]; // each player's FLUDD left out (1 << SMS_FRAME_HELD, 1 << SMS_FRAME_BACK)

// each player's frames in each view, as last placed
struct Frame {
	bool on;
	Mtx m;
};
Frame s_frames[2][2][4];
Mtx s_viewOf[2];
bool s_haveView[2];

class ModelsPacket : public J3DPacket {
public:
	int view;
	virtual void draw();
};
ModelsPacket s_packets[2];

void corners(const SmsCorner* c, int n)
{
	if (n < 3)
		return;
	n -= n % 3;
	for (int at = 0; at < n;) { // in batches the GX call's count holds
		const int k = n - at > 60000 ? 60000 : n - at;
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)k);
		for (int i = 0; i < k; i++) {
			const SmsCorner& q = c[at + i];
			GXPosition3f32(q.x, q.y, q.z);
			GXColor4u8(q.r, q.g, q.b, q.a);
		}
		GXEnd();
		at += k;
	}
}

void setup()
{
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
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
}

void ModelsPacket::draw()
{
	if (!s_haveView[view])
		return;
	setup();
	for (int i = 0; i < s_drawCount; i++) {
		const Draw& d = s_draws[i];
		const Mesh* m = d.mesh >= 1 && d.mesh <= (SmsMesh)kMeshes ? &s_meshes[d.mesh - 1] : 0;
		if (!m || !m->corners)
			continue;
		Mtx world, viewModel;
		if (d.frame == SMS_FRAME_WORLD)
			MTXCopy(d.place, world);
		else {
			const Frame& f = s_frames[d.player][view][d.frame];
			if (!f.on)
				continue;
			MTXConcat(f.m, d.place, world);
		}
		MTXConcat(s_viewOf[view], world, viewModel);
		GXLoadPosMtxImm(viewModel, GX_PNMTX0);
		corners(m->corners, m->count);
	}
	if (s_triCount) {
		GXLoadPosMtxImm(s_viewOf[view], GX_PNMTX0);
		corners(s_tris, s_triCount);
	}
}

} // namespace

// the view's matrix of this frame, each view's (modapi_held_fludd.cpp, as FLUDD is placed)
void PortModelsView(int view, MtxPtr viewMtx)
{
	if (view < 0 || view > 1)
		return;
	MTXCopy(viewMtx, s_viewOf[view]);
	s_haveView[view] = true;
}

// a player's frame in a view as FLUDD is placed (null: not there)
void PortModelsFrame(int player, int view, int frame, MtxPtr world)
{
	if (player < 0 || player > 1 || view < 0 || view > 1 || frame < 1 || frame > 3)
		return;
	Frame& f = s_frames[player][view][frame];
	f.on     = world != nullptr;
	if (world)
		MTXCopy(world, f.m);
}

// each tick, before the mods' (modapi_world.cpp): what they ask for is drawn this tick only
void PortModelsTick()
{
	s_drawCount = 0;
	s_triCount  = 0;
	s_hide[0] = s_hide[1] = 0;
}

// Whether a mod draws or hides anything of this player's now (the old weapon models left out then)
bool PortModelsActive(int player) { return player >= 0 && player < 2 && s_hide[player] != 0; }

// As a player's FLUDD is entered for drawing (TWaterGun::perform's CUE_ENTRY): the models entered for
// this view (once); true when this FLUDD is left out (a mod's model in its place)
bool PortModelsEntry(TWaterGun* gun)
{
	const int view = PortViewer() ? 1 : 0;
	J3DDrawBuffer* b = j3dSys.getDrawBuffer(0);
	if (b && (s_drawCount || s_triCount) && !PortPacketEntered(&s_packets[view])) {
		s_packets[view].view = view;
		b->entryImm(&s_packets[view], 0);
	}
	if (!gun || !gun->mMario)
		return false;
	const int who = PortPlayerIndex(gun->mMario);
	if (who < 0 || who > 1 || !s_hide[who])
		return false;
	const bool heldHere = PortFluddHeld(gun->mMario) && who == view;
	return (s_hide[who] & (1 << (heldHere ? SMS_FRAME_HELD : SMS_FRAME_BACK))) != 0;
}

extern "C" SmsMesh modg_models_create(SmsMod* mod, const SmsCorner* c, int count)
{
	if (!c || count < 3)
		return 0;
	count -= count % 3;
	for (int i = 0; i < kMeshes; i++)
		if (!s_meshes[i].corners) {
			SmsCorner* copy = (SmsCorner*)malloc(sizeof(SmsCorner) * (size_t)count);
			if (!copy)
				return 0;
			memcpy(copy, c, sizeof(SmsCorner) * (size_t)count);
			s_meshes[i].mod     = mod;
			s_meshes[i].corners = copy;
			s_meshes[i].count   = count;
			return (SmsMesh)(i + 1);
		}
	return 0;
}

extern "C" void modg_models_destroy(SmsMod* mod, SmsMesh mesh)
{
	if (mesh < 1 || mesh > (SmsMesh)kMeshes || s_meshes[mesh - 1].mod != mod)
		return;
	Mesh& m = s_meshes[mesh - 1];
	for (int i = 0; i < s_drawCount; i++) // (not drawn this tick after all)
		if (s_draws[i].mesh == mesh)
			s_draws[i].mesh = 0;
	free(m.corners);
	m.corners = 0, m.count = 0, m.mod = 0;
}

// a mod unloading (mod_runtime.cpp): its models gone
extern "C" void modg_models_unload(SmsMod* mod)
{
	for (int i = 0; i < kMeshes; i++)
		if (s_meshes[i].corners && s_meshes[i].mod == mod)
			modg_models_destroy(mod, (SmsMesh)(i + 1));
}

extern "C" int modg_models_draw(SmsMesh mesh, int frame, int player, const float place[3][4])
{
	if (mesh < 1 || mesh > (SmsMesh)kMeshes || !s_meshes[mesh - 1].corners || !place || frame < 0 || frame > 3
	    || s_drawCount >= kDraws)
		return 0;
	if (frame != SMS_FRAME_WORLD && (player < 0 || player > 1))
		return 0;
	Draw& d  = s_draws[s_drawCount++];
	d.mesh   = mesh;
	d.frame  = frame;
	d.player = player;
	memcpy(d.place, place, sizeof(Mtx));
	return 1;
}

extern "C" int modg_models_draw_triangles(const SmsCorner* c, int count)
{
	if (!c || count < 3)
		return 0;
	count -= count % 3;
	if (count > kTris - s_triCount)
		count = (kTris - s_triCount) - (kTris - s_triCount) % 3;
	memcpy(s_tris + s_triCount, c, sizeof(SmsCorner) * (size_t)count);
	s_triCount += count;
	return count > 0;
}

extern "C" int modg_models_frame(int frame, int player, float out[3][4])
{
	if (!out || player < 0 || player > 1 || frame < 1 || frame > 3)
		return 0;
	// its own view's first (held in view is drawn only there), else the other's
	const int views[2] = {player, 1 - player};
	for (int k = 0; k < 2; k++) {
		const Frame& f = s_frames[player][views[k]][frame];
		if (f.on) {
			memcpy(out, f.m, sizeof(Mtx));
			return 1;
		}
	}
	return 0;
}

extern "C" void modg_models_hide_fludd(int player, int frames)
{
	if (player >= 0 && player < 2)
		s_hide[player] |= frames & ((1 << SMS_FRAME_HELD) | (1 << SMS_FRAME_BACK));
}
