// The mod API's FLUDD water drops (api->water) and actors (api->actor), and the game's particle,
// map ray and sound at a point (api->game, api->audio): general tools for any mod (API 1.8).
//
// Each water manager slot keeps a mod's number for its drop (its tag) and the player whose FLUDD
// made it, kept as the manager makes drops and moves them down as others end
// (port_water_drop_made/moved, patch zzzzzzzzzz-coop-water-owner).

#include "../modapi_bridge.h"
#include "port_ext.h"

#include <Enemy/Enemy.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <Enemy/WalkerEnemy.hpp>
#include <Enemy/HamuKuri.hpp>
#include <Enemy/AmiNoko.hpp>
#include <Enemy/BossTelesaObj.hpp>
#include <Enemy/Telesa.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EnemyManager.hpp>
#include <MSound/MSound.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapCollisionData.hpp>
#include <NPC/NpcBase.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/SharedParts.hpp>
#include <NPC/NpcInitData.hpp>
#include <NPC/NpcParts.hpp>
#include <Strategic/LiveManager.hpp>
#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/NozzleBase.hpp>
#include <M3DUtil/MActor.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <Player/SplashManager.hpp>
#include <Map/PollutionManager.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <sms_gx/gx_pc.h>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/MActor.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>

#include <math.h>
#include <stdlib.h>
#include <string.h>

int PortPlayers();                   // modapi_game.cpp: 2 while co-op's player 2 plays
TMario* PortPlayerMario(int player); // ...each player's Mario
extern "C" int port_cheat_infinite_fludd;
extern "C" void port_coop_drop_made(int slot); // ext/coop: whose drop it is (co-op's own)
extern "C" void port_coop_drop_moved(int to, int from);
extern "C" void port_coop_drop_set_owner(int slot, int player);
extern "C" void port_clip_play(const short* pcm, int frames, int rate, float gain); // platform/audio/port_clips.cpp

namespace {

SmsObject obj(JDrama::TNameRef* p) { return (SmsObject)p; }
JDrama::TNameRef* ref(SmsObject o) { return (JDrama::TNameRef*)o; }
void setVec(SmsVec3* v, f32 x, f32 y, f32 z) { v->x = x, v->y = y, v->z = z; }

// ------------------------------------------------------------------ water

u32 s_dropTag[TModelWaterManager::SLOT_NUM];      // a mod's number for each slot's drop (0 the game's)
s8 s_dropPlayer[TModelWaterManager::SLOT_NUM];    // the player whose FLUDD made it, -1 none
u8 s_dropUnseen[TModelWaterManager::SLOT_NUM];   // left out of the water's drawing (a mod draws it)
u32 s_emitTag;       // the drop being made now by a mod (water->emit), and whose
int s_emitPlayer;
bool s_emitUnseen;
bool s_modEmitting;  // ...so SMS_EV_WATER_EMIT does not hear of it
// a player's FLUDD's own stream being made now with a mod's tag (SMS_EV_WATER_EMIT's handler set it)
bool s_tagging;
u32 s_tagNow;
bool s_unseenNow;

// The player whose globals are in force (co-op runs as player 2 for a while), or -1
int playerNow()
{
	for (int p = 0; p < PortPlayers(); p++)
		if (gpMarioOriginal && PortPlayerMario(p) == gpMarioOriginal)
			return p;
	return -1;
}

void dropOut(int slot, SmsWaterDrop* out)
{
	TModelWaterManager* w = gpModelWaterManager;
	const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[slot];
	const JGeometry::TVec3<f32>& v = w->mParticleVelocitySOA[slot];
	SmsWaterDrop d;
	memset(&d, 0, sizeof d);
	setVec(&d.position, p.x, p.y, p.z);
	setVec(&d.velocity, v.x, v.y, v.z);
	d.drop_size = w->mParticleSizeSOA[slot];
	d.alive     = (int)w->mParticleLifetimeSOA[slot];
	d.attack    = w->mParticleAttackSOA[slot] & 0xff;
	d.tag       = s_dropTag[slot];
	d.player    = s_dropPlayer[slot];
	const int at = w->mParticleFlagSOA[slot] & 0xf; // 1 flying, 2 on the ground, 3 on a wall
	d.landed    = at == 2 ? 1 : at == 3 ? 2 : 0;
	d.clean     = w->mWaterParticleTypes[w->mParticleTypeSOA[slot]]->mCleanSize.get() * 32.0f; // as the game's splash
	d.unseen    = s_dropUnseen[slot];
	const uint32_t n = out->size && out->size < sizeof d ? out->size : (uint32_t)sizeof d;
	d.size = n;
	memcpy(out, &d, n);
}

// a player's FLUDD, with the nozzle it has out
TWaterGun* gunOf(int player, TNozzleBase** nozzle)
{
	TMario* m = player >= 0 && player < PortPlayers() ? PortPlayerMario(player) : 0;
	TWaterGun* g = m ? m->mWaterGun : 0;
	TNozzleBase* n = g ? g->getCurrentNozzle() : 0;
	if (!g || !n || !g->mEmitInfo)
		return 0;
	if (nozzle)
		*nozzle = n;
	return g;
}

// ------------------------------------------------------------------ actors

THitActor* hitActor(SmsObject o) { return o ? dynamic_cast<THitActor*>(ref(o)) : 0; }
TLiveActor* liveActor(SmsObject o) { return o ? dynamic_cast<TLiveActor*>(ref(o)) : 0; }

// an enemy that walks about by itself: not one on a fence (net Koopas), the boss's bubbles or the
// Shadow Mario look-alikes; the rooted and flying are none
bool walks(TLiveActor* a)
{
	return dynamic_cast<TWalkerEnemy*>(a) && !dynamic_cast<TAmiNoko*>(a) && !dynamic_cast<TBubble*>(a)
	       && !dynamic_cast<TKageMarioModoki*>(a);
}

uint32_t flagsOf(TLiveActor* a)
{
	if (!a)
		return 0;
	uint32_t f = 0;
	if (a->checkLiveFlag(LIVE_FLAG_AIRBORNE))
		f |= SMS_ACTOR_FLAG_AIRBORNE;
	if (a->checkLiveFlag(LIVE_FLAG_DEAD))
		f |= SMS_ACTOR_FLAG_DEAD;
	if (a->checkLiveFlag(LIVE_FLAG_HIDDEN))
		f |= SMS_ACTOR_FLAG_HIDDEN;
	if (walks(a))
		f |= SMS_ACTOR_FLAG_WALKS;
	if (dynamic_cast<TSmallEnemy*>(a))
		f |= SMS_ACTOR_FLAG_SMALL_ENEMY;
	if (dynamic_cast<TBaseNPC*>(a))
		f |= SMS_ACTOR_FLAG_CHARACTER;
	return f;
}

f32 heightOf(THitActor* a)
{
	const f32 d = a->getDamageHeight(), k = a->getAttackHeight();
	return d > k ? d : k;
}

f32 radiusOf(THitActor* a)
{
	const f32 d = a->getDamageRadius(), k = a->getAttackRadius();
	return d > k ? d : k;
}

} // namespace

// ------------------------------------------------------------------ the water manager's hooks

