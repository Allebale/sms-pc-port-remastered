// Real Shadows (port_shadows.h): each frame, as the main camera sets its projection, the sun's
// direction and where its shadow map is centred (Mario, or what the camera looks at) go to the
// renderer, which draws the map from what the camera draws and darkens what it shadows.
//
// The sun: the stage's sun in the sky (TSunModel), else the game's light (gpLightManager), else a
// light from high above; never lower than about 25 degrees over the horizon (at sunset the shadows
// would run the length of the stage). With Keep the Original Shadow Direction on, exactly the way
// the game's own shadows fell: straight down, under what casts them.
#include "port_ext.h"
#include "port_shadows.h"
#include "port_puddles.h"

#include <Camera/Camera.hpp>
#include <Camera/SunMgr.hpp>
#include <Camera/SunModel.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Enemy.hpp>
#include <M3DUtil/MActor.hpp>
#include <NPC/NpcBase.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Player/Mario.hpp>
#include <Player/Yoshi.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const f32 kHalf       = 2000.0f;  // the map covers this far either side of Mario
const f32 kLowest     = 0.42f;    // the sun's direction's least height (sin of about 25 degrees)
const f32 kStrength[] = {0.0f, 0.35f, 0.55f};

// ------------------------------------------------------------------ the level's own shadows
// Some stages have shadows painted in as part of the level: dark see-through shapes on the ground
// under buildings and trees. While Real Shadows are on they are hidden, so that nothing is shadowed
// twice: the map's materials whose names say they are shadows ("kage", Japanese for shadow, or
// "shadow"). Each stage's map materials are in the log, to find the ones these miss.
const int kMostBaked = 256;
J3DShape* s_baked[kMostBaked];
int s_bakedCount;
bool s_bakedHidden;

bool shadowName(const char* name)
{
	char low[64];
	int n = 0;
	for (; name && name[n] && n < 63; n++)
		low[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
	low[n] = 0;
	return strstr(low, "kage") || strstr(low, "shadow") || strstr(low, "shade");
}

void findBaked(J3DModel* model, int index)
{
	J3DModelData* data = model ? model->getModelData() : 0;
	JUTNameTab* names  = data ? data->getMaterialName() : 0;
	if (!data || !names)
		return;
	char line[1024];
	int len = snprintf(line, sizeof line, "[shadows] the map's model %d, %d materials:", index, data->getMaterialNum());
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		const char* name = names->getName((u16)i);
		const bool baked = name && shadowName(name);
		if (len < (int)sizeof line - 40)
			len += snprintf(line + len, sizeof line - len, " %s%s", name ? name : "?", baked ? "(shadow)" : "");
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		if (baked && mat && mat->getShape() && s_bakedCount < kMostBaked)
			s_baked[s_bakedCount++] = mat->getShape();
	}
	fprintf(stderr, "%s\n", line);
}

// Round Shadows Under Characters: the characters', enemies' and Yoshi's models (their shapes'
// display lists), which then cast no sun shadow (their round one is theirs); Mario's own draw is
// known to the renderer already (GXPC_ShadowTag).
int registerModel(J3DModel* model)
{
	J3DModelData* data = model ? model->getModelData() : 0;
	if (!data)
		return 0;
	int lists = 0;
	for (u16 m = 0; m < data->getMaterialNum(); m++) {
		J3DMaterial* mat = data->getMaterialNodePointer(m);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		for (u32 g = 0; g < shape->getMtxGroupNum(); g++) {
			J3DShapeDraw* draw = shape->getShapeDraw((u16)g);
			if (draw && draw->getDisplayList()) {
				GXPC_ShadowCharacter(draw->getDisplayList(), draw->getDisplayListSize());
				lists++;
			}
		}
	}
	return lists;
}

void registerCharacters()
{
	int models = 0, lists = 0;
	if (gpConductor)
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
			for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
				TLiveActor* actor = (*it)->getObj(i);
				if (!actor || !actor->getMActor() || !dynamic_cast<TSpineEnemy*>(actor))
					continue;  // an NPC is a TSpineEnemy too
				const int n = registerModel(actor->getMActor()->getModel());
				models += n > 0, lists += n;
			}
	// Yoshi, where he is
	TMario* m = gpMarioOriginal;
	const int yoshi = m && m->mYoshi && m->mYoshi->mActor ? registerModel(m->mYoshi->mActor->getModel()) : 0;
	fprintf(stderr, "[shadows] %d characters and enemies known (%d parts)%s\n", models, lists,
	        yoshi ? ", and Yoshi" : "");
}

