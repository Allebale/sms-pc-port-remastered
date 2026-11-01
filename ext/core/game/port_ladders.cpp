// 3D Ladders (port_puddles.h): the stage's flat ladders and climbable fences (a see-through picture
// on a flat card) get depth. As a stage is set up, its climbable collision (BG_TYPE_CLIMBABLE_FENCE)
// is matched with the map's shapes: a shape that is flat (thin one way) and lies on a fence is its
// picture. Its display lists go to the renderer with the way to the wall behind it
// (GXPC_LadderRegister), which, with 3D Ladders on, draws the card again a few times behind
// itself, a little darker each, so that its rails and rungs have sides (gx_fifo.cpp).
#include "port_ext.h"
#include "port_puddles.h"

#include <Map/Map.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/MapData.hpp>
#include <Map/MapModel.hpp>
#include <Map/JointModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" void GXPC_LadderRegister(const void* list, unsigned size, float nx, float ny, float nz);
extern "C" void GXPC_LadderClear(void);

namespace {

const f32 kThin  = 30.0f;  // a shape thinner than this one way is a flat card
const f32 kReach = 40.0f;  // how near a fence it must be
const f32 kWall  = 400.0f; // how far to look for the wall behind it

// J3DShape's bounding box (its first members, private to J3D): mMaterial, mIndex, mMtxGroupNum,
// mFlags, mRadius, mMin, mMax
struct ShapeHead {
	J3DMaterial* material;
	u16 index, groups;
	u32 flags;
	f32 radius;
	Vec lo, hi;
};

struct Fence {
	f32 lo[3], hi[3];
	f32 n[3];
};
const int kMostFences = 4096;
// in the program's own memory: allocated as a stage is set up it would be on the stage's heap, freed
// with the stage, and the next stage's objects written over (that crashed the Pokeys in Bianco Hills)
Fence s_fences[kMostFences];
int s_fenceCount;

void collectFences()
{
	s_fenceCount = 0;
	TMapCollisionData* c = gpMapCollisionData;
	if (!c || !c->unk28)
		return;
	for (u32 i = 0; i < c->unk34 && s_fenceCount < kMostFences; i++) {
		const TBGCheckData& d = c->unk28[i];
		if (!d.isFence())
			continue;
		Fence& f = s_fences[s_fenceCount++];
		const JGeometry::TVec3<f32>* p[3] = {&d.getPoint1(), &d.getPoint2(), &d.getPoint3()};
		for (int k = 0; k < 3; k++) {
			const f32 v0 = k == 0 ? p[0]->x : k == 1 ? p[0]->y : p[0]->z;
			f.lo[k] = f.hi[k] = v0;
			for (int t = 1; t < 3; t++) {
				const f32 v = k == 0 ? p[t]->x : k == 1 ? p[t]->y : p[t]->z;
				if (v < f.lo[k])
					f.lo[k] = v;
				if (v > f.hi[k])
					f.hi[k] = v;
			}
		}
		f.n[0] = d.getNormal().x, f.n[1] = d.getNormal().y, f.n[2] = d.getNormal().z;
	}
}

// The fence a flat box lies on (its normal), or none.
const Fence* fenceOf(const f32* lo, const f32* hi)
{
	for (int i = 0; i < s_fenceCount; i++) {
		const Fence& f = s_fences[i];
		bool overlap = true;
		for (int k = 0; k < 3 && overlap; k++)
			overlap = lo[k] <= f.hi[k] + kReach && hi[k] >= f.lo[k] - kReach;
		if (overlap)
			return &f;
	}
	return 0;
}

// How far the map's wall is from c along n (a big number: none within kWall).
f32 wallAlong(const f32* c, const f32* n)
{
	if (!gpMap)
		return 1e30f;
	JGeometry::TVec3<f32> a(c[0] + n[0] * 8.0f, c[1] + n[1] * 8.0f, c[2] + n[2] * 8.0f);
	JGeometry::TVec3<f32> b(c[0] + n[0] * kWall, c[1] + n[1] * kWall, c[2] + n[2] * kWall);
	JGeometry::TVec3<f32> hit;
	const TBGCheckData* d = gpMap->intersectLine(a, b, true, &hit);
	if (!d || d->isFence())
		return 1e30f;
	const f32 x = hit.x - c[0], y = hit.y - c[1], z = hit.z - c[2];
	return sqrtf(x * x + y * y + z * z);
}

int s_boxes, s_told;  // the log's lines this stage

int registerModel(J3DModel* model)
{
	J3DModelData* data = model ? model->getModelData() : 0;
	if (!data)
		return 0;
	int found = 0;
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		J3DMaterial* mat = data->getMaterialNodePointer(i);
		J3DShape* shape  = mat ? mat->getShape() : 0;
		if (!shape)
			continue;
		const ShapeHead* h = reinterpret_cast<const ShapeHead*>(shape);
		const f32 lo[3] = {h->lo.x, h->lo.y, h->lo.z}, hi[3] = {h->hi.x, h->hi.y, h->hi.z};
		if (s_boxes++ < 6)  // a few shapes' boxes, for the log (their units, against the fences')
			fprintf(stderr, "[ladders] material %d's box: (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)\n", i, lo[0], lo[1], lo[2],
			        hi[0], hi[1], hi[2]);
		f32 thinnest = 1e30f;
		for (int k = 0; k < 3; k++)
			if (hi[k] - lo[k] < thinnest)
				thinnest = hi[k] - lo[k];
		const Fence* f = fenceOf(lo, hi);
		if (!f)
			continue;
		if (!(thinnest >= 0.0f && thinnest < kThin)) {
			// near a fence but not flat: for the log (finding why a ladder is missed)
			if (s_told++ < 24)
				fprintf(stderr, "[ladders] material %d near a fence is not flat: %.0f x %.0f x %.0f at (%.0f, %.0f, %.0f)\n", i,
				        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]),
				        0.5f * (lo[2] + hi[2]));
			continue;
		}
		// back: the way to the wall behind it (the side with the nearer wall; the fence's back if none)
		const f32 c[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
		const f32 back[3] = {-f->n[0], -f->n[1], -f->n[2]};
		const f32 front[3] = {f->n[0], f->n[1], f->n[2]};
		const bool flip = wallAlong(c, front) < wallAlong(c, back);
		const f32* dir = flip ? front : back;
		for (u32 g = 0; g < shape->getMtxGroupNum(); g++) {
			J3DShapeDraw* draw = shape->getShapeDraw((u16)g);
			if (draw && draw->getDisplayList())
				GXPC_LadderRegister(draw->getDisplayList(), draw->getDisplayListSize(), dir[0], dir[1], dir[2]);
		}
		fprintf(stderr, "[ladders] material %d is a ladder or fence: %.0f x %.0f x %.0f at (%.0f, %.0f, %.0f)\n", i,
		        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], c[0], c[1], c[2]);
		found++;
	}
	return found;
}

void stageSetup()
{
	GXPC_LadderClear();
	s_boxes = s_told = 0;
	collectFences();
	int found = 0;
	if (s_fenceCount && gpMap && gpMap->getModelManager()) {
		TMapModelManager* models = gpMap->getModelManager();
		for (int i = 0; i < models->getJointModelNum(); i++)
			if (TJointModel* joint = models->getJointModel(i))
				found += registerModel(joint->getModel());
	}
	fprintf(stderr, "[ladders] %d climbable fence triangles, %d ladder and fence shapes found\n", s_fenceCount, found);
	for (int i = 0; i < s_fenceCount && i < 6; i++)  // where the fences are, for the log
		fprintf(stderr, "[ladders] a fence triangle: (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f), facing (%.2f, %.2f, %.2f)\n",
		        s_fences[i].lo[0], s_fences[i].lo[1], s_fences[i].lo[2], s_fences[i].hi[0], s_fences[i].hi[1],
		        s_fences[i].hi[2], s_fences[i].n[0], s_fences[i].n[1], s_fences[i].n[2]);
}

PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name       = "3d ladders";
		s_extension.stageSetup = stageSetup;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
