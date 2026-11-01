// Moving Vegetation (port_vegetation.h): plants that move. Two kinds:
//
// Palm trees (TMapObjTree: Delfino's, Pinna's, Gelato's, Sirena's, the banana trees). Each frond
// is a joint of its own on a spring (the game wobbles it when Mario lands on it); here the wind
// leans each one in gusts that roll across the stage, a little flutter on top, and FLUDD's water
// presses the fronds it reaches down. The fronds are the game's own, so they move in reflections
// and shadows too, and Mario standing on one rides it.
//
// Everything else leafy (bushes, flowers, a stage's leaves, potted plants): the materials whose
// names say they are plants, in every model as it loads, are registered with the renderer by their
// display lists; it moves their points as it draws them (gx_shader.cpp, sway): the wind leans
// them, their leaves flutter, and Mario and FLUDD's water push them aside. A plant that is a model
// of its own moves more the higher up it, so it stays rooted.
#include "port_ext.h"
#include "port_vegetation.h"

#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/MapObjTree.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <dolphin/os.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>

// The renderer's (platform/gx/src/gx_render.cpp).
extern "C" void GXPC_SwayRegister(const void* list, unsigned size, float minY, float maxY);
extern "C" void GXPC_SwayFrame(const float* world, const float* pushers, int count);

