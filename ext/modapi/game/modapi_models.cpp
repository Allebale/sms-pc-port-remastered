// A mod's own models (API 1.8, api->models): triangles with their colours (light baked in), drawn
// with the scene in each view, in the world or on a player's FLUDD (held in view, or on his back:
// its frames, worked out as FLUDD is placed, modapi_held_fludd.cpp). Asked for each tick; drawn as
// a packet of the scene's opaque draw buffer, entered as FLUDD is (TWaterGun::perform's CUE_ENTRY,
// the patch zzzzzzzz-held-fludd-mod-models). Nothing is drawn or left out till a mod asks.
#include "../modapi_bridge.h"

#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <M3DUtil/MActor.hpp>
#include <Player/NozzleBase.hpp>
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
TMario* PortPlayerMario(int i);
int PortPlayers();
extern "C" int port_coop_own_fludd_textures(void* mario); // ext/coop: player 2's FLUDD's own textures
// a picture shown in place of a texture, by its data (platform/gx/src/gx_texture.cpp)
extern "C" void GXPC_SetTextureOverride(const void* data, size_t bytes, const unsigned char* rgba, int w, int h)
    __attribute__((weak));

namespace {

// a packet already in a draw buffer's list this frame (each player's FLUDD enters; the models once a
// view). The buffer's list starts its first word (J3DDrawBuffer's layout).
struct DrawBufferPeek {
	J3DPacket** buffer;
};
bool entered(J3DDrawBuffer* b, J3DPacket* p)
{
	J3DPacket** list = reinterpret_cast<DrawBufferPeek*>(b)->buffer;
	int n            = 0;
	for (J3DPacket* q = list ? list[0] : 0; q && n < 100000; q = q->getNextPacket(), n++)
		if (q == p)
			return true;
	return false;
}
bool PortPacketEntered(J3DPacket* p) { return j3dSys.getDrawBuffer(0) && entered(j3dSys.getDrawBuffer(0), p); }

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
int s_gameHide[2]; // ...and the game's own (held in view, he is dead), each frame

// each player's frames in each view, as last placed
struct Frame {
	bool on;
	unsigned tick; // the tick it was placed in
	Mtx m;
};
Frame s_frames[2][2][4];
unsigned s_tick;

// placed lately: a FLUDD not drawn for a while (a portal, a stage's start) leaves no frame behind
bool fresh(const Frame& f)
{
	return f.on && s_tick - f.tick <= 2;
}
Mtx s_viewOf[2];
bool s_haveView[2];

// the mods' pictures (models->texture_create): as given (FLUDD's dress), and in the GameCube's
// RGBA8 layout for GX (4x4 blocks: alpha and red, then green and blue)
const int kTextures = 32;
struct Tex {
	SmsMod* mod;
	int w, h;
	u8* rgba;
	u8* tiled;
	void* block; // (tiled's allocation, 32-aligned within)
	GXTexObj obj;
};
Tex s_textures[kTextures];

void tile(Tex& t)
{
	for (int by = 0; by < t.h / 4; by++)
		for (int bx = 0; bx < t.w / 4; bx++) {
			u8* b = t.tiled + ((by * (t.w / 4) + bx) * 64);
			for (int k = 0; k < 16; k++) {
				const u8* c = t.rgba + (((by * 4 + (k >> 2)) * t.w) + bx * 4 + (k & 3)) * 4;
				b[k * 2]          = c[3];
				b[k * 2 + 1]      = c[0];
				b[32 + k * 2]     = c[1];
				b[32 + k * 2 + 1] = c[2];
			}
		}
}

Tex* texOf(SmsTexture t) { return t >= 1 && t <= (SmsTexture)kTextures && s_textures[t - 1].rgba ? &s_textures[t - 1] : 0; }

// triangles with pictures this tick (models->draw_textured), in batches of one picture and blending
const int kTexCorners = 40000, kBatches = 128;
SmsTexCorner s_tex[kTexCorners];
int s_texCount;
struct Batch {
	int first, count;
	SmsTexture texture;
	int blend;
};
Batch s_batches[kBatches];
int s_batchCount;
bool s_inScene; // SMS_EV_DRAW_SCENE going on: draws drawn at once
bool s_entered[2]; // each view's packet entered with FLUDD this frame (else drawn with the water)
int s_sceneView;

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

void textured(const SmsTexCorner* c, int n, SmsTexture texture, int blend)
{
	if (n < 3)
		return;
	n -= n % 3;
	Tex* t = texOf(texture);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	if (t) {
		GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GXLoadTexObj(&t->obj, GX_TEXMAP0);
		GXSetNumTexGens(1);
		GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	} else {
		GXSetNumTexGens(0);
		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	if (blend == SMS_BLEND_ALPHA) {
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	} else if (blend == SMS_BLEND_ADD) {
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	} else {
		GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	}
	for (int at = 0; at < n;) {
		const int k = n - at > 60000 ? 60000 : n - at;
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)k);
		for (int i = 0; i < k; i++) {
			const SmsTexCorner& q = c[at + i];
			GXPosition3f32(q.x, q.y, q.z);
			GXColor4u8(q.r, q.g, q.b, q.a);
			if (t)
				GXTexCoord2f32(q.u, q.v);
		}
		GXEnd();
		at += k;
	}
	// (back to the plain corners' state)
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetNumTexGens(0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
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
			if (!fresh(f))
				continue;
			MTXConcat(f.m, d.place, world);
		}
		MTXConcat(s_viewOf[view], world, viewModel);
		GXLoadPosMtxImm(viewModel, GX_PNMTX0);
		corners(m->corners, m->count);
	}
	GXLoadPosMtxImm(s_viewOf[view], GX_PNMTX0);
	if (s_triCount)
		corners(s_tris, s_triCount);
	for (int i = 0; i < s_batchCount; i++)
		textured(s_tex + s_batches[i].first, s_batches[i].count, s_batches[i].texture, s_batches[i].blend);
	if (modh_wants(SMS_EV_DRAW_SCENE)) { // the mods' own draws for this view, at once
		SmsEvDrawScene e;
		memset(&e, 0, sizeof e);
		e.view = view;
		memcpy(e.view_mtx, s_viewOf[view], sizeof e.view_mtx);
		const Mtx& v = s_viewOf[view]; // the eye: the view's move undone (its turn's transpose)
		e.eye.x = -(v[0][0] * v[0][3] + v[1][0] * v[1][3] + v[2][0] * v[2][3]);
		e.eye.y = -(v[0][1] * v[0][3] + v[1][1] * v[1][3] + v[2][1] * v[2][3]);
		e.eye.z = -(v[0][2] * v[0][3] + v[1][2] * v[1][3] + v[2][2] * v[2][3]);
		s_inScene   = true;
		s_sceneView = view;
		modh_dispatch(SMS_EV_DRAW_SCENE, &e, sizeof e);
		s_inScene = false;
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
	f.tick   = s_tick;
	if (world)
		MTXCopy(world, f.m);
}

