// A Dango's head (port_dango_head.h). A Dango stack (TBossDangoHamuKuri and the TDangoHamuKuri it
// carries, each held on the one below: mPrev under it, mNext over it) draws every ball with its
// manager's default.bmd. When that model's folder also has a head.bmd, each ball gets a second
// MActor made from it, and the ball on top of a stack is drawn with that one: the same skeleton,
// so the same animation (its index and frame) carries over at the swap. Knock the top ball off
// and the next one down becomes the head.
//
// Only a Dango without Strollin' Stus in its stage reads its own folder: with them it shares the
// Stu's model (TDangoHamuKuriManager::createModelDataArray), and the Stu's folder has no head.bmd.
#include "port_ext.h"
#include "port_dango_head.h"

#include <Enemy/HamuKuri.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <M3DUtil/MActor.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <Strategic/ObjModel.hpp>
#include <System/MarDirector.hpp>

#include <stdio.h>

namespace {

const int kMost = 64;   // Dango balls in a stage

struct Ball {
	TDangoHamuKuri* dango;
	MActor* body;
	MActor* head;
};

Ball gBalls[kMost];
int gCount = 0;
void* gStage = 0;   // the director the list belongs to: a new stage starts it again

Ball* find(TDangoHamuKuri* dango)
{
	for (int i = 0; i < gCount; ++i)
		if (gBalls[i].dango == dango)
			return &gBalls[i];
	return 0;
}

void swapTo(TDangoHamuKuri* dango, MActor* from, MActor* to)
{
	J3DFrameCtrl* was = from->getFrameCtrl(ANM_TYPE_BCK);
	f32 frame         = was ? was->getFrame() : 0.0f;
	dango->mMActor    = to;
	to->setLightType(LIGHT_TYPE_OBJECT);
	to->setBckFromIndex(dango->mCurrentBckAnm);
	J3DFrameCtrl* now = to->getFrameCtrl(ANM_TYPE_BCK);
	if (now)
		now->setFrame(frame);
}

} // namespace

void port_dango_head_init(TDangoHamuKuri* dango)
{
	void* stage = (void*)gpMarDirector;
	if (stage != gStage) {
		gStage = stage;
		gCount = 0;
	}
	if (gCount >= kMost || !dango->mManager)
		return;
	TModelDataKeeper* models = dango->mManager->getModelDataKeeper();
	if (!models || !models->getFolder())
		return;
	char path[256];
	snprintf(path, sizeof(path), "%s/head.bmd", models->getFolder());
	if (!JKRGetResource(path))
		return;
	TMActorKeeper* keeper = new TMActorKeeper(dango->mManager, 1);
	MActor* head          = keeper->createMActor("head.bmd", 3);
	if (!head)
		return;
	head->setLightType(LIGHT_TYPE_OBJECT);
	Ball ball = { dango, dango->mMActor, head };
	gBalls[gCount++] = ball;
	static bool told = false;
	if (!told) {
		told = true;
		fprintf(stderr, "[dango] %s has a head model: the top ball of each stack wears it\n", models->getFolder());
	}
}

void port_dango_head_update(TDangoHamuKuri* dango)
{
	if (!gCount || (void*)gpMarDirector != gStage)
		return;
	Ball* ball = find(dango);
	if (!ball)
		return;
	// on top of a stack: something under it and nothing over it
	bool top     = dango->mPrev && !dango->mNext;
	MActor* want = top ? ball->head : ball->body;
	if (dango->mMActor != want)
		swapTo(dango, dango->mMActor, want);
}