// A drop made in a slot (TModelWaterManager::makeEmit): whose, and a mod's number for it
extern "C" void port_water_drop_made(int slot)
{
	if (slot < 0 || slot >= TModelWaterManager::SLOT_NUM)
		return;
	s_dropTag[slot]    = s_modEmitting ? s_emitTag : s_tagging ? s_tagNow : 0;
	s_dropUnseen[slot] = s_modEmitting ? s_emitUnseen : s_tagging && s_unseenNow;
	s_dropPlayer[slot] = (s8)(s_modEmitting ? s_emitPlayer : playerNow());
	port_coop_drop_made(slot);
	if (s_modEmitting) // (a mod's drop for a player: theirs, whoever the game runs as)
		port_coop_drop_set_owner(slot, s_emitPlayer);
}

// a drop moved down to another slot as those before it ended (garbageCollect)
extern "C" void port_water_drop_moved(int to, int from)
{
	if (to < 0 || to >= TModelWaterManager::SLOT_NUM || from < 0 || from >= TModelWaterManager::SLOT_NUM)
		return;
	s_dropTag[to]    = s_dropTag[from];
	s_dropUnseen[to] = s_dropUnseen[from];
	s_dropPlayer[to] = s_dropPlayer[from];
	port_coop_drop_moved(to, from);
}

// Patch zzzzzzzzz-mod-drops: a drop left out of the water's drawing (a mod draws it itself)
bool PortWaterDropUnseen(int i) { return i >= 0 && i < TModelWaterManager::SLOT_NUM && s_dropUnseen[i]; }

// ...a drop touching the ground (its first touch) or a wall: told to mods (SMS_EV_WATER_LAND); true
// ends it there
bool waterLands(int i, const TBGCheckData* at, bool wall)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w || i < 0 || i >= (int)w->mParticleCount || !modh_wants(SMS_EV_WATER_LAND))
		return false;
	SmsEvWaterLand e;
	memset(&e, 0, sizeof e);
	e.slot   = i;
	e.tag    = s_dropTag[i];
	e.player = s_dropPlayer[i];
	const JGeometry::TVec3<f32>& p = w->mParticlePositionSOA[i];
	setVec(&e.position, p.x, p.y, p.z);
	setVec(&e.normal, 0.0f, wall ? 0.0f : 1.0f, 0.0f);
	if (at && at->isLegal()) {
		const JGeometry::TVec3<f32>& n = at->getNormal();
		setVec(&e.normal, n.x, n.y, n.z);
		e.on = at->mActor ? obj(const_cast<TLiveActor*>(at->mActor)) : 0;
	}
	e.wall      = wall ? 1 : 0;
	e.drop_size = w->mParticleSizeSOA[i];
	return modh_dispatch(SMS_EV_WATER_LAND, &e, sizeof e) != 0;
}
bool PortWaterDropLands(int i, const TBGCheckData* ground) { return waterLands(i, ground, false); }
bool PortWaterDropWall(int i, const TBGCheckData* wall) { return waterLands(i, wall, true); }

// Every request for water drops (TModelWaterManager::emitRequest): a player's FLUDD's stream is
// told to mods first (SMS_EV_WATER_EMIT), who may cancel it. True: cancelled, *made drops made.
bool PortWaterRequest(const TWaterEmitInfo& asked, int* made)
{
	if (s_modEmitting || s_tagging || !modh_wants(SMS_EV_WATER_EMIT))
		return false;
	const JGeometry::TVec3<f32>& at = asked.mPos.value;
	for (int p = 0; p < PortPlayers(); p++) {
		TMario* m    = PortPlayerMario(p);
		TWaterGun* g = m ? m->mWaterGun : 0;
		if (!g || gpMarioOriginal != m) // (its own FLUDD: the game runs as that player as it sprays)
			continue;
		const f32 dx = at.x - g->mEmitPos[0].x, dy = at.y - g->mEmitPos[0].y, dz = at.z - g->mEmitPos[0].z;
		if (dx * dx + dy * dy + dz * dz > 120.0f * 120.0f) // (its nozzle's stream, each of its streams)
			continue;
		SmsEvWaterEmit ev;
		memset(&ev, 0, sizeof ev);
		ev.player = p;
		ev.count  = asked.mNum.get();
		SmsWaterDrop& d = ev.drop;
		d.size = sizeof d;
		setVec(&d.position, at.x, at.y, at.z);
		const f32 pow = asked.mPow.get();
		setVec(&d.velocity, asked.mDir.value.x * pow, asked.mDir.value.y * pow, asked.mDir.value.z * pow);
		d.drop_size  = asked.mSize.get();
		d.hit_radius = asked.mHitRadius.get();
		d.hit_height = asked.mHitHeight.get();
		d.alive      = asked.mAlive.get();
		d.attack     = asked.mAttack.get();
		d.flags      = asked.mFlag.get();
		d.player     = p;
		if (modh_dispatch(SMS_EV_WATER_EMIT, &ev, sizeof ev)) {
			*made = 0;
			return true;
		}
		if (!d.tag && !d.unseen)
			return false;
		// a handler's tag (or its look of its own): the game's drops made now carry it
		s_tagging  = true;
		s_tagNow   = d.tag;
		s_unseenNow = d.unseen != 0;
		*made      = gpModelWaterManager->emitRequest(asked);
		s_tagging  = false;
		return true;
	}
	return false;
}

// ------------------------------------------------------------------ api->water

extern "C" int modg_water_nozzle_drop(int player, SmsWaterDrop* out)
{
	TNozzleBase* n = 0;
	TWaterGun* g   = gunOf(player, &n);
	if (!g || !out)
		return 0;
	TWaterEmitInfo info(*g->mEmitInfo);
	n->emitCommon(0, &info);
	SmsWaterDrop d;
	memset(&d, 0, sizeof d);
	setVec(&d.position, info.mPos.value.x, info.mPos.value.y, info.mPos.value.z);
	const f32 pow = n->mEmitParams.mEmitPow.get();
	setVec(&d.velocity, info.mDir.value.x * pow, info.mDir.value.y * pow, info.mDir.value.z * pow);
	d.drop_size  = n->mEmitParams.mSize.get();
	d.hit_radius = info.mHitRadius.get();
	d.hit_height = info.mHitHeight.get();
	d.alive      = info.mAlive.get();
	d.attack     = n->mEmitParams.mAttack.get();
	d.flags      = info.mFlag.get();
	d.player     = player;
	d.rate       = (f32)(n->mEmitParams.mDecRate.get() > 0 ? n->mEmitParams.mDecRate.get() : 1);
	const uint32_t sz = out->size && out->size < sizeof d ? out->size : (uint32_t)sizeof d;
	d.size = sz;
	memcpy(out, &d, sz);
	return 1;
}