// each tick, before the mods' (modapi_world.cpp): what they ask for is drawn this tick only
void PortModelsDressTick();
void PortHeldFluddPoseTick(); // modapi_held_fludd.cpp
void PortModelsTick()
{
	PortModelsDressTick();
	PortHeldFluddPoseTick();
	s_tick++;
	s_drawCount = 0;
	s_triCount  = 0;
	s_texCount = s_batchCount = 0;
	s_hide[0] = s_hide[1] = 0;
}


// As a player's FLUDD is entered for drawing (TWaterGun::perform's CUE_ENTRY): the models entered for
// this view (once); true when this FLUDD is left out (a mod's model in its place)
bool PortModelsEntry(TWaterGun* gun)
{
	const int view = PortViewer() ? 1 : 0;
	J3DDrawBuffer* b = j3dSys.getDrawBuffer(0);
	if (b && (s_drawCount || s_triCount || s_batchCount || modh_wants(SMS_EV_DRAW_SCENE)) && !PortPacketEntered(&s_packets[view])) {
		s_packets[view].view = view;
		b->entryImm(&s_packets[view], 0);
		s_entered[view] = true;
	}
	if (!gun || !gun->mMario)
		return false;
	const int who = PortPlayerIndex(gun->mMario);
	if (who < 0 || who > 1 || !(s_hide[who] | s_gameHide[who]))
		return false;
	const bool heldHere = PortFluddHeld(gun->mMario) && who == view;
	return ((s_hide[who] | s_gameHide[who]) & (1 << (heldHere ? SMS_FRAME_HELD : SMS_FRAME_BACK))) != 0;
}

