// The native mod API's two halves (ext/modapi/README.md): the game half
// (game/, built into the game's library with its flags and headers) reads and
// changes the game and turns the extension hooks into mod events; the host
// half (host/, built into the executable) loads the mods, keeps their
// subscriptions, settings and drawing, and hands them the SmsApi tables. This
// header is all the two share: plain C, the public SDK's types.
#ifndef MODAPI_BRIDGE_H
#define MODAPI_BRIDGE_H

#include <sms_mod.h> /* the mod SDK: sdk/include, the sdk/ submodule */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- host half

// The top of every frame: loads the mods the first time, then sends
// SMS_EV_FRAME and SMS_EV_DRAW and hands the frame's drawing to the window.
void modh_frame(uint32_t frame, int mode);
// Sends an event to its subscribers; returns 1 if a handler cancelled it.
// Consumable events stop at the first handler that consumes them.
int modh_dispatch(uint32_t type, void* data, uint32_t size);
// 1 when a mod listens for `type` (to skip building payloads nobody reads).
int modh_wants(uint32_t type);
// The controllers were read: keeps them as read (input->pad), sends
// SMS_EV_PAD_READ, and blanks them for the game while a mod has the input.
void modh_pad_read(SmsPadStatus* pads);
// A cancelled SMS_EV_SAVE_STATE_REQUEST's reason, kept until the next.
const char* modh_save_state_reason(void);
// A save file was loaded, started anew, saved, copied or erased
// (SMS_SAVE_FILE_*): the mods' files of save files follow it (mod_files.cpp).
// Before SMS_EV_SAVE_FILE is sent.
void modh_files_event(int what, int file, int from);

// ---------------------------------------------------------------- game half

void modg_state(SmsGameState* out);
int32_t modg_flag_get(uint32_t id);
void modg_flag_set(uint32_t id, int32_t value);
int modg_warp(int area, int episode);
int modg_cube_count(int set);
int modg_in_cube(int set, int index, float x, float y, float z); /* -1: no such cube */
SmsObject modg_find_object(const char* name);
const char* modg_object_name(SmsObject obj);
int modg_object_position(SmsObject obj, SmsVec3* out);
int modg_object_set_position(SmsObject obj, const SmsVec3* pos);
uint32_t modg_object_type(SmsObject obj);
int modg_object_set_color(SmsObject obj, int which, SmsColor color);
int modg_object_set_tint(SmsObject obj, SmsColor color);
// The running disc's game code and maker ("GMSE01"), into out[7].
void modg_game_code(char* out);
int modg_object_health(SmsObject obj, int* max);
/* the HUD's parts every mod hides (SMS_HUD_* bits ORed); the parts hidden now (any owner's) */
void modg_set_hud_hidden(uint32_t mask);
uint32_t modg_hud_hidden(void);

int modg_player_get(SmsPlayer* out);
int modg_player_set_position(const SmsVec3* pos);
int modg_player_set_velocity(const SmsVec3* vel);
int modg_player_set_forward_speed(float speed);
int modg_player_set_health(int health);
int modg_player_set_water(int water);
int modg_player_set_status(uint32_t status, uint32_t arg);
int modg_player_set_nozzle(int nozzle, int animate);
int modg_player_set_fludd(int has);
int modg_player_set_outfit(int shirt, int sunglasses, int helmet);
int modg_player_set_yaw(float degrees); // API 1.5 beta 8
int modg_player_set_aim(int on, float yaw, float pitch); // API 1.5 beta 8
int modg_player_set_hidden_parts(uint32_t parts);        // API 1.5 beta 8
int modg_held_fludd_set(int on, float right, float down, float forward, float scale, float aim); // first person
int modg_spray_weapon_set(int weapon, int flags); // first person
int modg_spray_rocket_jump(float strength);
int modg_spray_view_distance(int weapon, float distance);
int modg_spray_far_scale(float scale);
int modg_spray_shadow_damage(float scale);
int modg_spray_goop_area(float scale);
int modg_spray_key_launch(int on);
int modg_spray_feet_forward(float k);
int modg_spray_key_lift(float up);
int modg_held_fludd_view(int on);
int modg_held_fludd_zoom(float fovy, int flags);
int modg_held_fludd_state(int* firstPerson, int* held, int* weapon, float* zoomFovy, int* zoomFlags);
// split-screen co-op: the players (ext/modapi/game/modapi_game.cpp)
int modg_players(void);       // 2 while a second player plays, else 1
int modg_player_now(void);    // the player the game runs as now (or the one selected): 0 or 1
void modg_select_player(int player); // api->player and held_fludd act on this player (-1: the one the game runs as)
int modg_player_set_hidden_parts_of(int player, uint32_t parts);
int modg_player_view(int player, float* rect); // its part of the screen (x, y, w, h, 0 to 1): 1 while split
int modg_spray_weapon_stats(int weapon, int has, float every, float push, float range, int toDefeat, int water, float hit,
                           float burst, float lift, float dropSize);