extern "C" int modg_water_emit(const SmsWaterDrop* d)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!d)
		return -1;
	const int who = d->player >= 0 && d->player < PortPlayers() ? d->player : playerNow();
	TMario* m     = who >= 0 ? PortPlayerMario(who) : gpMarioOriginal;
	if (!w || !m || !m->mWaterGun || !m->mWaterGun->mEmitInfo)
		return -1;
	TWaterEmitInfo info(*m->mWaterGun->mEmitInfo);
	const f32 pow = sqrtf(d->velocity.x * d->velocity.x + d->velocity.y * d->velocity.y + d->velocity.z * d->velocity.z);
	info.mNum.set(1);
	info.mPos.value.set(d->position.x, d->position.y, d->position.z);
	if (pow > 1e-4f)
		info.mDir.value.set(d->velocity.x / pow, d->velocity.y / pow, d->velocity.z / pow);
	else
		info.mDir.value.set(0.0f, -1.0f, 0.0f);
	info.mV.value.set(0.0f, 0.0f, 0.0f);
	info.mPow.set(pow);
	info.mPowTremble.set(0.0f);
	info.mDirTremble.set(0.0f);
	info.mSize.set(d->drop_size > 0.0f ? d->drop_size : info.mSize.get());
	info.mSizeTremble.set(0.0f);
	info.mHitRadius.set(d->hit_radius);
	info.mHitHeight.set(d->hit_height);
	if (d->alive > 0)
		info.mAlive.set((s16)(d->alive < 32767 ? d->alive : 32767));
	info.mAttack.set((s16)(d->attack > 0 && d->attack < 256 ? d->attack : 1));
	info.mFlag.set(d->flags ? d->flags : 0x40);
	const int before = w->mParticleCount;
	s_emitUnseen     = d->size >= offsetof(SmsWaterDrop, unseen) + sizeof(int) && d->unseen;
	s_emitTag        = d->tag;
	s_emitPlayer     = who;
	s_modEmitting    = true;
	const int made   = w->emitRequest(info);
	s_modEmitting    = false;
	s_emitTag        = 0;
	return made && w->mParticleCount > before ? w->mParticleCount - 1 : -1;
}

extern "C" int modg_water_count(void) { return gpModelWaterManager ? gpModelWaterManager->mParticleCount : 0; }

extern "C" int modg_water_get(int slot, SmsWaterDrop* out)
{
	if (!out || slot < 0 || slot >= modg_water_count())
		return 0;
	dropOut(slot, out);
	return 1;
}

extern "C" int modg_water_set(int slot, const SmsWaterDrop* in)
{
	if (!in || slot < 0 || slot >= modg_water_count())
		return 0;
	TModelWaterManager* w = gpModelWaterManager;
	w->mParticlePositionSOA[slot].set(in->position.x, in->position.y, in->position.z);
	w->mParticleVelocitySOA[slot].set(in->velocity.x, in->velocity.y, in->velocity.z);
	w->mParticleLifetimeSOA[slot] = (f32)in->alive;
	s_dropTag[slot]               = in->tag;
	if (in->size >= offsetof(SmsWaterDrop, unseen) + sizeof(int))
		s_dropUnseen[slot] = in->unseen != 0;
	if (in->drop_size > 0.0f)
		w->mParticleSizeSOA[slot] = in->drop_size;
	return 1;
}

extern "C" int modg_water_drop_of(SmsObject sender)
{
	TWaterHitActor* d = sender ? dynamic_cast<TWaterHitActor*>(ref(sender)) : 0;
	if (!d || d->mParticleIndex < 0 || d->mParticleIndex >= modg_water_count())
		return -1;
	return d->mParticleIndex;
}

extern "C" int modg_water_use(int player, int drops)
{
	TNozzleBase* n = 0;
	TWaterGun* g   = gunOf(player, &n);
	if (!g)
		return -1;
	TMario* m = PortPlayerMario(player);
	if (drops > 0 && !(port_cheat_infinite_fludd && m->isMario()))
		g->depleteWater(drops * (n->mEmitParams.mDecRate.get() > 0 ? n->mEmitParams.mDecRate.get() : 1));
	return g->mCurrentWater;
}

// ------------------------------------------------------------------ api->actor

extern "C" int modg_actor_velocity(SmsObject o, SmsVec3* out)
{
	TLiveActor* a = liveActor(o);
	if (!a || !out)
		return 0;
	const JGeometry::TVec3<f32> v = a->getVelocity();
	setVec(out, v.x, v.y, v.z);
	return 1;
}

extern "C" int modg_actor_set_velocity(SmsObject o, const SmsVec3* v)
{
	TLiveActor* a = liveActor(o);
	if (!a || !v)
		return 0;
	JGeometry::TVec3<f32> n(v->x, v->y, v->z);
	a->setVelocity(n);
	return 1;
}

extern "C" uint32_t modg_actor_flags(SmsObject o) { return flagsOf(liveActor(o)); }

extern "C" int modg_actor_set_flags(SmsObject o, uint32_t on, uint32_t off)
{
	TLiveActor* a = liveActor(o);
	if (!a)
		return 0;
	if (on & SMS_ACTOR_FLAG_AIRBORNE) {
		a->mPosition.y += 8.0f; // off the floor it stands on, or the game lands it again at once
		a->onLiveFlag(LIVE_FLAG_AIRBORNE);
	}
	if (off & SMS_ACTOR_FLAG_AIRBORNE)
		a->offLiveFlag(LIVE_FLAG_AIRBORNE);
	return 1;
}

extern "C" int modg_actor_defeat(SmsObject o)
{
	TSmallEnemy* e = o ? dynamic_cast<TSmallEnemy*>(ref(o)) : 0;
	if (!e || e->checkLiveFlag(LIVE_FLAG_DEAD))
		return 0;
	e->kill(); // as a stomp: its own defeat, coins and all
	return 1;
}

extern "C" int modg_actor_near(const SmsVec3* p, float radius, uint32_t kinds, SmsObject* out, int most)
{
	if (!p || !gpConductor)
		return 0;
	int n = 0;
	const f32 r2 = radius * radius;
	for (int list = 0; list < 3; list++) { // the enemy managers, the other live managers, lone actors
		JGadget::TList<TEnemyManager*>::iterator a0 = gpConductor->unk20.begin(), a1 = gpConductor->unk20.end();
		JGadget::TList<TLiveManager*>::iterator b0 = gpConductor->unk10.begin(), b1 = gpConductor->unk10.end();
		JGadget::TList<TLiveActor*>::iterator c0 = gpConductor->unk30.begin(), c1 = gpConductor->unk30.end();
		while (list == 0 ? a0 != a1 : list == 1 ? b0 != b1 : c0 != c1) {
			TLiveManager* mgr = list == 0 ? (TLiveManager*)*a0 : list == 1 ? *b0 : 0;
			TLiveActor* lone  = list == 2 ? *c0 : 0;
			if (list == 0)
				++a0;
			else if (list == 1)
				++b0;
			else
				++c0;
			const int count = mgr ? mgr->getObjNum() : lone ? 1 : 0;
			for (int i = 0; i < count; i++) {
				TLiveActor* a = mgr ? dynamic_cast<TLiveActor*>(mgr->getObj(i)) : lone;
				if (!a || a->checkLiveFlag(LIVE_FLAG_DEAD) || a->checkLiveFlag(LIVE_FLAG_HIDDEN))
					continue;
				const uint32_t f = flagsOf(a);
				if (kinds && !(f & kinds))
					continue;
				const f32 mid = a->mPosition.y + 0.5f * heightOf(a);
				const f32 dx = a->mPosition.x - p->x, dy = mid - p->y, dz = a->mPosition.z - p->z;
				if (dx * dx + dy * dy + dz * dz > r2)
					continue;
				bool seen = false; // (a manager in two lists)
				for (int k = 0; k < n && k < most; k++)
					if (out[k] == obj(a))
						seen = true;
				if (seen)
					continue;
				if (out && n < most)
					out[n] = obj(a);
				n++;
			}
		}
	}
	return n;
}

