// Puddle Reflections (port_puddles.h, set by a mod, api->reflections): characters and objects
// reflected in FLUDD's puddles, not only Mario. Game code, with the game's flags and headers.
//
// The game reflects in puddles what has a TMirrorActor that may be "in the mirror" with no mirror
// near (flags 4 and 0x20: Mario, FLUDD, his cap, Yoshi): a copy of its model, drawn mirrored about
// the floor under Mario, through the puddles. As a stage is set up (port_ext.h stageSetup, the
// stage's heap current), each of its actors with a model gets such a copy, unless it has one. A
// copy is drawn only while there are puddles, its actor is near Mario and is neither dead, hidden
// nor clipped out (no ghost of an enemy beaten or a coin taken), and the setting is on.
#include "port_ext.h"
#include "port_puddles.h"
#include "port_grass.h"
#include "port_shadows.h"

#include <Enemy/Conductor.hpp>
#include <Enemy/BossGesso.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/Enemy.hpp>
#include <Enemy/GateKeeper.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <NPC/NpcBase.hpp>
#include <Camera/Camera.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapModel.hpp>
#include <Map/Sky.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/SDLModel.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Map/MapMirror.hpp>
#include <Strategic/MirrorActor.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JDrama/JDRDrawBufObj.hpp>
#include <JSystem/JParticle/JPAEmitterManager.hpp>
#include <MarioUtil/ReinitGX.hpp>
#include <System/EmitterViewObj.hpp>

#include <stdio.h>
#include <string.h>

namespace {

// "鏡シーン" (the mirror scene) in Shift-JIS, as the game names it, written as bytes so that it is
// the same with every compiler's execution character set.
const char kMirrorScene[] = "\x8B\xBE\x83\x56\x81\x5B\x83\x93";

// Room left on the stage's heap: the port's stages have tens of MB spare, and multiplayer's
// puppets and spawns want 1 to 2 MB of it after this.
const s32 kHeapRoom = 4 * 1024 * 1024;

// As far from Mario as a reflection is drawn: puddles go 5000 units from him (TModelWaterManager).
const f32 kNear = 5000.0f;

const int kMostModels = 2048;  // the models that have a copy, the game's and ours

class TPortMirrorActor : public TMirrorActor {
public:
	explicit TPortMirrorActor(TLiveActor* owner)
	    : TMirrorActor("port puddle reflection")
	    , mOwner(owner)
	    , mKind(kindOf(owner))
	    , mSeen(true)
	    , mSeenAt(-1)
	{
	}

	virtual void perform(u32 cue, JDrama::TGraphics* graphics)
	{
		if (shown())
			TMirrorActor::perform(cue, graphics);
	}

private:
	bool shown() const
	{
		if (!port_puddle_reflections() || !gpModelWaterManager
		    || (!port_puddles_near() && !port_water_mirror_active()))
			return false;
		if (!port_reflect_kind(mKind))
			return false;
		if (mOwner->checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_HIDDEN | LIVE_FLAG_CLIPPED_OUT))
			return false;
		if (!gpMarioPos)
			return false;
		const f32 dx = mOwner->mPosition.x - gpMarioPos->x;
		const f32 dz = mOwner->mPosition.z - gpMarioPos->z;
		if (dx * dx + dz * dz >= kNear * kNear)
			return false;
		return seenInReflection();
	}

