// The mod API's game objects by what they are (API 1.8): actor->kind, ->owner, ->parts, the water
// meters (how much water a boss, a character or a hiding picture still takes: ->water_meter,
// ->set_water_meter, ->set_water_wait) and ->knock (balls and fruit). General tools for any
// mod: none of it does anything till a mod asks.
#include "../modapi_bridge.h"

#include <Enemy/BossEel.hpp>
#include <Enemy/BossManta.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/BossWanwan.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Emario.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/EnemyMario.hpp>
#include <Enemy/GateKeeper.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <M3DUtil/MActor.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <NPC/NpcBase.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>

#include <math.h>
#include <string.h>

namespace {

SmsObject obj(JDrama::TNameRef* p) { return (SmsObject)p; }
THitActor* hitOf(SmsObject o) { return o ? dynamic_cast<THitActor*>((JDrama::TNameRef*)o) : 0; }
void setVec(SmsVec3* v, f32 x, f32 y, f32 z) { v->x = x, v->y = y, v->z = z; }

// a Phantamanta's wing (its extra hit area keeps no owner): the live manta nearest it
TBossManta* mantaNear(const JGeometry::TVec3<f32>& at)
{
	if (!gpConductor)
		return 0;
	TBossManta* best = 0;
	f32 closest      = 1e12f;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++)
			if (TBossManta* m = dynamic_cast<TBossManta*>((*it)->getObj(i))) {
				const f32 x = m->mPosition.x - at.x, z = m->mPosition.z - at.z;
				if (x * x + z * z < closest && !m->checkLiveFlag(LIVE_FLAG_DEAD))
					closest = x * x + z * z, best = m;
			}
	return best;
}

// the boss or actor a part belongs to (itself if none)
THitActor* ownerOf(THitActor* a)
{
	if (TBWHit* h = dynamic_cast<TBWHit*>(a))
		return h->mOwner;
	if (TBPHeadHit* h = dynamic_cast<TBPHeadHit*>(a))
		return h->mOwner;
	if (TGKHitObj* h = dynamic_cast<TGKHitObj*>(a))
		return h->mOwner;
	if (TBossEelTooth* t = dynamic_cast<TBossEelTooth*>(a))
		return t->mOwner;
	if (TBossMantaAdditionalCollision* c = dynamic_cast<TBossMantaAdditionalCollision*>(a)) {
		TBossManta* m = mantaNear(c->mPosition);
		return m ? (THitActor*)m : a;
	}
	return a;
}

int kindOf(THitActor* a)
{
	if (!a)
		return SMS_KIND_OTHER;
	if (dynamic_cast<TBossEelTooth*>(a))
		return SMS_KIND_EELY_MOUTH_TOOTH;
	THitActor* o = ownerOf(a);
	if (dynamic_cast<TEMario*>(o))
		return SMS_KIND_SHADOW_MARIO;
	if (dynamic_cast<TBossWanwan*>(o))
		return SMS_KIND_BIG_CHAIN_CHOMP;
	if (dynamic_cast<TBossManta*>(o))
		return SMS_KIND_PHANTAMANTA;
	if (dynamic_cast<TBossEel*>(o))
		return SMS_KIND_EELY_MOUTH;
	if (dynamic_cast<TBiancoGateKeeper*>(o))
		return SMS_KIND_PROTO_PIRANHA;
	if (dynamic_cast<TBossPakkun*>(o))
		return SMS_KIND_PETEY_PIRANHA;
	if (dynamic_cast<TWaterHitPictureHideObj*>(a))
		return SMS_KIND_HIDING_PICTURE;
	if (dynamic_cast<TMapObjBall*>(a))
		return SMS_KIND_BALL;
	if (dynamic_cast<TBaseNPC*>(a))
		return SMS_KIND_CHARACTER;
	if (dynamic_cast<TSmallEnemy*>(a))
		return SMS_KIND_SMALL_ENEMY;
	return SMS_KIND_OTHER;
}

f32 radiusOf(THitActor* a)
{
	const f32 r = a->getAttackRadius(), d = a->getDamageRadius();
	return r > d ? r : d;
}

// Proto Piranha's open mouth, its goop pile
bool gkMouth(TBiancoGateKeeper* g) { return g->mHead && g->mHead->mVulnerable && !g->checkLiveFlag(LIVE_FLAG_DEAD); }
bool gkPile(TBiancoGateKeeper* g) { return g->mVulnerable && !g->checkLiveFlag(LIVE_FLAG_DEAD); }

// a Phantamanta's hits by its generation (16, 8, 4, 2, 1)
const int kMantaHits[6] = {16, 8, 4, 2, 1, 1};
int mantaMost(TBossManta* m) { return kMantaHits[m->mGeneration >= 0 && m->mGeneration < 6 ? m->mGeneration : 0]; }