extern "C" int modg_actor_hit_size(SmsObject o, float* radius, float* height)
{
	THitActor* a = hitActor(o);
	if (!a)
		return 0;
	if (radius)
		*radius = radiusOf(a);
	if (height)
		*height = heightOf(a);
	return 1;
}

extern "C" int modg_actor_push(SmsObject o, float dx, float dz)
{
	TLiveActor* e = liveActor(o);
	if (!e || !gpMap)
		return 0;
	if (!gpMap->isInArea(e->mPosition.x + dx, e->mPosition.z + dz))
		return 0; // the stage's edge
	// moved with the map's own wall test, as actors are: kept out of walls, sliding along them;
	// steps, slopes and kerbs do not stop it (its body's size, not its reach)
	const f32 r      = e->mBodyRadius > 30.0f ? e->mBodyRadius : 30.0f;
	const f32 height = e->mHeadHeight > 60.0f ? e->mHeadHeight * 0.5f : 30.0f;
	TBGWallCheckRecord wall(e->mPosition.x + dx, e->mPosition.y + height, e->mPosition.z + dz, r, 1, 0);
	const bool touched = gpMap->isTouchedWallsAndMoveXZ(&wall);
	e->mPosition.x     = wall.mCenter.x;
	e->mPosition.z     = wall.mCenter.z;
	return touched ? 2 : 1;
}

extern "C" int modg_actor_wall_defeat(SmsObject o)
{
	THamuKuri* stu = o ? dynamic_cast<THamuKuri*>(ref(o)) : 0; // (Strollin' Stus and their kind)
	if (!stu || stu->checkLiveFlag(LIVE_FLAG_DEAD) || stu->getSpine()->getCurrentNerve() == &TNerveHamuKuriWallDie::theNerve())
		return 0;
	stu->getSpine()->reset();
	stu->getSpine()->setNext(&TNerveHamuKuriWallDie::theNerve());
	return 1;
}

extern "C" int modg_actor_send(SmsObject o, SmsObject sender, uint32_t message)
{
	THitActor* a = hitActor(o);
	THitActor* s = sender ? hitActor(sender) : (THitActor*)gpMarioOriginal;
	if (!a || !s)
		return 0;
	return a->receiveMessage(s, message) ? 1 : 0;
}

// ------------------------------------------------------------------ api->game, api->audio

extern "C" int modg_particle(uint32_t id, const SmsVec3* at)
{
	if (!at || !gpMarioParticleManager)
		return 0;
	JGeometry::TVec3<f32> p(at->x, at->y, at->z);
	return gpMarioParticleManager->emit(id, &p, 0, nullptr) ? 1 : 0;
}

// One of the game's effects on an object (game->object_particle), as the game puts its own on its
// actors: bound to its model's joint (TShine::calc's on joint 2) or its position, the actor its
// owner. The particle manager's lasting effects (its groups 1 and 3: one slot per owner, kept while
// asked for each frame, TMarioParticleManager::perform) go on while the mod asks; the others play
// once, following the object.
namespace {
TMarioParticleManager::TInfo* lastingRow(uint32_t id, u8* group)
{
	if (id >= 253 && id < 253 + 214) {
		*group = 1;
		return gpMarioParticleManager->unk10[id - 253];
	}
	if (id >= 486 && id < 486 + 19) {
		*group = 3;
		return gpMarioParticleManager->unk368[id - 486];
	}
	*group = 0;
	return 0;
}
} // namespace

extern "C" int modg_object_particle(SmsObject o, uint32_t id, int joint)
{
	TLiveActor* a = o ? dynamic_cast<TLiveActor*>(ref(o)) : 0;
	if (!a || !gpMarioParticleManager || a->checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_HIDDEN))
		return 0;
	MtxPtr mtx = 0;
	if (joint >= 0) {
		J3DModel* model = a->getMActor() ? a->getMActor()->getModel() : 0;
		if (!model || joint >= (int)model->getModelData()->getJointNum())
			return 0;
		mtx = model->getAnmMtx((u16)joint);
	}
	u8 group = 0;
	if (TMarioParticleManager::TInfo* row = lastingRow(id, &group)) {
		// a lasting effect: one of the slots each such effect has (32 most stages, the game's
		// own owners among them). TMarioParticleManager::getAvailableIdx takes the first when
		// none is free, someone else's: refused instead
		bool slot = false;
		for (int i = 0; i < gpMarioParticleManager->unk3B4 && !slot; i++)
			slot = row[i].mEmitter && row[i].unk0 == a;
		for (int i = 0; i < gpMarioParticleManager->unk3B4 && !slot; i++)
			slot = !row[i].checkFlag(TMarioParticleManager::INFO_FLAG_UNK4) && !row[i].mEmitter;
		if (!slot)
			return 0;
		// out of view the game asks for none (TShine::calc): it starts again when seen
		if (a->checkLiveFlag(LIVE_FLAG_CLIPPED_OUT))
			return 1;
	}
	JPABaseEmitter* e = mtx ? gpMarioParticleManager->emitAndBindToMtxPtr((s32)id, mtx, group, a)
	                        : gpMarioParticleManager->emitAndBindToPosPtr((s32)id, &a->mPosition, group, a);
	return e ? 1 : 0;
}

extern "C" int modg_ray(const SmsVec3* a, const SmsVec3* b, SmsVec3* hit)
{
	if (!a || !b || !gpMap)
		return 0;
	const f32 x = b->x - a->x, y = b->y - a->y, z = b->z - a->z;
	const f32 l = sqrtf(x * x + y * y + z * z);
	if (l < 1.0f)
		return 0;
	const f32 kPiece = 2000.0f; // (the map's own test is for short lines)
	for (f32 at = 0.0f; at < l; at += kPiece) {
		const f32 to = at + kPiece < l ? at + kPiece : l;
		JGeometry::TVec3<f32> p(a->x + x / l * at, a->y + y / l * at, a->z + z / l * at);
		JGeometry::TVec3<f32> q(a->x + x / l * to, a->y + y / l * to, a->z + z / l * to), wall;
		if (!gpMap->isInArea(p.x, p.z) || !gpMap->isInArea(q.x, q.z))
			return 0; // off the stage's map: nothing there
		if (gpMap->intersectLine(p, q, true, &wall)) {
			if (hit)
				setVec(hit, wall.x, wall.y, wall.z);
			return 1;
		}
	}
	return 0;
}

// The game's sounds keep the place they are played at and read it as long as they play: each a
// place of its own here, kept (a sound asked for again near where it plays keeps its place)
extern "C" void modg_play_sound_at(uint32_t id, const SmsVec3* at)
{
	if (!at || !SMSGetMSound())
		return;
	static JGeometry::TVec3<f32> s_places[32];
	static u32 s_ids[32];
	static int s_next;
	int slot = -1;
	for (int i = 0; i < 32 && slot < 0; i++) {
		const f32 dx = s_places[i].x - at->x, dy = s_places[i].y - at->y, dz = s_places[i].z - at->z;
		if (s_ids[i] == id && dx * dx + dy * dy + dz * dz < 200.0f * 200.0f)
			slot = i;
	}
	if (slot < 0)
		slot = s_next++ % 32;
	s_ids[slot] = id;
	s_places[slot].set(at->x, at->y, at->z);
	SMSGetMSound()->startSoundActorWithInfo(id, &s_places[slot], nullptr, 1.0f, 0, 0, nullptr, 0, 4); // (held ones go on as asked each tick)
}