	// Whether the copy can be seen in the puddles or water at all. The mirror's picture has only
	// the copies in it, not the stage, so a wall between a puddle and what it would reflect hides
	// nothing: a palm behind a house showed in a puddle in front of it. Here the way from the
	// mirror's surface to the object is checked against the stage's collision, as the camera sees
	// it mirrored: where the line from the mirrored camera to the object crosses the surface is
	// where the object shows, and from there up to the object must be clear (TMap::intersectLine).
	// Three heights are tried (its foot, its middle, high up: a palm's crown over a roof), and it
	// shows if any is clear and crosses the surface within the Reflection Distance of Mario. The
	// answer is kept a few ticks, each copy on its own tick, to spread the cost.
	bool seenInReflection() const
	{
		if (!gpMap || !gpCamera || !gpMirrorModelManager || !gpMirrorModelManager->unk24)
			return true;
		const int tick = gpModelWaterManager->unk5E00;
		const int slot = (int)(((unsigned long)this >> 4) % kSeenEvery);
		if (mSeenAt >= 0 && tick - mSeenAt < kSeenEvery && (tick + slot) % kSeenEvery)
			return mSeen;
		mSeenAt = tick;

		const TMirrorCamera* mirror     = gpMirrorModelManager->unk24;
		const JGeometry::TVec3<f32>& n  = mirror->unk84;
		const f32 d                     = mirror->unk90;
		const JGeometry::TVec3<f32>& c  = gpCamera->mPosition;
		const f32 above                 = n.x * c.x + n.y * c.y + n.z * c.z + d;
		if (above <= 0.0f) {  // the camera under the surface: nothing to judge by
			mSeen = true;
			return true;
		}
		// the camera mirrored through the surface
		const JGeometry::TVec3<f32> m(c.x - 2.0f * above * n.x, c.y - 2.0f * above * n.y, c.z - 2.0f * above * n.z);
		const f32 reach = port_puddle_reflection_distance() + 200.0f;
		static const f32 kHeights[3] = {30.0f, 150.0f, 450.0f};
		mSeen = false;
		for (int i = 0; i < 3 && !mSeen; i++) {
			const JGeometry::TVec3<f32> o(mOwner->mPosition.x, mOwner->mPosition.y + kHeights[i], mOwner->mPosition.z);
			const f32 fo = n.x * o.x + n.y * o.y + n.z * o.z + d;
			const f32 fm = -above;
			if (fo <= 0.0f)
				continue;  // under the surface
			const f32 t = fm / (fm - fo);
			JGeometry::TVec3<f32> x(m.x + (o.x - m.x) * t, m.y + (o.y - m.y) * t, m.z + (o.z - m.z) * t);
			const f32 ex = x.x - gpMarioPos->x, ez = x.z - gpMarioPos->z;
			if (ex * ex + ez * ez > reach * reach)
				continue;  // it would show beyond the reflection's disc
			x.y += 20.0f;  // off the floor
			JGeometry::TVec3<f32> hit;
			if (!gpMap->intersectLine(x, o, false, &hit))
				mSeen = true;
		}
		return mSeen;
	}

	// Which of the Reflect switches the actor answers to.
	static int kindOf(TLiveActor* actor)
	{
		if (dynamic_cast<TBaseNPC*>(actor))  // before enemies: an NPC is a TSpineEnemy too
			return PORT_REFLECT_CHARACTERS;
		if (dynamic_cast<TSpineEnemy*>(actor))
			return PORT_REFLECT_ENEMIES;
		if (dynamic_cast<TItem*>(actor) || dynamic_cast<TResetFruit*>(actor))
			return PORT_REFLECT_ITEMS;
		return PORT_REFLECT_OBJECTS;
	}

	static const int kSeenEvery = 6;  // ticks a seenInReflection answer is kept
	TLiveActor* mOwner;
	int mKind;
	mutable bool mSeen;
	mutable int mSeenAt;
};

// The game's own copies of Mario and Yoshi (Mario, his hands, cap and FLUDD, flag 4; Yoshi and
// his tongue, flags 4 and 0x20), with their flags as the game set them. Reflect Mario and Yoshi
// off, each is told not to be in the mirror (flag 1) while no real mirror is near (the hotel's
// keep him), and given back its flags when it is on.
const int kMostPlayerCopies = 64;
TMirrorActor* s_playerCopies[kMostPlayerCopies];
u16 s_playerFlags[kMostPlayerCopies];
int s_playerCount;
bool s_playerHidden;

