#ifndef PORT_PUDDLES_H
#define PORT_PUDDLES_H

// The port's puddle and water effects. A mod sets them through the mod API's api->reflections
// (SMS_REFLECT_* in the SDK's sms_mod.h); platform/misc/port_puddle_options.cpp keeps them, each
// as the original game has it until a mod sets it. The game's water (TModelWaterManager,
// zzzzz-ext-24-puddle-lifetime), its mirror and the port's reflection code read them. Plain C.

#ifdef __cplusplus
extern "C" {
#endif

// The options, in SMS_REFLECT_*'s order and units.
enum {
	PORT_OPT_LIFETIME,
	PORT_OPT_PUDDLES,
	PORT_OPT_DISTANCE,
	PORT_OPT_WATER,
	PORT_OPT_BEAT_MIRRORS,
	PORT_OPT_LEVEL,
	PORT_OPT_VIEW_ANGLE,
	PORT_OPT_PUDDLE_RIPPLES,
	PORT_OPT_WATER_RIPPLES,
	PORT_OPT_SUN_GLINTS,
	PORT_OPT_WATER_DEPTH,
	PORT_OPT_BLUR,
	PORT_OPT_FOOTPRINTS,
	PORT_OPT_SHOW,                   // PORT_REFLECT_KINDS of them
	PORT_OPT_BY_SURFACE = PORT_OPT_SHOW + 5,
	PORT_OPT_STRENGTH,               // PORT_PUDDLE_SURFACES of them
	PORT_OPT_WET_MARIO = PORT_OPT_STRENGTH + 10,
	PORT_REFLECT_OPTIONS
};
// Set (clamped to its range; 0 for no such option), get (-1 for none), its range.
int port_reflect_option_set(int option, int value);
int port_reflect_option_get(int option);
int port_reflect_option_range(int option, int* min, int* max, int* original);
// Every option back as the original game has it (the mod that set them unloaded).
void port_reflect_option_reset(void);

// 1 (Original) to 8.
int port_puddle_life_scale(void);
// Puddle Reflections: 1 when characters and objects are reflected in the puddles, not only
// Mario (ext/core/game/port_reflections.cpp). Their reflections are made as an area loads.
int port_puddle_reflections(void);
// Reflections by Surface: 1 when how strongly a puddle reflects follows the floor under it
// (sand less, smooth floors fully); 0 every puddle as strongly, as the game has it.
int port_puddle_surface(void);
// The surfaces Reflections by Surface tells apart (ext/core/game/port_reflections.cpp sorts a
// floor into one by its type and sound material), each with its strength on the settings menu's
// Reflections page.
enum {
	PORT_PUDDLE_STONE,  // smooth stone: the plaza's tiles, its carpets, and every floor not below
	PORT_PUDDLE_METAL,
	PORT_PUDDLE_CLAY,   // clay roof tiles
	PORT_PUDDLE_WOOD,
	PORT_PUDDLE_SAND,
	PORT_PUDDLE_GRASS,
	PORT_PUDDLE_CANVAS,
	PORT_PUDDLE_WET,    // wet and slippery floors
	PORT_PUDDLE_WATER,      // Water Reflections: the sea, lakes, pools, fountains and streams
	PORT_PUDDLE_WET_GROUND, // Water Reflections: wet ground itself, puddle or not
	PORT_PUDDLE_SURFACES
};
// How strongly a puddle on that surface reflects, in tenths of a percent of the game's own
// (where every puddle is full): 75 is 7.5%.
int port_puddle_surface_strength(int surface);
// How strongly a puddle on a floor of that type (mBGType) and sound material (its triangle's
// unk6) reflects, 0 to 255 (ext/core/game/port_reflections.cpp).
unsigned port_puddle_surface_alpha(unsigned type, unsigned material);

// The surface (PORT_PUDDLE_*) a floor of that type and sound material is (port_reflections.cpp).
int port_puddle_surface_kind(unsigned type, unsigned material);

// Viewing Angle: 0 off (every angle alike, as the game has it), 1 subtle, 2 realistic: reflections
// stronger looking across puddles and water, weaker looking down into them.
int port_reflection_view_angle(void);
// Reflection Blur: how blurred reflections in puddles and water are, in hundredths: 0 sharp to
// 1000 (10).
int port_reflection_blur(void);
// Sun Glints: 0 off, 1 subtle, 2 bright: the sun's light bouncing off puddles and water.
int port_sun_glints(void);
// Water Depth: 0 off, 1 subtle, 2 strong: the sea, lakes and pools darker over deep ground.
int port_water_depth(void);

// Reflection Quality (the settings menu, Reflections; not a mod's option): 0 low, 1 medium, 2 high
// (as before): how finely the reflection's surface is drawn (its ripples and glints), how many
// taps Reflection Blur takes.
int port_reflection_quality(void);
// Whether any of FLUDD's puddles lies within the Reflection Distance of Mario this tick (the
// reflection is only drawn there: the mirror's copies are not drawn for puddles beyond it).
int port_puddles_near(void);

// Reflection Distance: the radius, in the game's units, of the disc around Mario that puddle
// reflections are drawn over, fading out to its edge: 500 to 5000 (1000, the game's).
float port_puddle_reflection_distance(void);

// The Reflect switches: what is reflected, in puddles and water alike. Mario and Yoshi on, the rest
// off, until a mod sets them.
enum {
	PORT_REFLECT_MARIO,      // Mario and Yoshi: the game's own copies
	PORT_REFLECT_CHARACTERS, // the people (NPCs)
	PORT_REFLECT_ENEMIES,    // enemies and bosses
	PORT_REFLECT_ITEMS,      // coins, fruit and other things to take
	PORT_REFLECT_OBJECTS,    // everything else: crates, trees, doors, platforms
	PORT_REFLECT_KINDS
};
int port_reflect_kind(int kind);

// Puddles Beat Mirrors: 1 when FLUDD's puddles near Mario in a real mirror's area (a hotel
// mirror, a shiny floor) take the reflection from the mirror; 0 the mirror keeps it, as the game has
// it (the puddles then show none).
int port_puddles_beat_mirrors(void);

// Reflect the Level: 1 when the stage itself (its ground, buildings and sky) is reflected in
// puddles and water too, from the next area (ext/core/game/port_reflections.cpp).
int port_reflect_level(void);

// Wet Footprints: 1 when Mario's steps out of a puddle leave damp prints that fade.
int port_wet_footprints(void);
// Wet Mario: 1 when water leaves Mario wet: darker, his colours deeper, with a small highlight, up
// to where the water reached (all of him after swimming), drying over about 45 seconds.
int port_wet_mario(void);
// Wet Mario's look, from the menu: how dark, how shiny (1 each: as it was).
float port_wet_mario_darkness(void);
float port_wet_mario_shine(void);
// Around Mario's own draw (TMario::perform, zzzzz-ext-25-wet-mario): his wetness for the renderer,
// begin 1 before (view: the camera's, world to view), 0 after.
void port_wet_mario_draw(int begin, float (*view)[4]);
// Wet Characters: 1 when the people and enemies get wet too, with Wet Mario's look (sprayed by
// FLUDD: all over; in water: up to where it reaches), drying as he does.
int port_wet_characters(void);
// Once a frame, from the main camera's view (port_shadows_main_view): the wet ones to the renderer.
void port_wet_characters_view(float (*view)[4]);

// Water Reflections: 1 when the stage's own water and wet ground show the reflections puddles
// do (ext/core/game/port_water_reflections.cpp); 0 as the game has it.
int port_water_reflections(void);
// Water Ripples: 0 off (the reflection in water still), 1 gentle, 2 strong. Puddles and wet
// ground stay still.
int port_water_ripples(void);
// Puddle Ripples: 0 off (puddles always still), 1 gentle, 2 strong: rings spread in the puddles'
// and wet ground's reflection from each drop of FLUDD's water as it lands and from Mario's feet.
int port_puddle_ripples(void);
// Whether water or wet ground shows reflections this frame.
int port_water_mirror_active(void);
// Whether this frame's reflections are mirrored about a water surface, not the floor under Mario
// (FLUDD's puddles then show none: they are on another plane).
int port_water_mirror_on_water(void);

#ifdef __cplusplus
}
#endif

#endif