extern "C" int modg_splash(const SmsVec3* at, float size)
{
	if (!at || !gpSplashManager)
		return 0;
	gpSplashManager->newSplash(JGeometry::TVec3<f32>(at->x, at->y, at->z), size);
	return 1;
}

extern "C" int modg_clean_goop(const SmsVec3* at, float radius)
{
	if (!at || !gpPollution || radius <= 0.0f)
		return 0;
	gpPollution->clean(at->x, at->y, at->z, radius);
	return 1;
}

// ------------------------------------------------------------------ a model's parts (api->parts)

namespace {
// Handles for the player's own models (api->parts' player_part): addresses of this array, never objects
char s_playerParts[3];
J3DModel* playerPartModel(int which)
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return 0;
	switch (which) {
	case SMS_PARTS_MARIO:
		return m->getM3UModel() ? m->getM3UModel()->getModel() : 0;
	case SMS_PARTS_YOSHI: {
		TYoshi* y = SMS_GetYoshi();
		return y && y->isHatched() && y->mActor ? y->mActor->getModel() : 0;
	}
	case SMS_PARTS_FLUDD: {
		TWaterGun* g = m->getFludd();
		return g && g->mFluddModel ? g->mFluddModel->getModel() : 0;
	}
	}
	return 0;
}
J3DModel* partsModel(SmsObject o)
{
	const char* c = (const char*)o;
	if (c >= s_playerParts && c < s_playerParts + 3) {
		J3DModel* md = playerPartModel((int)(c - s_playerParts));
		return md && md->getModelData() ? md : 0;
	}
	TLiveActor* a = liveActor(o);
	if (!a)
		return 0;
	J3DModel* md = a->getMActor() ? a->getMActor()->getModel() : a->getModel();
	return md && md->getModelData() ? md : 0;
}
} // namespace

// Joint `name` (NULL or "" the first) of an actor's model, as it was last posed: its matrix
extern "C" int modg_parts_joint(SmsObject o, const char* name, float out[3][4])
{
	J3DModel* md = partsModel(o);
	if (!md || !out)
		return 0;
	J3DModelData* data = md->getModelData();
	int index          = 0;
	if (name && name[0]) {
		JUTNameTab* names = data->getJointName();
		index             = names ? names->getIndex(name) : -1;
	}
	if (index < 0 || index >= (int)data->getJointNum())
		return 0;
	MtxPtr m = md->getAnmMtx(index);
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			out[r][c] = m[r][c];
	return 1;
}

extern "C" int modg_parts_joint_count(SmsObject o)
{
	J3DModel* md = partsModel(o);
	return md ? (int)md->getModelData()->getJointNum() : 0;
}

extern "C" const char* modg_parts_joint_name(SmsObject o, int index)
{
	J3DModel* md      = partsModel(o);
	JUTNameTab* names = md ? md->getModelData()->getJointName() : 0;
	if (!names || index < 0 || index >= (int)md->getModelData()->getJointNum())
		return 0;
	return names->getName((u16)index);
}

// The parts drawn with a material whose name holds `material`, of this actor's model only (its own
// shape packets), drawn in a colour: game->object_set_tint's, a part at a time
extern "C" int modg_parts_tint(SmsObject o, const char* material, SmsColor color)
{
	J3DModel* md = partsModel(o);
	if (!md || !md->getShapePacketArray())
		return 0;
	J3DModelData* data = md->getModelData();
	JUTNameTab* names  = data->getMaterialName();
	int parts          = 0;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		const char* name = names ? names->getName(i) : 0;
		if (material && material[0] && (!name || !strstr(name, material)))
			continue;
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape || shape->getIndex() >= data->getShapeNum())
			continue;
		J3DShapePacket* packet = md->getShapePacket((u16)shape->getIndex());
		port_ext_set_model_tint(packet, packet, (unsigned)sizeof(J3DShapePacket), color);
		parts++;
	}
	return parts;
}

extern "C" int modg_parts_material_count(SmsObject o)
{
	J3DModel* md = partsModel(o);
	return md ? (int)md->getModelData()->getMaterialNum() : 0;
}

extern "C" const char* modg_parts_material_name(SmsObject o, int index)
{
	J3DModel* md      = partsModel(o);
	JUTNameTab* names = md ? md->getModelData()->getMaterialName() : 0;
	if (!names || index < 0 || index >= (int)md->getModelData()->getMaterialNum())
		return 0;
	return names->getName((u16)index);
}

// The parts drawn with a material whose name holds `material` left out of drawing (their shapes' visible
// flag: J3DShpFlag_Visible set hides one), or drawn again
extern "C" int modg_parts_hide(SmsObject o, const char* material, int hidden)
{
	J3DModel* md = partsModel(o);
	if (!md)
		return 0;
	J3DModelData* data = md->getModelData();
	JUTNameTab* names  = data->getMaterialName();
	int parts          = 0;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		const char* name = names ? names->getName(i) : 0;
		if (material && material[0] && (!name || !strstr(name, material)))
			continue;
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		if (hidden)
			shape->onFlag(J3DShpFlag_Visible);
		else
			shape->offFlag(J3DShpFlag_Visible);
		parts++;
	}
	return parts;
}

// A model's vertex positions (api->parts' vertex, set_vertex): its position array as the file has it (each
// vertex in its own joint's frame when it hangs on one), in the array's own number format and byte order
namespace {
struct PosArray {
	u8* base;
	int type, frac, comps;
	bool be;
	u32 count;
};
bool posArray(SmsObject o, PosArray& a)
{
	J3DModel* md = partsModel(o);
	if (!md)
		return false;
	J3DVertexData& vd = md->getModelData()->getVertexData();
	a.base           = (u8*)vd.getVtxPosArray();
	a.count          = vd.getVtxNum();
	a.type = -1, a.frac = 0, a.comps = 3;
	for (GXVtxAttrFmtList* f = vd.getVtxAttrFmtList(); f && f->attr != GX_VA_NULL; f++)
		if (f->attr == GX_VA_POS) {
			a.type = f->type, a.frac = f->frac, a.comps = f->cnt == GX_POS_XY ? 2 : 3;
			break;
		}
	if (!a.base || a.type < 0 || a.type > GX_F32)
		return false;
	a.be = GXPC_IsBigEndianData(a.base) != 0;
	return true;
}
int compSize(int type) { return type == GX_F32 ? 4 : (type == GX_S16 || type == GX_U16) ? 2 : 1; }
float readComp(const PosArray& a, const u8* p)
{
	u32 raw = 0;
	const int n = compSize(a.type);
	for (int k = 0; k < n; k++)
		raw |= (u32)p[a.be ? k : n - 1 - k] << (8 * (n - 1 - k));
	float scale = 1.0f / (float)(1 << a.frac);
	switch (a.type) {
	case GX_U8: return (float)(u8)raw * scale;
	case GX_S8: return (float)(s8)raw * scale;
	case GX_U16: return (float)(u16)raw * scale;
	case GX_S16: return (float)(s16)raw * scale;
	default: {
		float f;
		memcpy(&f, &raw, 4);
		return f;
	}
	}
}
void writeComp(const PosArray& a, u8* p, float value)
{
	u32 raw;
	float scaled = value * (float)(1 << a.frac);
	float rounded = scaled < 0 ? scaled - 0.5f : scaled + 0.5f;
	switch (a.type) {
	case GX_U8: raw = (u8)(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded)); break;
	case GX_S8: raw = (u8)(s8)(rounded < -128 ? -128 : (rounded > 127 ? 127 : rounded)); break;
	case GX_U16: raw = (u16)(rounded < 0 ? 0 : (rounded > 65535 ? 65535 : rounded)); break;
	case GX_S16: raw = (u16)(s16)(rounded < -32768 ? -32768 : (rounded > 32767 ? 32767 : rounded)); break;
	default: memcpy(&raw, &value, 4); break;
	}
	const int n = compSize(a.type);
	for (int k = 0; k < n; k++)
		p[a.be ? k : n - 1 - k] = (u8)(raw >> (8 * (n - 1 - k)));
}
} // namespace

