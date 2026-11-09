// 3D Grass: blades of grass on the stages' grass (ext/core/game/port_grass.cpp), as each place
// would have it: cut short where the Piantas look after it (Delfino Plaza, Pinna Park), wild where
// nobody does (Bianco Hills, Pianta Village...). They sway in the wind, bend away from Mario as he
// goes through them and lie flat under FLUDD's puddles. Off, the game is as it was.
#ifndef PORT_GRASS_H
#define PORT_GRASS_H

#ifdef __cplusplus
extern "C" {
#endif

// The setting (the pause menu's Settings, Graphics): 0 off, 1 by location, 2 always cut, 3 always
// wild.
int port_grass(void);
// Grass Distance: how far from Mario the blades go, times each kind's own reach (Low 1 to Max 12).
float port_grass_distance(void);
// Cut Grass Height and Wild Grass Height: times the kind's own (wild: 1; cut and tended: 0).
float port_grass_height(int wild);
// Grass Flattening: how wide and how long FLUDD's spray (and Mario, as Mario Grass Flattening allows) press the blades flat: 0 low, 1 medium,
// 2 high, 3 very high, 4 insane.
int port_grass_flatten(void);
// Mario Grass Flattening: 0 where he lands, dives, slides or skids, 1 always (each step too),
// 2 never.
int port_grass_mario(void);
// From port_draw_footprints (TModelWaterManager::perform, as the stage's ground is drawn): the
// blades near Mario. view: the camera's (world to view).
void port_grass_draw(float (*view)[4]);

#ifdef __cplusplus
}
#endif

#endif