const J3DModel* s_models[kMostModels];
int s_modelCount;

bool known(const J3DModel* model)
{
	for (int i = 0; i < s_modelCount; i++)
		if (s_models[i] == model)
			return true;
	return false;
}

void remember(const J3DModel* model)
{
	if (s_modelCount < kMostModels)
		s_models[s_modelCount++] = model;
}

void rememberPlayerCopies(JDrama::TViewObjPtrListT<JDrama::TViewObj>* scene)
{
	s_playerCount  = 0;
	s_playerHidden = false;
	JGadget::TList_pointer<JDrama::TViewObj*>& children = scene->getChildren();
	for (JGadget::TList_pointer<JDrama::TViewObj*>::iterator it = children.begin();
	     it != children.end() && s_playerCount < kMostPlayerCopies; ++it) {
		TMirrorActor* mirror = dynamic_cast<TMirrorActor*>(*it);
		if (!mirror || dynamic_cast<TPortMirrorActor*>(mirror) || (mirror->unk1A & 1)
		    || !(mirror->unk1A & (4 | 0x20)))
			continue;
		s_playerCopies[s_playerCount] = mirror;
		s_playerFlags[s_playerCount]  = mirror->unk1A;
		s_playerCount++;
	}
}

void showPlayerCopies()
{
	const bool hide = !port_reflect_kind(PORT_REFLECT_MARIO) && gpMirrorModelManager
	                  && !gpMirrorModelManager->isUnk18Present();
	if (hide == s_playerHidden)
		return;
	s_playerHidden = hide;
	for (int i = 0; i < s_playerCount; i++)
		s_playerCopies[i]->unk1A = (u16)(hide ? s_playerFlags[i] | 1 : s_playerFlags[i]);
}

// Leaving the stage: its copies go with it.
void moveStage() { s_playerCount = 0; }

// ------------------------------------------------------------------ Reflect the Level
// The stage itself in the reflection: its map models (the ground, the buildings) and its sky get
// a copy in the mirror scene too, drawn mirrored as the characters' are while there are puddles or
// the water reflects. The mirror camera looks up from under the mirror's surface, so all of the
// stage on that side of it (the ground's underside, the sea floor) would cover the picture: the
// port's renderer cuts it away at the surface while the mirror's picture is drawn
// (port_mirror_clip, from TMirrorCamera::perform). Made as an area loads, with the setting on.
class TPortLevelMirror : public TMirrorActor {
public:
	TPortLevelMirror()
	    : TMirrorActor("port level reflection")
	{
	}

	virtual void perform(u32 cue, JDrama::TGraphics* graphics)
	{
		// only while a puddle near Mario or the water shows the reflection (the stage drawn twice
		// costs the most of all)
		if (port_reflect_level() && gpModelWaterManager && (port_puddles_near() || port_water_mirror_active()))
			TMirrorActor::perform(cue, graphics);
	}
};

// A copy of model for the level's reflection, unless it has one, or memory is short.
bool levelCopy(J3DModel* model, JKRHeap* heap)
{
	if (!model || known(model) || !heap || heap->getTotalFreeSize() < kHeapRoom || s_modelCount >= kMostModels)
		return false;
	TPortLevelMirror* mirror = new TPortLevelMirror();
	mirror->init(model, (u16)(0x20 | (dynamic_cast<SDLModel*>(model) ? 8 : 0)));
	remember(model);
	return true;
}