extern "C" int modg_parts_vertex_count(SmsObject o)
{
	PosArray a;
	return posArray(o, a) ? (int)a.count : 0;
}

extern "C" int modg_parts_vertex(SmsObject o, int index, float out[3])
{
	PosArray a;
	if (!out || !posArray(o, a) || index < 0 || (u32)index >= a.count)
		return 0;
	const u8* p = a.base + (size_t)index * a.comps * compSize(a.type);
	for (int c = 0; c < 3; c++)
		out[c] = c < a.comps ? readComp(a, p + c * compSize(a.type)) : 0.0f;
	return 1;
}

extern "C" int modg_parts_set_vertex(SmsObject o, int index, const float in[3])
{
	PosArray a;
	if (!in || !posArray(o, a) || index < 0 || (u32)index >= a.count)
		return 0;
	u8* p = a.base + (size_t)index * a.comps * compSize(a.type);
	for (int c = 0; c < a.comps; c++)
		writeComp(a, p + c * compSize(a.type), in[c]);
	return 1;
}

extern "C" SmsObject modg_parts_player_part(int which)
{
	if (which < 0 || which > 2 || !playerPartModel(which))
		return 0;
	return (SmsObject)&s_playerParts[which];
}

// Yoshi's body colours, one a fruit (TYoshi's bodyColor, which he moves towards each frame): all four
// the mod's colour, or the game's own back
extern "C" GXColor bodyColor[4];
extern "C" int modg_parts_yoshi_color(SmsColor color)
{
	static GXColor s_own[4];
	static bool s_saved;
	if (!s_saved) {
		for (int i = 0; i < 4; i++)
			s_own[i] = bodyColor[i];
		s_saved = true;
	}
	for (int i = 0; i < 4; i++) {
		GXColor c = {(u8)(color >> 24), (u8)(color >> 16), (u8)(color >> 8), 0xFF};
		bodyColor[i] = (color & 0xFF) ? c : s_own[i];
	}
	return 1;
}

// ------------------------------------------------------------------ joints hidden, worn parts left off (api->parts)

// A joint hidden (api->parts' hide_joint): as the model is posed, that joint (and with it every joint under
// it) shrunk to a hundredth about its own place, so what hangs on it is too small to see. Done in the
// joint's calc callback (J3DMtxCalcBasic::recursiveCalc calls it right after the joint's matrix is made,
// before its children's): the game's own callback on that joint, if any, still runs first. The callbacks
// sit on the model data, shared by every actor drawn with it; which models are hidden is kept here.
namespace {
struct HiddenJoint {
	J3DModel* model;
	u16 jnt;
};
const int kMostHiddenJoints = 128;
HiddenJoint s_hiddenJoints[kMostHiddenJoints];
int s_hiddenJointCount;

struct HookedJoint {
	J3DNode* node;
	J3DNodeCallBack own; // the game's callback there before ours (0 none)
};
const int kMostHooked = 512;
HookedJoint s_hooked[kMostHooked];
int s_hookedCount, s_hookedNext;

J3DNodeCallBack ownCallBack(J3DNode* node)
{
	for (int i = 0; i < s_hookedCount; i++)
		if (s_hooked[i].node == node)
			return s_hooked[i].own;
	return 0;
}

void shrinkMtx(MtxPtr m, f32 k)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			m[r][c] *= k;
}

BOOL hiddenJointCallBack(J3DNode* node, BOOL timing)
{
	J3DNodeCallBack own = ownCallBack(node);
	BOOL result         = own ? own(node, timing) : TRUE;
	if (timing != 0)
		return result;
	J3DModel* md = j3dSys.getModel();
	const u16 jnt = static_cast<J3DJoint*>(node)->getJntNo();
	for (int i = 0; i < s_hiddenJointCount; i++) {
		if (s_hiddenJoints[i].model != md || s_hiddenJoints[i].jnt != jnt)
			continue;
		const f32 k = 0.01f; // not 0: the normals' matrices are inverted, and a hundredth is gone from view
		shrinkMtx(J3DSys::mCurrentMtx, k);
		shrinkMtx(md->getAnmMtx(jnt), k);
		J3DSys::mCurrentS.x *= k;
		J3DSys::mCurrentS.y *= k;
		J3DSys::mCurrentS.z *= k;
		break;
	}
	return result;
}

// Our callback on the joint (once: a node already ours keeps the callback it had kept)
bool hookJoint(J3DNode* node)
{
	if (node->getCallBack() == hiddenJointCallBack)
		return true;
	int slot = -1;
	for (int i = 0; i < s_hookedCount && slot < 0; i++)
		if (s_hooked[i].node == node) // a node gone with an earlier stage, its memory used again
			slot = i;
	if (slot < 0) {
		if (s_hookedCount < kMostHooked)
			slot = s_hookedCount++;
		else // full after many stages: the oldest, long gone with its stage
			slot = s_hookedNext++ % kMostHooked;
	}
	s_hooked[slot].node = node;
	s_hooked[slot].own  = node->getCallBack();
	node->setCallBack(hiddenJointCallBack);
	return true;
}

int hideJointOf(J3DModel* md, const char* name, int hidden)
{
	if (!md || !md->getModelData())
		return 0;
	J3DModelData* data = md->getModelData();
	JUTNameTab* names  = data->getJointName();
	const int index    = names ? names->getIndex(name) : -1;
	if (index < 0 || index >= (int)data->getJointNum())
		return 0;
	int at = -1;
	for (int i = 0; i < s_hiddenJointCount && at < 0; i++)
		if (s_hiddenJoints[i].model == md && s_hiddenJoints[i].jnt == (u16)index)
			at = i;
	if (!hidden) {
		if (at >= 0)
			s_hiddenJoints[at] = s_hiddenJoints[--s_hiddenJointCount];
		return 1;
	}
	if (at >= 0)
		return 1;
	if (s_hiddenJointCount >= kMostHiddenJoints)
		return 0;
	J3DNode* node = data->getJointNodePointer((u16)index);
	if (!node || !hookJoint(node))
		return 0;
	s_hiddenJoints[s_hiddenJointCount].model = md;
	s_hiddenJoints[s_hiddenJointCount].jnt   = (u16)index;
	s_hiddenJointCount++;
	return 1;
}

