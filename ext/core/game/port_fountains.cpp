// Puddles Around Fountains (port_puddles.h): the stage's arched fountains (the Pinna and Bianco
// fountain effects, TEffectPinnaFunsui and TEffectBiancoFunsui: their jets are only a particle
// effect, so they leave no water) drop FLUDD's own water round themselves where their jets come
// down, a few drops a second while Mario is near. They land as FLUDD's do and make its puddles,
// with their reflections, ripples, lifetime and all.
//
// The drops are FLUDD's particles (TModelWaterManager), made already lying on the ground (state 2,
// a puddle), as its own become when they land: never flying, so they hit no one (only flying ones
// are checked against actors: TObjHitCheck::checkWater), and only on ground no actor owns (a puddle
// sprays what owns its ground) and away from goop (a puddle cleans what it lies on). A fountain's
// puddles once sprayed a Pokey's head in Bianco Hills and crashed the game. They take at most a third
// of FLUDD's particles, so that FLUDD always has the rest.
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
const f32 kDropsPerSec = 4.0f;    // each fountain's
const f32 kTicksPerSec = 30.0f;

struct Fountain {
	JDrama::TActor* actor;
	f32 inner, outer;  // where its jets come down, from its middle
};
Fountain s_fountains[kMost];
int s_count;
f32 s_due[kMost];
u32 s_rand = 4242u;

f32 rand01()
{
	s_rand = s_rand * 1664525u + 1013904223u;
	return (f32)(s_rand >> 8) * (1.0f / 16777216.0f);
}

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
			s_due[s_count++]           = 0.0f;
		} else if (TEffectBiancoFunsui* b = dynamic_cast<TEffectBiancoFunsui*>(o)) {
			s_fountains[s_count].actor = b;
			s_fountains[s_count].inner = 200.0f;
			s_fountains[s_count].outer = 450.0f;
			s_due[s_count++]           = 0.0f;
		}
	}
	fprintf(stderr, "[puddles] %d fountains in this stage\n", s_count);
}

// One drop of FLUDD's water low over the ground at (x, z) by the fountain at y.
void drop(TModelWaterManager* w, f32 x, f32 y, f32 z)
{
	if (!gpMap)
		return;
	const TBGCheckData* floor = 0;
	const f32 g = gpMap->checkGroundIgnoreWaterThrough(x, y + 200.0f, z, &floor);
	if (!floor || !floor->isLegal() || floor->isWaterSurface() || floor->isPool() || floor->mActor
	    || floor->getNormal().y < 0.7f || g < y - 300.0f || g > y + 200.0f)
		return;  // no flat ground of the stage's own there near the fountain's height, or water
	TWaterParticleType* type = w->mWaterParticleTypes[0];
	const f32 clean = type ? type->mCleanSize.get() * 32.0f : 64.0f;
	if (gpPollution) {  // no goop where it would clean it
		static const f32 kAround[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (int k = 0; k < 5; k++)
			if (gpPollution->isPolluted(x + kAround[k][0] * clean, g, z + kAround[k][1] * clean))
				return;
	}
	const int i = w->mParticleCount;
	w->mParticleLifetimeSOA[i] = 255.0f;  // as emitRequest's default (Puddle Lifetime scales its drying)
	w->mParticleFlagSOA[i]     = 2;       // a puddle on the ground, as a landed drop
	w->mParticleAttackSOA[i]   = 0;       // harms nothing
	w->mParticlePositionSOA[i].set(x, g, z);
	w->mParticleVelocitySOA[i].set(0.0f, 0.0f, 0.0f);
	// as large as a landed drop (a landing grows it by its type's magnify)
	w->mParticleSizeSOA[i] = (17.0f + 8.0f * rand01()) * (type ? type->mMagnify.get() : 1.0f);
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
	const int budget = (int)w->unk10 / 3;
	for (int f = 0; f < s_count; f++) {
		const Fountain& fo = s_fountains[f];
		const JGeometry::TVec3<f32>& p = fo.actor->mPosition;
		const f32 dx = p.x - m->mPosition.x, dz = p.z - m->mPosition.z;
		if (dx * dx + dz * dz > kNearMario * kNearMario)
			continue;
		s_due[f] += kDropsPerSec / kTicksPerSec;
		for (; s_due[f] >= 1.0f; s_due[f] -= 1.0f) {
			if ((int)w->mParticleCount >= budget || (int)w->mParticleCount >= (int)w->unk10)
				break;
			const f32 a = rand01() * 6.2831853f;
			const f32 r = fo.inner + (fo.outer - fo.inner) * rand01();
			drop(w, p.x + sinf(a) * r, p.y, p.z + cosf(a) * r);
		}
		if (s_due[f] > 4.0f)
			s_due[f] = 4.0f;
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