void stageSetup()
{
	registerCharacters();
	s_bakedCount  = 0;
	s_bakedHidden = false;
	if (!gpMap || !gpMap->getModelManager())
		return;
	TMapModelManager* models = gpMap->getModelManager();
	for (int i = 0; i < models->getJointModelNum(); i++)
		if (TJointModel* joint = models->getJointModel(i))
			findBaked(joint->getModel(), i);
	fprintf(stderr, "[shadows] %d of the level's own shadow shapes found\n", s_bakedCount);
}

// Hidden while Real Shadows are on, shown again when they are off.
void hideBaked(bool hide)
{
	if (hide == s_bakedHidden)
		return;
	s_bakedHidden = hide;
	for (int i = 0; i < s_bakedCount; i++)
		if (hide)
			s_baked[i]->onFlag(J3DShpFlag_Visible);  // set: not drawn (J3DJoint::entryIn)
		else
			s_baked[i]->offFlag(J3DShpFlag_Visible);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name       = "shadows";
		s_extension.stageSetup = stageSetup;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

extern "C" void GXPC_Ladders3D(int on);
extern "C" void GXPC_ShadowCastOnly(int on);

// Water in the air casts a shadow too (with Water Shadows and Real Shadows): FLUDD's drops and its
// spray weapons' shots (all of gpModelWaterManager's flying drops), drawn only into the sun's shadow
// map (GXPC_ShadowCastOnly), as the frame's shadows begin, each facing the sun. FLUDD's own drops
// are stretched along their flight (a frame of it) so a stream's shadows run into one another; the
// SMG's longer still (a streak); the other weapons' a round disc. At most 400 a frame.
static void waterDisc(f32 x, f32 y, f32 z, f32 r, const f32* u, const f32* v, const f32* ca, const f32* sa, int sides)
{
	for (int k = 0; k < sides; k++) {
		const int j = (k + 1) % sides;
		GXPosition3f32(x, y, z);
		GXColor4u8(255, 255, 255, 255);
		GXPosition3f32(x + (u[0] * ca[k] + v[0] * sa[k]) * r, y + (u[1] * ca[k] + v[1] * sa[k]) * r, z + (u[2] * ca[k] + v[2] * sa[k]) * r);
		GXColor4u8(255, 255, 255, 255);
		GXPosition3f32(x + (u[0] * ca[j] + v[0] * sa[j]) * r, y + (u[1] * ca[j] + v[1] * sa[j]) * r, z + (u[2] * ca[j] + v[2] * sa[j]) * r);
		GXColor4u8(255, 255, 255, 255);
	}
}

static void waterShadows(float (*view)[4], const f32 sun[3])
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || !w->mParticleCount)
		return;
	// across the sun: two ways at right angles to it
	f32 ux = -sun[2], uy = 0.0f, uz = sun[0];
	f32 ul = sqrtf(ux * ux + uz * uz);
	if (ul < 1e-3f)
		ux = 1.0f, uz = 0.0f, ul = 1.0f;
	ux /= ul, uz /= ul;
	const f32 vx = sun[1] * uz - sun[2] * uy, vy = sun[2] * ux - sun[0] * uz, vz = sun[0] * uy - sun[1] * ux;
	const f32 U[3] = {ux, uy, uz}, V[3] = {vx, vy, vz};
	const int kSides = 8;
	f32 ca[kSides], sa[kSides];
	for (int k = 0; k < kSides; k++)
		ca[k] = cosf(6.2831853f * k / kSides), sa[k] = sinf(6.2831853f * k / kSides);
	const int kVerts = kSides * 3 * 2 + 6; // a disc at each end and the band between
	int n = 0;
	for (int i = 0; i < (int)w->mParticleCount && n < 400; i++)
		if ((w->mParticleFlagSOA[i] & 0xf) == 1 && w->mParticleLifetimeSOA[i] > 0.0f)
			n++;
	if (!n)
		return;
	GXPC_ShadowCastOnly(1);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(view, GX_PNMTX0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	GXSetNumTexGens(0);
	GXSetNumIndStages(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetCullMode(GX_CULL_NONE);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetColorUpdate(GX_TRUE);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, (u16)(n * kVerts));
	int done = 0;
	for (int i = 0; i < (int)w->mParticleCount && done < n; i++) {
		if ((w->mParticleFlagSOA[i] & 0xf) != 1 || w->mParticleLifetimeSOA[i] <= 0.0f)
			continue;
		done++;
		const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
		const JGeometry::TVec3<f32>& vel = w->mParticleVelocitySOA[i];
		f32 r = w->mParticleSizeSOA[i] * 0.35f;
		r     = r < 3.0f ? 3.0f : r > 120.0f ? 120.0f : r;
		// whose drop: the spray weapons' tag above the game's own attack (FLUDD's none, 0; the SMG 4)
		const int weapon = ((unsigned short)w->mParticleAttackSOA[i] >> 8) & 0xf;
		f32 back = 0.0f, ahead = 0.0f; // its flight's frames behind and ahead of it
		if (weapon == 0)
			back = 0.8f, ahead = 0.3f;
		else if (weapon == 4)
			back = 6.0f, ahead = 1.0f, r *= 0.8f;
		// its flight, flattened across the sun
		const f32 along = vel.x * sun[0] + vel.y * sun[1] + vel.z * sun[2];
		f32 dx = vel.x - sun[0] * along, dy = vel.y - sun[1] * along, dz = vel.z - sun[2] * along;
		f32 dl = sqrtf(dx * dx + dy * dy + dz * dz);
		if (dl * (back + ahead) > 1500.0f) {
			const f32 k = 1500.0f / (dl * (back + ahead));
			back *= k, ahead *= k;
		}
		const f32 ax = p.x - dx * back, ay = p.y - dy * back, az = p.z - dz * back;
		const f32 bx = p.x + dx * ahead, by = p.y + dy * ahead, bz = p.z + dz * ahead;
		waterDisc(ax, ay, az, r, U, V, ca, sa, kSides);
		waterDisc(bx, by, bz, r, U, V, ca, sa, kSides);
		// the band between: across the flight, facing the sun
		f32 sx = sun[1] * dz - sun[2] * dy, sy = sun[2] * dx - sun[0] * dz, sz = sun[0] * dy - sun[1] * dx;
		f32 sl = sqrtf(sx * sx + sy * sy + sz * sz);
		if (sl < 1e-4f)
			sx = ux, sy = uy, sz = uz, sl = 1.0f;
		sx *= r / sl, sy *= r / sl, sz *= r / sl;
		const f32 q[6][3] = {{ax + sx, ay + sy, az + sz}, {ax - sx, ay - sy, az - sz}, {bx + sx, by + sy, bz + sz},
		                     {bx + sx, by + sy, bz + sz}, {ax - sx, ay - sy, az - sz}, {bx - sx, by - sy, bz - sz}};
		for (int k = 0; k < 6; k++) {
			GXPosition3f32(q[k][0], q[k][1], q[k][2]);
			GXColor4u8(255, 255, 255, 255);
		}
	}
	GXEnd();
	GXPC_ShadowCastOnly(0);
}