// A character's worn parts left off (api->parts' hide_worn): its TNpcParts place emptied, as a
// character made without that part has it (every reader of the places skips an empty one), the part
// kept here to put back
struct LeftOff {
	TBaseNPC* npc;
	int index, lod;
	TSharedParts* part;
};
const int kMostLeftOff = 256;
LeftOff s_leftOff[kMostLeftOff];
int s_leftOffCount;

TBaseNPC* npcOf(SmsObject o)
{
	TLiveActor* a = liveActor(o);
	return a ? dynamic_cast<TBaseNPC*>(a) : 0;
}
} // namespace

extern "C" int modg_parts_hide_joint(SmsObject o, const char* name, int hidden)
{
	if (!name || !name[0])
		return 0;
	const char* c = (const char*)o;
	if (c >= s_playerParts && c < s_playerParts + 3)
		return hideJointOf(partsModel(o), name, hidden);
	TLiveActor* a = liveActor(o);
	if (!a)
		return 0;
	// every model of its own (a character's far and close ones alike), or its one model
	int done = 0;
	TMActorKeeper* keeper = a->getActorKeeper();
	if (keeper && keeper->mActors)
		for (int i = 0; i < keeper->getActorNum(); i++)
			if (MActor* m = keeper->getMActor(i))
				done |= hideJointOf(m->getModel(), name, hidden);
	if (!done)
		done = hideJointOf(partsModel(o), name, hidden);
	return done;
}

extern "C" const char* modg_parts_worn_name(SmsObject o, int index)
{
	TBaseNPC* n = npcOf(o);
	if (!n || index < 0 || index >= 12)
		return 0;
	const TNpcInitInfo* info = SMSGetNpcInitData(n->getActorType() - 0x4000001);
	if (!info || !info->unk4[index] || !info->unk4[index]->unk8[0])
		return 0;
	// only a part this one wears (or one left off by a mod)
	bool worn = n->unk168 && (n->unk168->unk0[0][index] || n->unk168->unk0[1][index]);
	for (int i = 0; i < s_leftOffCount && !worn; i++)
		worn = s_leftOff[i].npc == n && s_leftOff[i].index == index;
	return worn ? info->unk4[index]->unk8[0] : 0;
}

extern "C" int modg_parts_hide_worn(SmsObject o, int index, int hidden)
{
	TBaseNPC* n = npcOf(o);
	if (!n || !n->unk168 || index < 0 || index >= 12)
		return 0;
	TNpcParts* parts = n->unk168;
	int done         = 0;
	for (int lod = 0; lod < 2; lod++) {
		if (hidden) {
			TSharedParts* part = parts->unk0[lod][index];
			if (!part)
				continue;
			if (s_leftOffCount >= kMostLeftOff)
				return done;
			LeftOff& l = s_leftOff[s_leftOffCount++];
			l.npc      = n;
			l.index    = index;
			l.lod      = lod;
			l.part     = part;
			parts->unk0[lod][index] = 0;
			done = 1;
		} else {
			for (int i = 0; i < s_leftOffCount; i++) {
				LeftOff& l = s_leftOff[i];
				if (l.npc != n || l.index != index || l.lod != lod)
					continue;
				if (!parts->unk0[lod][index])
					parts->unk0[lod][index] = l.part;
				s_leftOff[i] = s_leftOff[--s_leftOffCount];
				done = 1;
				break;
			}
		}
	}
	return done;
}

// The stage's models and characters went with it: what was hidden of them is forgotten (the callbacks
// stay on model data still in use, doing nothing more than calling the game's own)
extern "C" void modg_parts_forget(void)
{
	s_hiddenJointCount = 0;
	s_leftOffCount     = 0;
}

// ------------------------------------------------------------------ actors' models scaled (actor->set_model_scale)

namespace {
struct Scaled {
	TLiveActor* actor;
	f32 scale;
};
const int kMostScaled = 16;
Scaled s_scaled[kMostScaled];
int s_scaledCount; // in use, the slots below it
} // namespace

// An actor's model scaled about its middle (its joints' middle), all of it alike, and its skin's
// matrices made again: as it is posed this tick
void PortActorModelScale(TLiveActor* a, f32 sc)
{
	J3DModel* md = a && a->mMActor ? a->mMActor->getModel() : 0;
	J3DModelData* data = md ? md->getModelData() : 0;
	const int n = data ? data->getJointNum() : 0;
	if (n <= 0)
		return;
	f32 cx = 0.0f, cy = 0.0f, cz = 0.0f;
	for (int j = 0; j < n; j++) {
		MtxPtr m = md->getAnmMtx(j);
		cx += m[0][3], cy += m[1][3], cz += m[2][3];
	}
	cx /= (f32)n, cy /= (f32)n, cz /= (f32)n;
	Mtx toOrigin, scale, back, about, tmp;
	MTXTrans(toOrigin, -cx, -cy, -cz);
	MTXScale(scale, sc, sc, sc);
	MTXTrans(back, cx, cy, cz);
	MTXConcat(scale, toOrigin, tmp);
	MTXConcat(back, tmp, about);
	for (int j = 0; j < n; j++) {
		Mtx out;
		MTXConcat(about, md->getAnmMtx(j), out);
		md->setAnmMtx(j, out);
	}
	if (data->getWEvlpMtxNum() > 0)
		md->calcWeightEnvelopeMtx();
}

// Every live actor's model, posed for this tick (TLiveActor::perform, patch
// zzzzzzzzzz-live-actor-posed): a mod's scale on it. Nothing to do with none asked for.
extern "C" void port_live_actor_posed(TLiveActor* a)
{
	if (s_scaledCount <= 0 || !a)
		return;
	for (int i = 0; i < s_scaledCount; i++)
		if (s_scaled[i].actor == a) {
			if (a->checkLiveFlag(LIVE_FLAG_DEAD) && !a->mMActor) // gone
				s_scaled[i].actor = 0;
			else
				PortActorModelScale(a, s_scaled[i].scale);
			return;
		}
}

extern "C" int modg_actor_set_model_scale(SmsObject o, float scale)
{
	TLiveActor* a = liveActor(o);
	if (!a)
		return 0;
	const bool none = scale > 0.999f && scale < 1.001f;
	int freeSlot = -1;
	for (int i = 0; i < kMostScaled; i++) {
		if (s_scaled[i].actor == a) {
			if (none)
				s_scaled[i].actor = 0;
			else
				s_scaled[i].scale = scale;
			return 1;
		}
		if (freeSlot < 0 && !s_scaled[i].actor)
			freeSlot = i;
	}
	if (none)
		return 1;
	if (freeSlot < 0)
		freeSlot = 0; // (the oldest asked makes way)
	s_scaled[freeSlot].actor = a;
	s_scaled[freeSlot].scale = scale > 0.001f ? scale : 0.001f;
	if (freeSlot + 1 > s_scaledCount)
		s_scaledCount = freeSlot + 1;
	return 1;
}

// a mod unloading: every model its own size again (mod_runtime.cpp)
extern "C" void modg_actor_unscale_all(void)
{
	memset(s_scaled, 0, sizeof s_scaled);
	s_scaledCount = 0;
}

// a stage's actors are gone with it (modapi_game.cpp's stage setup)
void PortWorldSparksReset();
void PortModelsDressReset(); // modapi_models.cpp
void PortWorldStageSetup()
{
	PortModelsDressReset();
	memset(s_scaled, 0, sizeof s_scaled);
	s_scaledCount = 0;
	PortWorldSparksReset();
}