// a hiding picture's washing: in ticks of FLUDD's stream
f32 pictureLeft(TWaterHitPictureHideObj* p)
{
	const f32 speed = p->mSprayProgressSpeed > 0.001f ? p->mSprayProgressSpeed : 1.0f;
	return (p->mReverse ? p->mProgress - p->mMinProgress : p->mMaxProgress - p->mProgress) / speed;
}

// TMapObjGeneral's states a ball is not knocked in
bool ballBusy(TMapObjBall* b)
{
	const int st = b->mState;
	if (st == TMapObjGeneral::STATE_HOLDING || st == TMapObjGeneral::STATE_APPEARING || st == TMapObjGeneral::STATE_BREAKING
	    || st == TMapObjGeneral::STATE_WAITING_TO_APPEAR || st == TMapObjGeneral::STATE_BURIED
	    || st == TMapObjGeneral::STATE_SINKING)
		return true;
	return dynamic_cast<TResetFruit*>(b) && (st == TMapObjGeneral::STATE_ROTTING || st == TMapObjGeneral::STATE_BROKEN);
}

} // namespace

extern "C" int modg_actor_kind(SmsObject o) { return kindOf(hitOf(o)); }

extern "C" SmsObject modg_actor_owner(SmsObject o)
{
	THitActor* a = hitOf(o);
	return a ? obj(ownerOf(a)) : o;
}

extern "C" int modg_actor_parts(SmsObject o, SmsObject* out, int most)
{
	THitActor* a = hitOf(o);
	int n        = 0;
	if (TBossEel* e = dynamic_cast<TBossEel*>(a)) {
		for (int i = 0; i < 8; i++)
			if (e->mTeeth[i]) {
				if (out && n < most)
					out[n] = obj(e->mTeeth[i]);
				n++;
			}
	} else if (TBossPakkun* p = dynamic_cast<TBossPakkun*>(a)) {
		if (p->mHeadHit) {
			if (out && most > 0)
				out[0] = obj(p->mHeadHit);
			n = 1;
		}
	} else if (TBiancoGateKeeper* g = dynamic_cast<TBiancoGateKeeper*>(a)) {
		if (g->mHead) {
			if (out && most > 0)
				out[0] = obj(g->mHead);
			n = 1;
		}
	}
	return n;
}