namespace {

const f32 kAmounts[] = {0.0f, 0.6f, 1.0f, 1.8f, 2.7f};  // Off through Very Windy
const f32 kWind[3]   = {0.8f, 0.0f, 0.6f};        // the way the breeze blows (unit)

// Palm fronds are solid only while their tree was touched lately (port_tree_leaf_collides)
const int kTouchedTicks = 240;                      // about as long as the game's fronds take to settle
std::map<const TMapObjTree*, int> s_sinceTouch;     // ticks since Mario last touched each tree

f32 amount()
{
	const int level = port_vegetation();
	return kAmounts[level < 0 || level > 4 ? 0 : level];
}

f32 seconds()
{
	const s32 ms = (s32)OSTicksToMilliseconds(OSGetTime());
	return (f32)(((ms % 600000) + 600000) % 600000) * 0.001f;
}

// ------------------------------------------------------------------ the leafy materials
// Plants by their materials' names (lower case): leaves, palms, flowers, bushes, trees, ivy, potted
// grass; not what is only near them (trunks, bark, shadows, walls, the ground, rock, sand).
bool plantName(const char* name)
{
	char low[64];
	int n = 0;
	for (; name && name[n] && n < 63; n++)
		low[n] = (char)(name[n] >= 'A' && name[n] <= 'Z' ? name[n] + 32 : name[n]);
	low[n] = 0;
	static const char* const kNot[] = {"kage", "shadow", "shade", "trunk", "bark", "miki", "kabe", "wall", "ground",
	                                   "slop", "road", "sand", "suna", "tuti", "soil", "dirt", "rock", "iwa", "gake",
	                                   "floor", "yuka", "shiba", "stone", "pot_", "fence", "env", "street", "ravine"};
	for (unsigned i = 0; i < sizeof kNot / sizeof kNot[0]; i++)
		if (strstr(low, kNot[i]))
			return false;
	static const char* const kYes[] = {"leaf", "leav", "palm", "yashi", "happa", "kusa", "hana", "flower", "bush",
	                                   "shrub", "tree", "plant", "fern", "ivy", "tuta", "vine", "potgrass", "frond", "flo_", "_flo"};
	for (unsigned i = 0; i < sizeof kYes / sizeof kYes[0]; i++)
		if (strstr(low, kYes[i]))
			return true;
	return false;
}

// J3DShape's bounds (its lowest and highest corner in the model), which it keeps to itself: the
// same fields in the same order.
struct ShapeBounds {
	J3DMaterial* material;
	u16 index, groups;
	u32 flags;
	f32 radius;
	Vec min, max;
};

int s_registered;

// ------------------------------------------------------------------ what pushes plants aside
// FLUDD's water near Mario, in the air or just landed: up to five drops at least 90 units apart
// (each pushes what is round it), and the same drops for the palms' fronds.
const int kMostDrops = 96;
f32 s_drops[kMostDrops][3];
int s_dropCount;

void gatherDrops(f32 mx, f32 my, f32 mz)
{
	s_dropCount               = 0;
	TModelWaterManager* water = gpModelWaterManager;
	const int count           = water ? water->getParticleCount() : 0;
	for (int i = 0; i < count && s_dropCount < kMostDrops; i++) {
		const JGeometry::TVec3<f32>& p = water->mParticlePositionSOA[i];
		const f32 dx = p.x - mx, dy = p.y - my, dz = p.z - mz;
		if (dx * dx + dy * dy + dz * dz > 2500.0f * 2500.0f)
			continue;
		s_drops[s_dropCount][0] = p.x, s_drops[s_dropCount][1] = p.y, s_drops[s_dropCount][2] = p.z;
		s_dropCount++;
	}
}

void afterMovement()
{
	const f32 a = amount();
	static bool s_wasOn;
	if ((a > 0.0f) != s_wasOn)
		fprintf(stderr, "[vegetation] Moving Vegetation %s (%d plant parts known)\n", a > 0.0f ? "on" : "off", s_registered);
	s_wasOn = a > 0.0f;
	if (a <= 0.0f || !gpMarioPos) {
		s_dropCount = 0;
		GXPC_SwayFrame(0, 0, 0);
		return;
	}
	const f32 mx = gpMarioPos->x, my = gpMarioPos->y, mz = gpMarioPos->z;
	gatherDrops(mx, my, mz);
	f32 pushers[6 * 4];
	int n = 0;
	pushers[0] = mx, pushers[1] = my + 60.0f, pushers[2] = mz, pushers[3] = 75.0f;  // Mario, his middle
	n++;
	for (int i = 0; i < s_dropCount && n < 6; i++) {
		bool apart = true;
		for (int k = 1; k < n && apart; k++) {
			const f32 dx = pushers[k * 4] - s_drops[i][0], dy = pushers[k * 4 + 1] - s_drops[i][1],
			          dz = pushers[k * 4 + 2] - s_drops[i][2];
			apart = dx * dx + dy * dy + dz * dz > 90.0f * 90.0f;
		}
		if (!apart)
			continue;
		pushers[n * 4] = s_drops[i][0], pushers[n * 4 + 1] = s_drops[i][1], pushers[n * 4 + 2] = s_drops[i][2];
		pushers[n * 4 + 3] = 85.0f;
		n++;
	}
	const f32 world[8] = {kWind[0], kWind[1], kWind[2], seconds(), a, 0.35f, 0.55f, 0.0f};
	GXPC_SwayFrame(world, pushers, n);
}

// A new area: its trees are new (an old tree's place may be a new tree's), none touched yet
void stageSetup() { s_sinceTouch.clear(); }

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "vegetation";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

void port_veg_model(J3DModelData* data)
{
	JUTNameTab* names = data ? data->getMaterialName() : 0;
	if (!names)
		return;
	char line[512];
	int len = 0, found = 0;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		const char* name = names->getName(i);
		if (!name || !plantName(name))
			continue;
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		const ShapeBounds* b = (const ShapeBounds*)shape;
		for (u32 g = 0; g < shape->getMtxGroupNum(); g++) {
			J3DShapeDraw* draw = shape->getShapeDraw((u16)g);
			if (draw && draw->getDisplayList())
				GXPC_SwayRegister(draw->getDisplayList(), draw->getDisplayListSize(), b->min.y, b->max.y);
		}
		found++;
		s_registered++;
		if (len < (int)sizeof line - 80)
			len += snprintf(line + len, sizeof line - len, " %s (%.0f high)", name, b->max.y - b->min.y);
	}
	if (found)
		fprintf(stderr, "[vegetation] a model's plants:%s\n", line);
}