// ------------------------------------------------------------------ game->wall_at, ->set_view_distance

extern "C" int modg_wall_at(const SmsVec3* at, float radius, SmsVec3* normal)
{
	if (!at || !gpMap)
		return 0;
	TBGWallCheckRecord wall(at->x, at->y, at->z, radius, 1,
	                        TBGWallCheckRecord::DONT_MOVE_XZ | TBGWallCheckRecord::IGNORE_WATER_THROUGH);
	if (!gpMap->isTouchedWallsAndMoveXZ(&wall) || !wall.mResultWalls[0])
		return 0;
	const JGeometry::TVec3<f32>& n = wall.mResultWalls[0]->getNormal();
	const f32 nl = sqrtf(n.x * n.x + n.z * n.z);
	if (nl < 1e-3f)
		return 0;
	if (normal)
		setVec(normal, n.x / nl, 0.0f, n.z / nl);
	return 1;
}

namespace {
f32 s_viewScale = 1.0f; // game->set_view_distance
bool s_waterFar;        // the water manager's drop distances set for it
} // namespace

extern "C" int modg_set_view_distance(float scale)
{
	const f32 k = scale > 1.0f ? (scale < 20.0f ? scale : 20.0f) : 1.0f;
	s_viewScale = k;
	return 1;
}

// How far a live manager's actors are seen and moved, times their own (livemanager.cpp): the
// furthest asked for; map objects as ever
f32 PortViewDistanceScale(TLiveManager* manager)
{
	if (s_viewScale <= 1.0f || dynamic_cast<TMapObjBaseManager*>(manager))
		return 1.0f;
	return s_viewScale;
}

// each tick: the water drops kept as far (modapi_game.cpp's after movement)
void PortModelsTick(); // modapi_models.cpp
void PortWorldTick()
{
	PortModelsTick(); // what mods draw is asked for again each tick
	// a drop a mod draws itself never lies on the ground or runs down a wall as water (the game would
	// draw its wet patch): told to mods as landed there (if no landing told them yet), and gone
	TModelWaterManager* w = gpModelWaterManager;
	for (int i = 0; w && i < (int)w->mParticleCount; i++) {
		const int at = w->mParticleFlagSOA[i] & 0xf;
		if (!s_dropUnseen[i] || (at != 2 && at != 3) || w->mParticleLifetimeSOA[i] <= 0.0f)
			continue;
		waterLands(i, w->unk2914[i], at == 3);
		w->mParticleLifetimeSOA[i] = 0.0f;
	}
	if (gpModelWaterManager && (s_viewScale > 1.0f || s_waterFar)) {
		gpModelWaterManager->unk5E08 = 5000.0f * s_viewScale;
		gpModelWaterManager->unk5E0C = 6000.0f * s_viewScale;
		s_waterFar                   = s_viewScale > 1.0f;
	}
}

// ------------------------------------------------------------------ audio->play_clip

extern "C" int modg_play_clip(const int16_t* pcm, int frames, int rate, float volume)
{
	if (!pcm || frames <= 0 || rate <= 0 || frames > rate * 30)
		return 0;
	// copied (the mod's memory may go): four of the port's own, each the latest asked for in turn
	static short* s_copies[4];
	static int s_sizes[4];
	static int s_next;
	const int i = s_next++ % 4;
	if (s_sizes[i] < frames) {
		free(s_copies[i]);
		s_copies[i] = (short*)malloc((size_t)frames * sizeof(short));
		s_sizes[i]  = s_copies[i] ? frames : 0;
	}
	if (!s_copies[i])
		return 0;
	memcpy(s_copies[i], pcm, (size_t)frames * sizeof(short));
	port_clip_play(s_copies[i], frames, rate, volume > 0.0f ? volume : 0.0f);
	return 1;
}

// ------------------------------------------------------------------ API 1.8: goop-like tools

extern "C" int modg_actor_set_position(SmsObject o, const SmsVec3* at)
{
	TLiveActor* a = liveActor(o);
	if (!a || !at)
		return 0;
	a->mPosition.set(at->x, at->y, at->z);
	return 1;
}

extern "C" int modg_actor_set_yaw(SmsObject o, float degrees)
{
	TLiveActor* a = liveActor(o);
	if (!a)
		return 0;
	a->mRotation.y = degrees;
	return 1;
}

extern "C" float modg_actor_gravity(SmsObject o)
{
	TLiveActor* a = liveActor(o);
	return a ? a->getGravityY() : 0.0f;
}

namespace {
// the places the shocked's sparks are bound to (they follow it while they last)
struct Spark {
	TLiveActor* a;
	JGeometry::TVec3<f32> at;
};
Spark s_sparks[32];
int s_nextSpark;
} // namespace

extern "C" int modg_actor_effect(SmsObject o, int effect)
{
	TLiveActor* a = liveActor(o);
	if (!a || !gpMarioParticleManager)
		return 0;
	if (effect == SMS_EFFECT_SMOKE) {
		gpMarioParticleManager->emit(PARTICLE_MS_MOE_FIRE_OFF, &a->mPosition, 0, nullptr);
		return 1;
	}
	if (effect == SMS_EFFECT_BURNING) { // a burning Strollin' Stu's flames
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_A, &a->mPosition, 1, a);
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_B, &a->mPosition, 1, a);
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_MOE_FIRE_D, &a->mPosition, 1, a);
		return 1;
	}
	if (effect == SMS_EFFECT_SHOCKED) { // Mario's own shock's sparks, on its middle
		Spark* sp = 0;
		for (int i = 0; i < 32 && !sp; i++)
			if (s_sparks[i].a == a)
				sp = &s_sparks[i];
		if (!sp) {
			sp    = &s_sparks[s_nextSpark++ % 32];
			sp->a = a;
		}
		const f32 size = a->mScaling.y > 0.1f ? a->mScaling.y : 1.0f;
		sp->at.set(a->mPosition.x, a->mPosition.y + 40.0f * size, a->mPosition.z);
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_A, &sp->at, 1, a);
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_C, &sp->at, 1, a);
		gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_M_BIRI_B, &sp->at, 1, a);
		return 1;
	}
	return 0;
}

extern "C" int modg_out_of_walls(const SmsVec3* at, float radius, SmsVec3* out, SmsVec3* normal)
{
	if (!at || !out)
		return 0;
	*out = *at;
	if (!gpMap)
		return 0;
	TBGWallCheckRecord wall(at->x, at->y, at->z, radius, 4, TBGWallCheckRecord::IGNORE_WATER_THROUGH);
	if (!gpMap->isTouchedWallsAndMoveXZ(&wall))
		return 0;
	setVec(out, wall.mCenter.x, at->y, wall.mCenter.z);
	if (normal) {
		setVec(normal, 0.0f, 0.0f, 0.0f);
		if (wall.mResultWalls[0]) {
			const JGeometry::TVec3<f32>& n = wall.mResultWalls[0]->getNormal();
			setVec(normal, n.x, n.y, n.z);
		}
	}
	return 1;
}

// a new stage: the sparks' actors are gone
void PortWorldSparksReset() { memset(s_sparks, 0, sizeof s_sparks); }