extern "C" int modg_actor_water_meter(SmsObject o, SmsWaterMeter* out)
{
	THitActor* part = hitOf(o);
	if (!part || !out)
		return 0;
	THitActor* a = ownerOf(part);
	SmsWaterMeter m;
	memset(&m, 0, sizeof m);
	setVec(&m.spot, a->mPosition.x, a->mPosition.y, a->mPosition.z);
	m.spot_radius = radiusOf(a);
	bool has      = true;
	if (TEMario* em = dynamic_cast<TEMario*>(a)) {
		TEnemyMario* e = em->mEnemyMario;
		if (!e)
			return 0;
		const u16 doing = e->mEMDoing;
		m.most = e->getSettingsParams()->mWaterCtMax.get();
		m.left = e->mWaterCounter;
		m.open = !e->getSettingsParams()->mInvincibleFlag.get() && !em->checkLiveFlag(LIVE_FLAG_DEAD)
		         && (doing == TEnemyMario::EM_DOING_REPLAY || doing == TEnemyMario::EM_DOING_REPLAY_WAITING
		             || doing == TEnemyMario::EM_DOING_REPLAY_JUMP_TO_NEAREST_NODE);
		setVec(&m.spot, e->mPosition.x, e->mPosition.y + 80.0f, e->mPosition.z);
		m.spot_radius = 80.0f;
	} else if (TBossWanwan* c = dynamic_cast<TBossWanwan*>(a)) {
		m.most = c->getSaveParam2()->mSLBWHitPointMax.get();
		m.left = c->getHitPoints();
		m.open = !c->mIsInBath && m.left > 0;
		m.spot.y += 150.0f;
		m.spot_radius = 250.0f;
	} else if (TBossManta* mt = dynamic_cast<TBossManta*>(a)) {
		m.most = mantaMost(mt);
		m.left = m.most - mt->unk19C > 0 ? m.most - mt->unk19C : 0;
		m.open = mt->isDamageable() && !mt->checkLiveFlag(LIVE_FLAG_DEAD);
		m.wait = mt->unk1A0;
		m.spot_radius = 300.0f * mt->mScaling.x;
	} else if (TBossEelTooth* t = dynamic_cast<TBossEelTooth*>(part)) {
		if (!t->mOwner)
			return 0;
		m.most = t->mOwner->getBossEelParams().mSLToothMaxHitPoint.get() - 1;
		m.left = t->mHitPoints - 1 > 0 ? t->mHitPoints - 1 : 0;
		m.wait = t->mDamageCooldown;
		m.open = !t->mOwner->mToothBroken && m.left > 0 && m.wait == 0;
		setVec(&m.spot, t->mPosition.x, t->mPosition.y, t->mPosition.z);
		m.spot_radius = 150.0f;
	} else if (TBiancoGateKeeper* g = dynamic_cast<TBiancoGateKeeper*>(a)) {
		if (gkMouth(g)) { // awake, its mouth open: water in it hurts it
			m.most = m.left = 1;
			m.open = 1;
			setVec(&m.spot, g->mHead->mPosition.x, g->mHead->mPosition.y, g->mHead->mPosition.z);
			const f32 r   = radiusOf(g->mHead);
			m.spot_radius = r > 40.0f ? r : 40.0f;
		} else { // asleep in its goop pile: water wakes it
			m.most = 255;
			m.left = 255 - g->unk17C;
			m.open = gkPile(g);
		}
	} else if (TBossPakkun* p = dynamic_cast<TBossPakkun*>(a)) {
		m.most = p->getSaveParam2()->mSLWaterMarkLimit.get();
		m.left = m.most - p->mWaterMark > 0 ? m.most - p->mWaterMark : 0;
		m.open = p->mState == BOSSPAKU_STATE_UNK2 && !p->checkLiveFlag(LIVE_FLAG_DEAD); // his mouth open
		if (p->mHeadHit) {
			setVec(&m.spot, p->mHeadHit->mPosition.x, p->mHeadHit->mPosition.y, p->mHeadHit->mPosition.z);
			m.spot_radius = radiusOf(p->mHeadHit);
		}
	} else if (TBaseNPC* n = dynamic_cast<TBaseNPC*>(a)) {
		if (!n->isPollutionNpc())
			return 0;
		m.most = 1000;
		m.left = (int)(n->mPollutionAmount * 1000.0f + 0.999f);
		m.left = m.left > 0 ? m.left : 0;
		m.open = m.left > 0 && gpMarDirector && !gpMarDirector->isTalkOrDemoModeNow();
		m.spot.y += 80.0f;
	} else if (TWaterHitPictureHideObj* pic = dynamic_cast<TWaterHitPictureHideObj*>(a)) {
		const f32 speed = pic->mSprayProgressSpeed > 0.001f ? pic->mSprayProgressSpeed : 1.0f;
		m.most = (int)((pic->mMaxProgress - pic->mMinProgress) / speed + 0.999f);
		m.left = pic->mState == TWaterHitPictureHideObj::STATE_FINISHED ? 0 : (int)(pictureLeft(pic) + 0.999f);
		m.open = m.left > 0;
	} else
		has = false;
	if (!has)
		return 0;
	const uint32_t size = out->size && out->size < sizeof m ? out->size : (uint32_t)sizeof m;
	m.size = size;
	memcpy(out, &m, size);
	return 1;
}

