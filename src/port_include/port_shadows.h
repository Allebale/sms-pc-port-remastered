// Real Shadows: the sun's shadows of everything (the level, characters, objects), drawn by the
// renderer from a shadow map (platform/gx, GXPC_ShadowFrame). Off, the game is as it was.
#ifndef PORT_SHADOWS_H
#define PORT_SHADOWS_H

#ifdef __cplusplus
extern "C" {
#endif

// The setting (the pause menu's Settings, Graphics): 0 off, 1 soft, 2 strong.
int port_shadows(void);
// With Real Shadows on: 1 the game's own round shadows under characters are kept, lighter, so that
// they stand on the ground (its trees', buildings' and boats' are left out: zzzzz-ext-31-shadows-
// old-off); 0 none of the game's own.
int port_shadows_round(void);
// Shadow Distance: how far the far map reaches either side of Mario, in units.
float port_shadows_distance(void);
// Keep the Original Shadow Direction: 1 the sun's shadows fall exactly where the game's own fell
// (straight below what casts them, as TMBindShadowManager puts them); 0 from the stage's sun.
int port_shadows_original_direction(void);
// From CPolarSubCamera::perform (zzzzz-ext-31-shadows), the main camera's projection just set:
// the sun's direction and the box round Mario for this frame, with the camera's view (world to
// view). camera: the camera (only the game's own, gpCamera, counts).
void port_shadows_main_view(void* camera, float (*view)[4]);
// Fire Lights (ext/core/game/port_lights.cpp): the fires' lights seen from a camera whose
// projection was just set (the main camera's, from port_shadows_main_view; a mirror's, from
// port_mirror_clip), lit in what it draws.
void port_lights_view(float (*view)[4]);
// Ambient Occlusion (platform/misc/port_puddle_settings.cpp): 0 off, 1 fine detail, 2 large
// shapes, 3 both; its strength; its reach from the camera, in units; Depth Buffer View (0 off,
// 1 depth, 2 faces, 3 both, 4 the shade alone).
int port_ao(void);
float port_ao_strength(void);
float port_ao_distance(void);
int port_depth_view(void);
// Ambient Occlusion Quality: 0 low, 1 medium, 2 high (how finely, and from how many samples)
int port_ao_quality(void);
// Ambient Occlusion Update Rate: 0 every frame, 1 every other frame
int port_ao_half_rate(void);
// Mario Ambient Occlusion: how much of his own shade Mario keeps, 0 (at first: none, no halo) to 1
float port_ao_mario(void);
// AO Near Grass: 1 takes the shade off the ground among and around 3D Grass (characters keep theirs)
int port_ao_grass(void);
// The renderer's: armed for the main camera's view this frame (applied as its 3D scene ends).
void GXPC_AOFrame(int mode, float strength, float distance, int view, int quality, int halfRate);
// Ambient Occlusion: Mario's place this frame, in the main camera's view (from his feet to over his
// cap, and how far round), so that he casts no shade round him (null or 0: none)
void GXPC_AOMario(const float* bottom, const float* top, float radius, float keep);
// Ambient Occlusion: AO Near Grass (1: no shade on the ground under and round 3D Grass's blades)
void GXPC_AOGrass(int on);
// Sandy Mario (ext/core/game/port_sandy.cpp; the setting in platform/misc/port_puddle_settings.cpp:
// 0 off, 1 light, 2 normal, 3 heavy): around Mario's own draw (port_wet_mario_draw), the sand on him.
int port_sandy_mario(void);
#ifdef __cplusplus
}
void port_sandy_mario_draw(int begin, float (*view)[4]);
extern "C" {
#endif

// The renderer's (platform/gx/src/gx_render.cpp, gx_api.cpp).
// half: the near map's reach either side of focus; reach: the far one's.
void GXPC_ShadowFrame(const float view[3][4], const float sun[3], const float focus[3], float strength,
                      float half, float reach);
void GXPC_ShadowNotMain(void);
// Round Shadows Under Characters (with Real Shadows): a character's or enemy's display list (its
// model's, registered as the stage is set up), and whether they and Mario cast no sun shadow now
void GXPC_ShadowCharacter(const void* list, unsigned size);
void GXPC_ShadowRound(int on);
// Soft Shadow Edges: how wide a shadow's soft edge is, in units (0 sharp)
void GXPC_ShadowSoftness(float units);
float port_shadows_softness(void);
// Around Mario's own draw (port_wet_mario_draw): for the log, whether his batches cast.
void GXPC_ShadowTag(int on);

#ifdef __cplusplus
}
#endif

#endif