void makeLevelMirrors()
{
	if (!port_reflect_level() || !gpMap || !gpMap->getModelManager())
		return;
	JKRHeap* heap   = JKRHeap::getCurrentHeap();
	const s32 start = heap ? heap->getTotalFreeSize() : 0;
	int made        = 0;
	TMapModelManager* models = gpMap->getModelManager();
	for (int i = 0; i < models->getJointModelNum(); i++)
		if (TJointModel* joint = models->getJointModel(i))
			made += levelCopy(joint->getModel(), heap);
	TSky* sky = JDrama::TNameRefGen::search<TSky>("\x8B\xF3");  // "空" in Shift-JIS
	if (sky && sky->getMActor())
		made += levelCopy(sky->getMActor()->getModel(), heap);
	fprintf(stderr, "[puddles] the level reflected: %d of its models, the sky %s (%d KB)\n", made,
	        sky ? "too" : "none", heap ? (int)((start - heap->getTotalFreeSize()) / 1024) : 0);
}

// Actors given no reflection. The goop Piranha bosses (the airstrip's, the plaza's gatekeepers)
// and Gooper Blooper drive their model through a calculation they set on its shared data each
// frame (setMtxCalc on joint 0); with a copy of the model drawn too, the real one came out torn
// into streaks and mostly not drawn at all. They are left out.
bool noReflection(TLiveActor* actor)
{
	return dynamic_cast<TBossPakkun*>(actor) || dynamic_cast<TGateKeeperBase*>(actor)
	       || dynamic_cast<TBossGesso*>(actor);
}

// ------------------------------------------------------------------ the mirror's extras
// What the mirror scene has no model of: the effects (fire, smoke, sparkles: the game's particles,
// gpMarioParticleManager's). Each frame the mirror's picture is drawn, a packet of the port's at the
// end of the mirror scene's translucent draw buffer (the one its copies are entered into, j3dSys's
// as the scene is performed) draws them seen from the mirror's camera, with Reflect Objects on.
class TPortMirrorPacket : public J3DPacket {
public:
	virtual void draw()
	{
		TMirrorCamera* camera = gpMirrorModelManager ? gpMirrorModelManager->unk24 : 0;
		if (!camera)
			return;
		MtxPtr view = camera->getUnk30();
		static bool s_told;
		if (!s_told) {
			s_told = true;
			fprintf(stderr, "[puddles] effects drawn in the reflection (seen from the mirror's camera)\n");
		}
		if (port_reflect_kind(PORT_REFLECT_OBJECTS) && gpMarioParticleManager && gpMarioParticleManager->unk3B8
		    && gpCamera) {
			JPADrawInfo info(view, camera->getUnk80() * gpCamera->getFovy(), gpCamera->getAspect());
			gpMarioParticleManager->unk3B8->draw(&info);
		}
		ReInitializeGX();
	}
};

class TPortMirrorExtras : public JDrama::TViewObj {
public:
	TPortMirrorExtras()
	    : JDrama::TViewObj("port mirror extras")
	{
	}

	virtual void perform(u32 cue, JDrama::TGraphics*)
	{
		J3DDrawBuffer* buffer = (cue & CUE_ENTRY) ? j3dSys.getDrawBuffer(1) : 0;  // the scene's translucent one
		if (!buffer || !gpModelWaterManager)
			return;
		if (!gpModelWaterManager->getParticleCount() && !port_water_mirror_active())
			return;  // no puddle or water shows a reflection this frame
		if (!port_reflect_kind(PORT_REFLECT_OBJECTS))
			return;
		buffer->entryImm(&mPacket, 0);
	}

	TPortMirrorPacket mPacket;
};