extern "C" int modg_actor_set_water_meter(SmsObject o, int left, int effects)
{
	THitActor* part = hitOf(o);
	if (!part)
		return 0;
	THitActor* a = ownerOf(part);
	left         = left > 0 ? left : 0;
	if (TEMario* em = dynamic_cast<TEMario*>(a)) {
		TEnemyMario* e = em->mEnemyMario;
		if (!e)
			return 0;
		const int most   = e->getSettingsParams()->mWaterCtMax.get();
		e->mWaterCounter = (s16)(left < most ? left : most);
		if (left == 0)
			e->hitWater(em); // empty: down now
		return 1;
	}
	if (TBossWanwan* c = dynamic_cast<TBossWanwan*>(a)) {
		const int most = c->getSaveParam2()->mSLBWHitPointMax.get();
		const int was  = c->getHitPoints();
		left           = left < most ? left : most;
		c->mHitPoints  = (u8)left;
		if (effects && left < was) { // as the game's own hit
			c->mSparkRequest += 1;
			if (left == 0) {
				gpMarioParticleManager->emitAndBindToMtxPtr(BWANWAN_JPA_MS_BWAN_DOWNYUGE, c->getModel()->getAnmMtx(1), 0, nullptr);
				SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_TO_COOL, &c->mPosition);
			} else
				SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_COOL, &c->mPosition);
		}
		return 1;
	}
	if (TBossManta* mt = dynamic_cast<TBossManta*>(a)) {
		if (mt->checkLiveFlag(LIVE_FLAG_DEAD))
			return 0;
		const int most = mantaMost(mt);
		if (left == 0) { // its last hit, the game's own (it splits or is gone)
			if (!mt->isDamageable())
				return 0;
			mt->unk19C = most - 1;
			if (effects)
				gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &mt->mPosition, 0, nullptr);
			mt->collidedWithWater();
			return 1;
		}
		mt->unk19C = most - (left < most ? left : most);
		return 1;
	}
	if (TBossEelTooth* t = dynamic_cast<TBossEelTooth*>(part)) {
		if (!t->mOwner || t->mOwner->mToothBroken)
			return 0;
		const int most = t->mOwner->getBossEelParams().mSLToothMaxHitPoint.get() - 1;
		left           = left < most ? left : most;
		if (effects && t->mDamageCooldown == 0 && left < t->mHitPoints - 1) {
			t->mHitPoints = left + 2; // the game's own hit takes the last (its sound, its shine, a bad one off)
			t->receiveMessage(0, HIT_MESSAGE_SPRAYED_BY_WATER);
		} else
			t->mHitPoints = left + 1;
		return 1;
	}
	if (TBiancoGateKeeper* g = dynamic_cast<TBiancoGateKeeper*>(a)) {
		if (gkMouth(g)) {
			if (left > 0)
				return 1;
			g->unk154++; // water in its mouth
			if (effects)
				gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &g->mHead->mPosition, 0, nullptr);
			return 1;
		}
		if (!gkPile(g))
			return 0;
		g->unk154++;
		g->unk17E = 0x10; // (wet: the game keeps counting towards waking it)
		const int woke = 255 - (left < 255 ? left : 255);
		if (woke > g->unk17C)
			g->unk17C = (u8)woke;
		if (effects)
			gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &g->mPosition, 0, nullptr);
		return 1;
	}
	if (TBossPakkun* p = dynamic_cast<TBossPakkun*>(a)) {
		const int limit = p->getSaveParam2()->mSLWaterMarkLimit.get();
		const int mark  = limit - (left < limit ? left : limit);
		if (effects && mark > p->mWaterMark && p->mState == BOSSPAKU_STATE_UNK2)
			p->gotWaterDamage(); // as a drop in his mouth
		p->mWaterMark = mark;
		return 1;
	}
	if (TBaseNPC* n = dynamic_cast<TBaseNPC*>(a)) {
		if (!n->isPollutionNpc())
			return 0;
		const bool was      = n->mPollutionAmount > 0.0f;
		n->mPollutionAmount = (f32)left / 1000.0f;
		if (left == 0 && was) { // clean: as the game's own (it thanks Mario)
			n->mTalkForbidCount = 60;
			n->onUnk1DA(TBaseNPC::UNK1DA_FLAG_UNK2);
		}
		return 1;
	}
	if (TWaterHitPictureHideObj* pic = dynamic_cast<TWaterHitPictureHideObj*>(a)) {
		if (pic->mState == TWaterHitPictureHideObj::STATE_FINISHED)
			return left == 0;
		const f32 speed = pic->mSprayProgressSpeed > 0.001f ? pic->mSprayProgressSpeed : 1.0f;
		const f32 by    = left == 0 ? 100000.0f : (pictureLeft(pic) - (f32)left) * speed;
		if (by > 0.0f)
			pic->forward(by);
		return 1;
	}
	return 0;
}

extern "C" int modg_actor_set_water_wait(SmsObject o, int ticks)
{
	THitActor* part = hitOf(o);
	if (!part)
		return 0;
	ticks = ticks > 0 ? ticks : 0;
	if (TBossEelTooth* t = dynamic_cast<TBossEelTooth*>(part)) {
		t->mDamageCooldown = ticks;
		return 1;
	}
	if (TBossManta* m = dynamic_cast<TBossManta*>(ownerOf(part))) {
		m->unk1A0 = ticks;
		return 1;
	}
	return 0;
}

extern "C" int modg_actor_knock(SmsObject o, const SmsVec3* push)
{
	THitActor* a = hitOf(o);
	TLiveActor* l = dynamic_cast<TLiveActor*>(a);
	if (!l || !push || l->checkLiveFlag(LIVE_FLAG_DEAD) || l->checkLiveFlag(LIVE_FLAG_HIDDEN))
		return 0;
	if (TMapObjBall* b = dynamic_cast<TMapObjBall*>(l)) {
		if (ballBusy(b))
			return 0;
		b->mVelocity.x += push->x;
		b->mVelocity.z += push->z;
		b->mVelocity.y = push->y;
		b->mPosition.y += 6.0f;
		b->offLiveFlag(LIVE_FLAG_UNK10); // moving again (as kicked)
		if (TResetFruit* fruit = dynamic_cast<TResetFruit*>(b))
			fruit->makeObjLiving(); // off its tree or its spot, as a kicked one
		return 1;
	}
	return 0;
}