void port_shadows_main_view(void* camera, float (*view)[4])
{
	if (camera != (void*)gpCamera || !view)
		return;
	port_lights_view(view);  // Fire Lights, seen from the main camera
	port_wet_characters_view(view);  // Wet Characters: the wet ones, in this view
	GXPC_Ladders3D(port_ladders_3d());  // 3D Ladders (port_ladders.cpp)
	port_grab_view(view);  // the grab log: what Mario takes hold of, and its textures
	GXPC_AOFrame(port_ao(), port_ao_strength(), port_ao_distance(), port_depth_view(), port_ao_quality(), port_ao_half_rate());  // Ambient Occlusion
	// Ambient Occlusion: how much Mario (and FLUDD on his back) shades round him and himself (Mario
	// Ambient Occlusion: none at first, no halo): where he stands, from his feet to over his cap,
	// in the camera's view
	GXPC_AOGrass(port_ao_grass(), view);  // Ambient Occlusion Near Grass
	const float keep = port_ao_mario();
	TMario* m = keep < 1.0f ? gpMarioOriginal : 0;
	if (m) {
		float c[2][3];
		const f32 h[2] = {-10.0f, 175.0f};
		for (int e = 0; e < 2; e++)
			for (int r = 0; r < 3; r++)
				c[e][r] = view[r][0] * m->mPosition.x + view[r][1] * (m->mPosition.y + h[e]) +
				          view[r][2] * m->mPosition.z + view[r][3];
		GXPC_AOMario(c[0], c[1], 65.0f, keep);
	} else
		GXPC_AOMario(0, 0, 0.0f, 1.0f);
	const int level = port_shadows();
	hideBaked(level > 0);
	// Round Shadows Under Characters: then Mario, the characters and enemies cast no sun shadow
	GXPC_ShadowRound(level > 0 && port_shadows_round());
	if (level <= 0 || level > 2) {
		static bool s_toldOff;
		if (!s_toldOff)
			fprintf(stderr, "[shadows] off\n");
		s_toldOff = true;
		GXPC_ShadowFrame(view, 0, 0, 0.0f, 0.0f, 0.0f);
		return;
	}
	f32 focus[3];
	if (gpMarioPos) {
		focus[0] = gpMarioPos->x, focus[1] = gpMarioPos->y, focus[2] = gpMarioPos->z;
	} else {
		focus[0] = gpCamera->unk148.x, focus[1] = gpCamera->unk148.y, focus[2] = gpCamera->unk148.z;
	}
	f32 sun[3] = {0.35f, 1.0f, 0.25f};  // high above, a little to the side
	int source = 3;  // 0 the game's own shadows' way, 1 the stage's sun, 2 the game's light, 3 high above
	const bool original = port_shadows_original_direction() != 0;
	const Vec* light    = gpLightManager ? gpLightManager->getLightPos() : 0;
	const bool lit = light && light->y > 0.0f && light->x * light->x + light->y * light->y + light->z * light->z > 1e-6f;
	const bool sky = gpSunModel && gpSunMgr && gpSunMgr->unk14;
	if (original) {
		// exactly where the game's own fall: TMBindShadowManager puts each round, tree's and
		// umbrella's shadow on the ground straight below what casts it (TMBindShadowManager::calcVtx:
		// the request's position, its height the ground's), so the light is straight overhead
		sun[0] = 0.0f, sun[1] = 1.0f, sun[2] = 0.0f;
		source = 0;
	} else if (sky) {
		const JGeometry::TVec3<f32>& p = gpSunModel->mPosition;
		sun[0] = p.x - focus[0], sun[1] = p.y - focus[1], sun[2] = p.z - focus[2];
		source = 1;
	} else if (lit) {
		sun[0] = light->x, sun[1] = light->y, sun[2] = light->z;
		source = 2;
	}
	f32 l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
	if (l < 1e-3f) {
		sun[0] = 0.0f, sun[1] = 1.0f, sun[2] = 0.0f, l = 1.0f;
	}
	sun[0] /= l, sun[1] /= l, sun[2] /= l;
	if (source != 0 && sun[1] < kLowest) {  // raised to the least height, keeping its way round
		const f32 h = sqrtf(sun[0] * sun[0] + sun[2] * sun[2]);
		const f32 k = h > 1e-4f ? sqrtf(1.0f - kLowest * kLowest) / h : 0.0f;
		sun[0] *= k, sun[2] *= k, sun[1] = kLowest;
		if (h <= 1e-4f)
			sun[1] = 1.0f;
	}
	const f32 reach = port_shadows_distance();
	// the log: when they come on, or the sun's source or the distance changes
	static int s_toldLevel = -1, s_toldSource = -1;
	static f32 s_toldFar;
	if (level != s_toldLevel || source != s_toldSource || reach != s_toldFar) {
		static const char* const kSource[] = {"the game's own shadows' direction", "the stage's sun",
		                                      "the game's light (no sun in the sky)", "high above (no light)"};
		fprintf(stderr, "[shadows] %s, from %s (toward it: %.2f %.2f %.2f), reaching %.0f\n",
		        level == 1 ? "soft" : "strong", kSource[source], sun[0], sun[1], sun[2], reach);
		s_toldLevel  = level;
		s_toldSource = source;
		s_toldFar    = reach;
	}
	GXPC_ShadowSoftness(port_shadows_softness());  // Soft Shadow Edges
	GXPC_ShadowMapSize(port_shadows_map_size());     // Shadow Size
	GXPC_ShadowFrame(view, sun, focus, kStrength[level], kHalf, reach);
	if (port_shadows_water())
		waterShadows(view, sun);  // FLUDD's water in the air, and its spray weapons' shots
}