void stageSetup()
{
	JDrama::TViewObjPtrListT<JDrama::TViewObj>* scene
	    = JDrama::TNameRefGen::search<JDrama::TViewObjPtrListT<JDrama::TViewObj> >(kMirrorScene);
	if (!scene)
		return;
	rememberPlayerCopies(scene);
	scene->insert(new TPortMirrorExtras());  // the effects, in the reflections

	// the models the game gave a copy already (Mario, FLUDD, Yoshi, coins...)
	s_modelCount = 0;
	JGadget::TList_pointer<JDrama::TViewObj*>& children = scene->getChildren();
	for (JGadget::TList_pointer<JDrama::TViewObj*>::iterator it = children.begin(); it != children.end(); ++it)
		if (TMirrorActor* mirror = dynamic_cast<TMirrorActor*>(*it))
			remember(mirror->getUnk10());

	makeLevelMirrors();
	if (!port_puddle_reflections() || !gpConductor)
		return;

	JKRHeap* heap   = JKRHeap::getCurrentHeap();
	const s32 start = heap ? heap->getTotalFreeSize() : 0;
	int made        = 0;
	bool full       = false;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin();
	     !full && it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* actor = (*it)->getObj(i);
			if (!actor || !actor->getMActor() || noReflection(actor))
				continue;
			J3DModel* model = actor->getMActor()->getModel();
			if (!model || known(model))
				continue;
			if (!heap || heap->getTotalFreeSize() < kHeapRoom || s_modelCount >= kMostModels) {
				full = true;
				break;
			}
			// 0x20: in puddles, by its own place; 8: an SDLModel, copied as one
			const u16 flags = (u16)(0x20 | (dynamic_cast<SDLModel*>(model) ? 8 : 0));
			TPortMirrorActor* mirror = new TPortMirrorActor(actor);
			mirror->init(model, flags);
			remember(model);
			made++;
		}
	fprintf(stderr, "[puddles] reflections for %d characters and objects (%d KB)%s\n", made,
	        heap ? (int)((start - heap->getTotalFreeSize()) / 1024) : 0,
	        full ? "; the stage's memory is low, so not for all of them" : "");
}

// ------------------------------------------------------------------ Reflections by Surface
// The surface a puddle lies on, by its floor's type (mBGType, without the shadow and camera
// flags) and the sound material its triangle carries (unk6, what Mario's steps and bumps sound
// like). What each material number means is in the game's sound data, not its code: these were
// told apart by walking Delfino Plaza with the log below. A floor not listed counts as smooth
// stone (0x00: the plaza's tiles, and its carpets, which are pictures on the same floor). How
// strongly each reflects is set by a mod (api->reflections) (port_puddles.h).
int surfaceOf(unsigned type, unsigned material)
{
	switch (type & 0x3FFF) {
	case BG_TYPE_SAND:
	case BG_TYPE_SEA_FLOOR:
		return PORT_PUDDLE_SAND;
	case BG_TYPE_WET_GROUND:
	case BG_TYPE_HYDROPHOBIC:
		return PORT_PUDDLE_WET;
	default:
		break;
	}
	switch (material) {
	case 0x05: return PORT_PUDDLE_SAND;
	case 0x07: return PORT_PUDDLE_WOOD;   // painted wood (the boardwalks)
	case 0x0C:                             // metal (the corrugated roofs)
	case 0x0D: return PORT_PUDDLE_METAL;  // painted metal (manhole covers)
	case 0x10: return PORT_PUDDLE_GRASS;
	case 0x12: return PORT_PUDDLE_CLAY;   // clay roof tiles
	case 0x17: return PORT_PUDDLE_CANVAS; // the bouncy awnings
	default: return PORT_PUDDLE_STONE;
	}
}

// Tenths of a percent of the game's own strength.
int floorStrength(unsigned type, unsigned material)
{
	return port_puddle_surface_strength(surfaceOf(type, material));
}

u8 surfaceAlpha(unsigned type, unsigned material)
{
	const int tenths = floorStrength(type, material);
	return (u8)((tenths < 0 ? 0 : tenths > 1000 ? 1000 : tenths) * 255 / 1000);
}