int modg_camera_get(SmsCamera* out);

int modg_world_area_kind(int area);
int modg_world_of(int area);
int modg_world_episode_count(int area);
int modg_world_episode_shine(int area, int episode);
int modg_world_episode_name(int area, int episode, char* out, int size);
int modg_world_next_episode(int area);
int modg_world_shine_episode(int shine, int* area);
int modg_world_has_cutscene(const char* name);
int modg_world_play_cutscene(const char* name, const SmsVec3* focus, int frames);
int modg_world_cutscene_playing(void);
void modg_play_system_sound(uint32_t id);
// API 1.8: general tools (modapi_world.cpp)
void modg_play_sound_at(uint32_t id, const SmsVec3* at);
int modg_particle(uint32_t id, const SmsVec3* at);
int modg_ray(const SmsVec3* a, const SmsVec3* b, SmsVec3* hit);
int modg_splash(const SmsVec3* at, float size);
int modg_clean_goop(const SmsVec3* at, float radius);
int modg_water_nozzle_drop(int player, SmsWaterDrop* out);
int modg_water_emit(const SmsWaterDrop* drop);
int modg_water_count(void);
int modg_water_get(int slot, SmsWaterDrop* out);
int modg_water_set(int slot, const SmsWaterDrop* in);
int modg_water_drop_of(SmsObject sender);
int modg_water_use(int player, int drops);
int modg_actor_velocity(SmsObject o, SmsVec3* out);
int modg_actor_set_velocity(SmsObject o, const SmsVec3* v);
uint32_t modg_actor_flags(SmsObject o);
int modg_actor_set_flags(SmsObject o, uint32_t on, uint32_t off);
int modg_actor_defeat(SmsObject o);
int modg_actor_near(const SmsVec3* p, float radius, uint32_t kinds, SmsObject* out, int most);
int modg_actor_hit_size(SmsObject o, float* radius, float* height);
int modg_actor_push(SmsObject o, float dx, float dz);
int modg_actor_send(SmsObject o, SmsObject sender, uint32_t message);
int modg_actor_wall_defeat(SmsObject o);
int modg_actor_set_model_scale(SmsObject o, float scale);
int modg_actor_kind(SmsObject o);
SmsObject modg_actor_owner(SmsObject o);
int modg_actor_parts(SmsObject o, SmsObject* out, int most);
int modg_actor_water_meter(SmsObject o, SmsWaterMeter* out);
int modg_actor_set_water_meter(SmsObject o, int left, int effects);
int modg_actor_set_water_wait(SmsObject o, int ticks);
int modg_actor_knock(SmsObject o, const SmsVec3* push);
int modg_wall_at(const SmsVec3* at, float radius, SmsVec3* normal);
int modg_set_view_distance(float scale);
int modg_play_clip(const int16_t* pcm, int frames, int rate, float volume);
int modg_player_set_fall_start(float y);
void modg_held_fludd_kick(float amount);
int modg_held_fludd_aim(SmsVec3* eye, SmsVec3* forward);
void* modg_native(const char* name);

#ifdef __cplusplus
}
#endif

#endif
