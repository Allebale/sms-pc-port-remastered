// Puddles Around Fountains (port_puddles.h): the stage's arched fountains (the Pinna and Bianco
// fountain effects, TEffectPinnaFunsui and TEffectBiancoFunsui: their jets are only a particle
// effect, so they leave no water) each get one big puddle under them, as wide as where their jets
// come down: one of FLUDD's own puddles (TModelWaterManager's particle, made lying on the ground,
// state 2, very large), with its reflections, ripples and all. While Mario is near, the fountain
// keeps it topped up; once he goes, it dries in about a minute.
//
// Made lying on the ground, never flying, so it hits no one (only flying ones are checked against
// actors: TObjHitCheck::checkWater), and only on ground no actor owns (a puddle sprays what owns its
// ground) and away from goop (a puddle cleans what it lies on).
#include "port_ext.h"
#include "port_puddles.h"

#include <Enemy/Conductor.hpp>
#include <Enemy/EffectObj.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/PollutionManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

const int kMost        = 16;      // fountains in a stage
const f32 kNearMario   = 4500.0f; // only while Mario is this near
const f32 kTicksPerSec = 30.0f;
const f32 kPuddleWider   = 1.9f;  // the puddle out to where the jets end (measured on screen)
const f32 kPuddleSeconds = 60.0f; // a fountain's puddle dries this long after it is last fed

struct Fountain {
	JDrama::TActor* actor;
	f32 inner, outer;  // where its jets come down, from its middle
};
Fountain s_fountains[kMost];
int s_count;

void stageSetup()
{
	s_count = 0;
	if (!gpConductor)
		return;
	for (JGadget::TList<JDrama::TViewObj*>::iterator it = gpConductor->unk40.begin(); it != gpConductor->unk40.end(); ++it) {
		JDrama::TViewObj* o = *it;
		if (!o || s_count >= kMost)
			continue;
		if (TEffectPinnaFunsui* p = dynamic_cast<TEffectPinnaFunsui*>(o)) {
			s_fountains[s_count].actor = p;
			s_fountains[s_count].inner = 260.0f;
			s_fountains[s_count].outer = 520.0f;
			s_count++;
		} else if (TEffectBiancoFunsui* b = dynamic_cast<TEffectBiancoFunsui*>(o)) {
			s_fountains[s_count].actor = b;
			s_fountains[s_count].inner = 200.0f;
			s_fountains[s_count].outer = 450.0f;
			s_count++;
		}
	}
	fprintf(stderr, "[puddles] %d fountains in this stage\n", s_count);
}

// The fountain's one big puddle: found by where it lies and its size (it moves only as indices do,
// when the manager packs its particles), or -1.
int findPuddle(TModelWaterManager* w, const Fountain& fo, f32 size)
{
	const JGeometry::TVec3<f32>& p = fo.actor->mPosition;
	for (int i = 0; i < (int)w->mParticleCount; i++) {
		if ((w->mParticleFlagSOA[i] & 0xf) != 2 || w->mParticleAttackSOA[i] != 0 || w->mParticleSizeSOA[i] < size * 0.9f)
			continue;
		const f32 dx = w->mParticlePositionSOA[i].x - p.x, dz = w->mParticlePositionSOA[i].z - p.z;
		if (dx * dx + dz * dz < 40.0f * 40.0f)
			return i;
	}
	return -1;
}

// The fountain's big puddle made, lying on the ground under it, if the ground there is the stage's
// own, flat, not water and not goop.
void makePuddle(TModelWaterManager* w, const Fountain& fo, f32 size, f32 life)
{
	if (!gpMap || (int)w->mParticleCount >= (int)w->unk10)
		return;
	const JGeometry::TVec3<f32>& p = fo.actor->mPosition;
	const TBGCheckData* floor = 0;
	const f32 g = gpMap->checkGroundIgnoreWaterThrough(p.x, p.y + 100.0f, p.z, &floor);
	if (!floor || !floor->isLegal() || floor->isWaterSurface() || floor->isPool() || floor->mActor
	    || floor->getNormal().y < 0.7f || g < p.y - 300.0f)
		return;
	TWaterParticleType* type = w->mWaterParticleTypes[0];
	const f32 clean = type ? type->mCleanSize.get() * 32.0f : 64.0f;
	if (gpPollution) {  // no goop where it would clean it
		static const f32 kAround[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (int k = 0; k < 5; k++)
			if (gpPollution->isPolluted(p.x + kAround[k][0] * clean, g, p.z + kAround[k][1] * clean))
				return;
	}
	const int i = w->mParticleCount;
	w->mParticleLifetimeSOA[i] = life;
	w->mParticleFlagSOA[i]     = 2;  // a puddle on the ground, as a landed drop
	w->mParticleAttackSOA[i]   = 0;  // harms nothing (and marks it as the fountain's)
	w->mParticlePositionSOA[i].set(p.x, g, p.z);
	w->mParticleVelocitySOA[i].set(0.0f, 0.0f, 0.0f);
	w->mParticleSizeSOA[i] = size;
	w->mParticleTypeSOA[i] = 0;
	w->unk2914[i]          = floor;
	w->unk2514[i]          = 0;
	w->mParticleCount++;
}

void afterMovement()
{
	TModelWaterManager* w = gpModelWaterManager;
	TMario* m             = gpMarioOriginal;
	if (!s_count || !w || !m || !port_fountain_puddles())
		return;
	TWaterParticleType* type = w->mWaterParticleTypes[0];
	const f32 magnify = type ? type->mMagnify.get() : 1.0f;
	// about a minute to dry once the fountain stops topping it up (Mario gone): the puddles dry
	// Puddle Lifetime's times slower, one a tick
	const int scale = port_puddle_life_scale() > 1 ? port_puddle_life_scale() : 1;
	const f32 life  = kPuddleSeconds * kTicksPerSec / (f32)scale;
	for (int f = 0; f < s_count; f++) {
		const Fountain& fo = s_fountains[f];
		const JGeometry::TVec3<f32>& p = fo.actor->mPosition;
		const f32 dx = p.x - m->mPosition.x, dz = p.z - m->mPosition.z;
		if (dx * dx + dz * dz > kNearMario * kNearMario)
			continue;
		// as wide as where its jets come down: a landed drop of size 21 is about 85 units across
		// (seen in Erto Rock: that reached about half way to where the jets end, so twice that)
		const f32 size = 21.0f * magnify * fo.outer / 85.0f * kPuddleWider;
		const int i = findPuddle(w, fo, size);
		if (i >= 0)
			w->mParticleLifetimeSOA[i] = life;  // still fed by the fountain
		else
			makePuddle(w, fo, size, life);
	}
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name          = "fountain puddles";
		s_extension.stageSetup    = stageSetup;
		s_extension.afterMovement = afterMovement;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