// While Reflections by Surface is on: the floor under Mario, logged each time it changes and
// stays changed a moment, so that the material numbers can be told apart by walking.
// The log: why puddles show no reflection, when it is not this code's doing. The game draws
// none while Mario is in a real mirror's room (TMirrorModelManager: the mirror's plane, not the
// floor, is reflected about then), nor while the floor under him is none (in the air off a ledge,
// say). Each change of either is logged once it has lasted a moment.
void logMirrorState()
{
	static int s_state = -1, s_seen = -1, s_steady;
	if (!gpMirrorModelManager || !gpMarioGroundPlane)
		return;
	const TBGCheckData* floor = *gpMarioGroundPlane;
	const int state = gpMirrorModelManager->isUnk18Present() ? 1 : (!floor || !floor->isLegal()) ? 2 : 0;
	if (state != s_seen) {
		s_seen   = state;
		s_steady = 0;
		return;
	}
	if (++s_steady != 30 || state == s_state)
		return;
	s_state = state;
	if (state == 1)
		fprintf(stderr, "[puddles] Mario is in a real mirror's area (mirror %d): the game shows no puddle "
		        "reflections here\n", gpMirrorModelManager->getUnk18());
	else if (state == 2)
		fprintf(stderr, "[puddles] no floor under Mario: no puddle reflections\n");
	else
		fprintf(stderr, "[puddles] puddle reflections shown (no real mirror near, floor under Mario)\n");
}

void afterMovement()
{
	showPlayerCopies();
	logMirrorState();

	static unsigned s_logged = 0xFFFFFFFFu, s_seen = 0xFFFFFFFFu;
	static int s_steady;
	if (!port_puddle_surface() || !gpMarioGroundPlane || !*gpMarioGroundPlane)
		return;
	const TBGCheckData* floor = *gpMarioGroundPlane;
	if (!floor->isLegal())
		return;
	const unsigned key = (unsigned)floor->mBGType << 8 | floor->unk6;
	if (key != s_seen) {
		s_seen   = key;
		s_steady = 0;
		return;
	}
	if (++s_steady != 15 || key == s_logged)  // a moment: 15 ticks
		return;
	s_logged = key;
	const int tenths = floorStrength(floor->mBGType, floor->unk6);
	fprintf(stderr, "[puddles] Mario stands on: floor type 0x%04X, material 0x%02X (reflects %d.%d%%)\n",
	        floor->mBGType, floor->unk6, tenths / 10, tenths % 10);
}

// Filled by name: a hook added to PortExtension anywhere never shifts it.
PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "reflections";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		s_extension.moveStage     = moveStage;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

extern "C" int port_puddle_surface_kind(unsigned type, unsigned material) { return surfaceOf(type, material); }

extern "C" unsigned port_puddle_surface_alpha(unsigned type, unsigned material)
{
	return surfaceAlpha(type, material);
}

// From TMirrorCamera::perform (zzzzz-ext-25-water-reflections), its projection just set: with
// Reflect the Level on and the mirror's surface a puddle's or the water's (no real mirror near),
// the renderer cuts away what is below that surface while the mirror's picture is drawn. The
// surface (unk84, unk90: n . p + d = 0, the side to keep where it is above 0) is taken into the
// mirror camera's view (unk30, world to view: v = A p + t, so the plane there is A n, d - A n . t)
// and raised 3 units, so the surface itself goes too. The next projection set turns it off.
extern "C" void GXPC_SetUserClipPlane(int on, const float* plane);

void port_mirror_clip(TMirrorCamera* mirror)
{
	GXPC_ShadowNotMain();  // Real Shadows: the mirror's view is not the main camera's
	port_lights_view(mirror ? mirror->unk30 : 0);  // Fire Lights, seen from the mirror
	if (!port_reflect_level() || !mirror || !gpMirrorModelManager || gpMirrorModelManager->isUnk18Present())
		return;
	MtxPtr v                       = mirror->unk30;
	const JGeometry::TVec3<f32>& n = mirror->unk84;
	f32 plane[4];
	for (int r = 0; r < 3; r++)
		plane[r] = v[r][0] * n.x + v[r][1] * n.y + v[r][2] * n.z;
	plane[3] = mirror->unk90 - (plane[0] * v[0][3] + plane[1] * v[1][3] + plane[2] * v[2][3]) - 3.0f;
	GXPC_SetUserClipPlane(1, plane);
}