// FLUDD left out by the game itself this frame (modapi_held_fludd.cpp: held in view, he is dead)
void PortModelsHideFluddNow(int player, int frames)
{
	if (player >= 0 && player < 2)
		s_gameHide[player] = frames;
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
extern "C" void modg_models_texture_destroy(SmsMod* mod, SmsTexture texture);
extern "C" void modg_models_unload(SmsMod* mod)
{
	for (int i = 0; i < kTextures; i++)
		if (s_textures[i].rgba && s_textures[i].mod == mod)
			modg_models_texture_destroy(mod, (SmsTexture)(i + 1));
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
	if (s_inScene) { // at once, in this view
		Mtx world, viewModel;
		if (frame == SMS_FRAME_WORLD)
			memcpy(world, place, sizeof(Mtx));
		else {
			const Frame& f = s_frames[player][s_sceneView][frame];
			if (!fresh(f))
				return 0;
			MTXConcat(f.m, (MtxPtr)place, world);
		}
		MTXConcat(s_viewOf[s_sceneView], world, viewModel);
		setup();
		GXLoadPosMtxImm(viewModel, GX_PNMTX0);
		corners(s_meshes[mesh - 1].corners, s_meshes[mesh - 1].count);
		GXLoadPosMtxImm(s_viewOf[s_sceneView], GX_PNMTX0);
		return 1;
	}
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
	if (s_inScene) { // at once, in this view
		setup();
		GXLoadPosMtxImm(s_viewOf[s_sceneView], GX_PNMTX0);
		corners(c, count);
		return 1;
	}
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
		if (fresh(f)) {
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

extern "C" int modg_models_draw_textured(const SmsTexCorner* c, int count, SmsTexture texture, int blend)
{
	if (!c || count < 3)
		return 0;
	count -= count % 3;
	if (s_inScene) { // at once, in this view
		setup();
		GXLoadPosMtxImm(s_viewOf[s_sceneView], GX_PNMTX0);
		textured(c, count, texture, blend);
		return 1;
	}
	if (count > kTexCorners - s_texCount)
		count = (kTexCorners - s_texCount) - (kTexCorners - s_texCount) % 3;
	if (count <= 0)
		return 0;
	Batch* b = s_batchCount ? &s_batches[s_batchCount - 1] : 0;
	if (!b || b->texture != texture || b->blend != blend || b->first + b->count != s_texCount) {
		if (s_batchCount >= kBatches)
			return 0;
		b          = &s_batches[s_batchCount++];
		b->first   = s_texCount;
		b->count   = 0;
		b->texture = texture;
		b->blend   = blend;
	}
	memcpy(s_tex + s_texCount, c, sizeof(SmsTexCorner) * (size_t)count);
	s_texCount += count;
	b->count += count;
	return 1;
}

// ------------------------------------------------------------------ pictures

extern "C" SmsTexture modg_models_texture_create(SmsMod* mod, const uint8_t* rgba, int w, int h)
{
	const bool pow2 = w >= 4 && h >= 4 && w <= 512 && h <= 512 && !(w & (w - 1)) && !(h & (h - 1));
	if (!rgba || !pow2)
		return 0;
	for (int i = 0; i < kTextures; i++) {
		Tex& t = s_textures[i];
		if (t.rgba)
			continue;
		t.rgba  = (u8*)malloc((size_t)w * h * 4);
		t.block = malloc((size_t)w * h * 4 + 32);
		if (!t.rgba || !t.block) {
			free(t.rgba), free(t.block);
			t.rgba = 0, t.block = 0;
			return 0;
		}
		t.tiled = (u8*)(((uintptr_t)t.block + 31) & ~(uintptr_t)31);
		t.mod = mod, t.w = w, t.h = h;
		memcpy(t.rgba, rgba, (size_t)w * h * 4);
		tile(t);
		GXInitTexObj(&t.obj, t.tiled, (u16)w, (u16)h, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
		GXInitTexObjLOD(&t.obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
		return (SmsTexture)(i + 1);
	}
	return 0;
}

void redress(SmsTexture texture); // (below)

extern "C" int modg_models_texture_update(SmsMod* mod, SmsTexture texture, const uint8_t* rgba)
{
	Tex* t = texOf(texture);
	if (!t || t->mod != mod || !rgba)
		return 0;
	memcpy(t->rgba, rgba, (size_t)t->w * t->h * 4);
	tile(*t);
	GXInvalidateTexAll();
	redress(texture);
	return 1;
}

void undressTexture(SmsTexture texture);

extern "C" void modg_models_texture_destroy(SmsMod* mod, SmsTexture texture)
{
	Tex* t = texOf(texture);
	if (!t || t->mod != mod)
		return;
	undressTexture(texture);
	for (int i = 0; i < s_batchCount; i++) // (not drawn this tick after all)
		if (s_batches[i].texture == texture)
			s_batches[i].count = 0;
	free(t->rgba), free(t->block);
	memset(t, 0, sizeof *t);
}

// ------------------------------------------------------------------ FLUDD in a mod's picture

namespace {
struct Dressed {
	const void* data;
	u32 bytes;
};
struct Dress {
	SmsTexture texture; // 0 its own
	Dressed list[48];
	int count;
	u32 next; // (frames till its textures are looked over again: another nozzle's, a new stage's)
};
Dress s_dress[2];

void undress(int p)
{
	Dress& d = s_dress[p];
	if (GXPC_SetTextureOverride)
		for (int i = 0; i < d.count; i++)
			GXPC_SetTextureOverride(d.list[i].data, d.list[i].bytes, 0, 0, 0);
	d.count = 0;
}

void dressModel(J3DModel* model, Dress& d, const Tex& t)
{
	J3DModelData* data   = model ? model->getModelData() : 0;
	J3DTexture* textures = data ? data->getTexture() : 0;
	if (!textures)
		return;
	for (u16 i = 0; i < textures->getNum(); i++) {
		const ResTIMG* r = textures->getResTIMG(i);
		if (!r || r->alphaEnabled == 1)
			continue; // a cut-out: as it is
		// where GX reads it: the header's offset added in 32 bits, as on the console
		const void* at  = (const void*)(uintptr_t)(u32)((uintptr_t)r + r->imageDataOffset);
		const u32 bytes = GXGetTexBufferSize(r->width, r->height, r->format, GX_FALSE, 0);
		bool known      = false;
		for (int k = 0; k < d.count && !known; k++)
			known = d.list[k].data == at;
		if (!known && d.count < 48) {
			GXPC_SetTextureOverride(at, bytes, t.rgba, t.w, t.h);
			d.list[d.count].data  = at;
			d.list[d.count].bytes = bytes;
			d.count++;
		}
	}
}
} // namespace

// each tick (modapi_world.cpp's): the dressed FLUDDs' textures looked over (new ones dressed)
void PortModelsDressTick()
{
	if (!GXPC_SetTextureOverride)
		return;
	for (int p = 0; p < 2; p++) {
		Dress& d     = s_dress[p];
		const Tex* t = texOf(d.texture);
		if (!t || p >= PortPlayers()) {
			if (d.count)
				undress(p);
			if (!t)
				d.texture = 0;
			continue;
		}
		if (d.next-- > 0)
			continue;
		d.next       = 30;
		TMario* m    = PortPlayerMario(p);
		TWaterGun* g = m ? m->mWaterGun : 0;
		if (!g)
			continue;
		if (p == 1) // (player 2's FLUDD shared player 1's textures: its own first)
			port_coop_own_fludd_textures(m);
		dressModel(g->mFluddModel ? g->mFluddModel->getModel() : 0, d, *t);
		for (int n = 0; n < 6; n++) {
			TNozzleBase* nozzle = g->mNozzleList[n];
			if (nozzle && nozzle->getMActor())
				dressModel(nozzle->getMActor()->getModel(), d, *t);
		}
	}
}

// a new stage: FLUDD's textures are new ones (dressed again as they are seen)
void PortModelsDressReset()
{
	for (int p = 0; p < 2; p++) {
		undress(p);
		s_dress[p].next = 0;
	}
	memset(s_frames, 0, sizeof s_frames); // a new stage: nothing placed yet
}

void redress(SmsTexture texture)
{
	for (int p = 0; p < 2; p++) {
		Dress& d     = s_dress[p];
		const Tex* t = texOf(texture);
		if (d.texture != texture || !t || !GXPC_SetTextureOverride)
			continue;
		for (int i = 0; i < d.count; i++)
			GXPC_SetTextureOverride(d.list[i].data, d.list[i].bytes, t->rgba, t->w, t->h);
	}
}

void undressTexture(SmsTexture texture)
{
	for (int p = 0; p < 2; p++)
		if (s_dress[p].texture == texture) {
			undress(p);
			s_dress[p].texture = 0;
		}
}

extern "C" int modg_held_fludd_dress(int player, SmsTexture texture)
{
	if (player < 0 || player > 1)
		return 0;
	if (texture && !texOf(texture))
		return 0;
	Dress& d = s_dress[player];
	if (d.texture == texture)
		return 1;
	undress(player);
	d.texture = texture;
	d.next    = 0;
	return 1;
}

// Patch zzzzzzzzz-mod-drops: as the water drops are drawn (TModelWaterManager::perform's draw):
// the mods' models drawn here when FLUDD was not drawn this frame to enter them (first person with
// its other nozzles, FLUDD hidden with Mario's body)
void PortWaterDrawn(MtxPtr view)
{
	const int v        = PortViewer() ? 1 : 0;
	const bool entered = s_entered[v];
	s_entered[v]       = false;
	if (view && !entered && (s_drawCount || s_triCount || s_batchCount || modh_wants(SMS_EV_DRAW_SCENE))) {
		PortModelsView(v, view);
		s_packets[v].view = v;
		s_packets[v].draw();
	}
}
