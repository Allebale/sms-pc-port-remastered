// The grab log: as Mario takes hold of something (a fence or ladder, a bar, a wire, a ceiling), the
// log says what: his status, the wall in front of him (its collision type: 0x10A a climbable fence)
// and, from the renderer (GXPC_GrabProbe), the draws in the next frame near his hands with their
// textures, as the texture pack names them. So a ladder's or fence's look can be known by its
// texture, for 3D Ladders. Only the log; nothing changes in the game.
#include "port_ext.h"
#include "port_puddles.h"

#include <Map/MapData.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>

#include <stdio.h>

extern "C" void GXPC_GrabProbe(const float* at, float radius);

namespace {
// what he holds on to: bars and wires (0x10000000), fences and ladders (0x20000000), ceilings (0x200000)
const u32 kHolding = 0x10000000 | 0x20000000 | 0x200000;
u32 s_was;
} // namespace

// Once a frame, from the main camera's view (port_shadows_main_view).
void port_grab_view(float (*view)[4])
{
	TMario* m = gpMarioOriginal;
	if (!m || !view)
		return;
	const u32 st      = m->mStatus;
	const bool holds  = (st & kHolding) != 0;
	const bool before = (s_was & kHolding) != 0;
	s_was             = st;
	if (!holds || before)
		return;
	const TBGCheckData* wall = m->getWallPlane();
	fprintf(stderr, "[grab] Mario takes hold: status 0x%X at (%.0f, %.0f, %.0f); the wall before him: %s%X%s\n", st,
	        m->mPosition.x, m->mPosition.y, m->mPosition.z, wall ? "type 0x" : "none", wall ? (unsigned)wall->mBGType : 0u,
	        wall && wall->isFence() ? " (a climbable fence)" : "");
	// his hands: over his head while climbing, about 130 units up
	const f32 p[3] = {m->mPosition.x, m->mPosition.y + 130.0f, m->mPosition.z};
	float at[3];
	for (int r = 0; r < 3; r++)
		at[r] = view[r][0] * p[0] + view[r][1] * p[1] + view[r][2] * p[2] + view[r][3];
	GXPC_GrabProbe(at, 400.0f);  // a draw with a corner this near (a ladder card's are at its ends)
}