void port_veg_model_named(J3DModelData* data, const char* path)
{
	if (!data || !path)
		return;
	const char* base = strrchr(path, '/');
	base             = base ? base + 1 : path;
	if (!plantName(base))
		return;
	// the whole model's height (all its shapes), so that its petals, leaves and stem bend as one
	f32 lo = 1e30f, hi = -1e30f;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		const ShapeBounds* b = (const ShapeBounds*)shape;
		lo = b->min.y < lo ? b->min.y : lo;
		hi = b->max.y > hi ? b->max.y : hi;
	}
	if (lo > hi)
		return;
	int parts = 0;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		for (u32 g = 0; g < shape->getMtxGroupNum(); g++) {
			J3DShapeDraw* draw = shape->getShapeDraw((u16)g);
			if (draw && draw->getDisplayList())
				GXPC_SwayRegister(draw->getDisplayList(), draw->getDisplayListSize(), lo, hi);
		}
		parts++;
	}
	s_registered += parts;
	fprintf(stderr, "[vegetation] %s: a plant, all %d of its parts (%.0f high)\n", base, parts, hi - lo);
}

void port_tree_sway(TMapObjTree* tree)
{
	const f32 a = amount();
	if (a <= 0.0f || !tree || !tree->mLeaves || tree->mLeafNum <= 0)
		return;
	tree->mFreezeLeaves = false;  // kept moving
	{
		std::map<const TMapObjTree*, int>::iterator it = s_sinceTouch.find(tree);
		if (it != s_sinceTouch.end() && it->second < kTouchedTicks)
			it->second++;
	}
	const f32 t = seconds();
	for (int i = 0; i < tree->mLeafNum; i++) {
		TMapObjLeaf& leaf = tree->mLeaves[i];
		const f32 lx = leaf.mTransform.mMtx[0][3], ly = leaf.mTransform.mMtx[1][3], lz = leaf.mTransform.mMtx[2][3];
		// the gusts, as the renderer's (rolling across the stage), and each frond's own flutter
		const f32 ph   = lx * 0.0021f + lz * 0.0017f;
		const f32 gust = 0.55f + 0.3f * sinf(t * 1.3f + ph) + 0.15f * sinf(t * 2.9f + ph * 2.3f);
		f32 target     = a * (0.035f * gust + 0.012f * sinf(t * 3.7f + (f32)i * 1.9f));
		// FLUDD's water on it: pressed down
		for (int k = 0; k < s_dropCount; k++) {
			const f32 dx = s_drops[k][0] - lx, dy = s_drops[k][1] - ly, dz = s_drops[k][2] - lz;
			if (dx * dx + dy * dy + dz * dz < 170.0f * 170.0f) {
				target += 0.18f;
				break;
			}
		}
		// the spring's rest moved to the target: the game's own step pulls it there
		leaf.mAngularVelocity += target * tree->mLeafStiffness;
	}
}

// In the game a frond's solid shape (TMapCollisionMove) is only in the world on the ticks when the
// tree moves it, and a tree rests (is frozen) until Mario touches it. With Moving Vegetation on the
// fronds never rest, so every palm kept solid fronds all the time: walls in the air where there was
// only a thin leaf. Now the shapes follow the fronds only while the tree was touched lately (Mario's
// touch, or standing on it), like the game; the swaying alone is only what is seen.

void port_tree_touched(TMapObjTree* tree)
{
	if (tree && amount() > 0.0f)
		s_sinceTouch[tree] = 0;
}

bool port_tree_leaf_collides(TMapObjTree* tree, int index)
{
	if (amount() <= 0.0f)
		return true;  // off: the game as it was
	const TBGCheckData* ground = gpMarioGroundPlane ? SMS_GetMarioGroundPlane() : 0;
	if (ground && ground->getActor() == tree)
		s_sinceTouch[tree] = 0;  // he stands on one of its fronds
	std::map<const TMapObjTree*, int>::iterator it = s_sinceTouch.find(tree);
	return it != s_sinceTouch.end() && it->second < kTouchedTicks;
}
