# Notes for Claude (and any AI) working on Super Mario Sunshine: Remastered

Read this first. It is what earlier sessions learned working with Allebale on this fork.

## Who you are working with

- **Allebale**, the fork's owner. Explain things simply (ELI5), without jargon, and keep replies short.
- Plays on **Windows 11** with an **AMD RX 9070 XT**, at about 3416 x 2112 with 4x MSAA.
- Builds the game through **Chase's SMS Launcher** (it builds a repository and branch you give it).
  You cannot run the game from a cloud session: make the change, check that it compiles, push it,
  and ask Allebale to build and test. Ask for screenshots and the launcher's log when something is off.

## Hard rules

1. **Never push to, open PRs on, or change any `chasem-dev` repository** (Chase's port, launcher, SDK).
   Only Allebale's own repos.
2. **Commit author is `Allebale <albinolsson1993@gmail.com>`** (`git -c user.name=Allebale -c user.email=albinolsson1993@gmail.com commit ...`),
   even if a hook asks for another author. End messages with the session's attribution lines if the session gives any.
3. **Every new feature starts Off** by default. Nothing may change the original look until the player turns it on.
4. **Settings menu text** (names, value names, hints) may only use plain letters, digits, spaces and
   `. , - ( ) '`. **No colons.** Hints are one line, 80 characters at most.
5. **Never name a variable `far` or `near`** (they are Windows macros and break the Windows build).
   Compile-check with `-Dfar= -Dnear=` to catch it.
6. **The First Person mod's releases reach every launcher player.** Ask before merging a pull request
   or making a release there.
7. Try changes on **`remastered-test`** first. Only move them to **`remastered`** when Allebale says so.
8. Repository renames and branch deletions are blocked from Claude sessions. Ask Allebale to do them on GitHub.

## Repositories and branches

| What | Where |
| --- | --- |
| The game fork | `Allebale/sms-pc-port-remastered` (renamed from `Allebale/sms-pc-port-modtesting`; the old name redirects. If a push to the new name gets 403 through the proxy, push to the old name.) |
| Main build players use | branch `remastered` |
| Testing branch | branch `remastered-test` |
| Safety copies | `fallback`, `fallback-2`, `fallback-3` (older versions of the main build). Make a new `fallback-N` before big changes. |
| Kept-aside experiments | `ambient-occlusion`, `hanging-things` (Swaying Hanging Things, removed), `ao-thin-option` (local only, never pushed), `character-ambient-occlusion` (Character Ambient Occlusion: None to Full shade on people and enemies, against blocky patches on low-poly ones; for Allebale to try later, not on `remastered-test`) |
| First Person beta | branch `first-person-beta`: Chase's beta and only the First Person mod's game side (FLUDD held in view, FLUDD weapons), no Remastered features (its four `zzzzzzz-held-fludd*` patches are made against Chase's own patches; its sdk is the SDK fork's `first-person-beta` branch: Chase's SDK with only `held_fludd`, right after `files`; nothing Remastered in it. `combined-plus` has `held_fludd` in that same place, `reflections` after it, so the First Person mod (built with that SDK in its `sdk/`) finds it the one way, `SMS_API_HAS` and its magic, on both builds; Chase's beta is what matters most). Bring Chase's updates in as for `remastered-test` |
| First Person mod | `Allebale/sms-first-person-mod` (the launcher uses its GitHub releases; work on branch `split`, also pushed to `fludd-gun` for CI), version 1.2.0 |
| FLUDD Weapons mod | `Allebale/fludd-weapons` (its own repository since Nov 2026, version 1.0.0; a release only on a `v*` tag). The two mods talk through `held_fludd`'s `set_view`, `set_zoom`, `get_state`, `take_mouse` |
| SDK fork | `Allebale/sms-mod-sdk-modtesting`: branch `remastered-test` for this branch (combined-plus as it was here, plus Chase's newer SDKs); `combined-plus` itself also carries the goop weapons' SDK |
| Chase's port (upstream, read only) | `chasem-dev/sms-pc-port`, branch `main` (the launcher's Beta builds the newest commit there) |

### Moving test to main

`remastered-test` normally contains everything on `remastered`, so moving it over is a fast-forward:
`git push origin origin/remastered-test:refs/heads/remastered`. Make a `fallback-N` branch of the old
`remastered` first.

### Bringing in Chase's updates

Fetch `https://github.com/chasem-dev/sms-pc-port.git main`, merge it into `remastered-test`, resolve
conflicts by **keeping both** Chase's change and the fork's, compile-check, push, and let Allebale test
before it goes to `remastered`.

If the `sdk` submodule conflicts (Chase moved to a new SDK), merge Chase's SDK commit into
`Allebale/sms-mod-sdk-modtesting` `remastered-test` (keep the fork's `reflections` as the last `SmsApi`
member), push it, and point `sdk` at that merge (`git update-index --cacheinfo 160000,<sha>,sdk`).
Then dry-run every `decomp-patches/*.patch` in C-locale name order on a copy of `decomp/`: Chase's
patch numbers can collide with the fork's (both had `ext-31` and `ext-32`); different names still apply.

### If the launcher fails with `EPERM: operation not permitted, rename ...sms-pc-port.releases...`

Windows (usually the antivirus scanning new files, or an Explorer window) held the launcher's download
folder. Close Explorer windows there, delete leftover `*.download-*` folders in
`%AppData%\sms-launcher\sms-pc-port.releases`, and build again. Adding `%AppData%\sms-launcher` as a
Windows Security exclusion helps. Last time Allebale had to delete the release folders and let the
launcher download everything again. Memory cards are backed up first (`SMS Launcher Backups`).

## What the fork adds, and where it lives

All of the fork's settings are in **`platform/misc/port_puddle_settings.cpp`**: the Reflections page,
the fork's rows on the Graphics page, saving to `puddles.txt` (beside the memory card, separate from
saves), and the two presets:

- **Reflections page `Preset`**: Original, Remastered, Performance or Ultra Performance (`kPresets`).
- **Graphics page `Effects Preset`**: Original, Remastered, Performance or Ultra Performance (`kEffects`: each Graphics row by its
  label and the value name for each preset). **When you add a Graphics setting, add it to `kEffects`.**
- **Performance** is made for a player's laptop (RTX 4050 6 GB, i5-13420H, 16 GB): smaller shadow maps
  (Shadow Size Medium), cheaper Ambient Occlusion, Reflection Quality Low, no blur; Reflect the Level kept.
  **Ultra Performance**: Shadow Size Low, no Ambient Occlusion, less grass, everything reflected but no Reflect the Level.

Saves are never touched by the fork: no file it changes deals with the memory card or progress.

| Feature | Game side | Renderer side |
| --- | --- | --- |
| Puddle and water reflections, ripples, glints, water depth, wet footprints, Puddle Lifetime | `ext/core/game/port_reflections.cpp`, `port_water_reflections.cpp`, `platform/misc/port_puddle_options.cpp` | `platform/gx/src/gx_render.cpp`, `gx_shader.cpp` |
| Wet Mario, Sandy Mario (drawn on Mario only) | `port_water_reflections.cpp` (`port_wet_mario_draw`), `port_sandy.cpp` | `gx_shader.cpp`: only compiled into Mario's own program (`depthOnly == 3`, set by `GXPC_ShadowTag`) |
| Wet Characters, Wet Characters Shine (Light and Strong by default, with drips as Mario's: `port_wet_drip`; Off in the Original preset; their own darkness and shine, as Wet Mario's rows) | `ext/core/game/port_wet_characters.cpp`: sprayed (the `objectHitDone` extension hook, `HIT_MESSAGE_SPRAYED_BY_WATER`) or standing in water; the wettest 8 near Mario sent each frame as spheres in the view (`GXPC_SetWetCharacters`) | `gx_fifo.cpp` (`g.charDraw` for registered character lists), `gx_shader.cpp` (`depthOnly == 4`, `u_wetc`) |
| Real Shadows, Shadow Size (`GXPC_ShadowMapSize`: 4096, 2048, 1024), Soft Shadow Edges, Round Shadows Under Characters | `ext/core/game/port_shadows.cpp`, `decomp-patches/zzzzz-ext-31-shadows*.patch` | `gx_render.cpp` (`GXPC_ShadowFrame`, two 4096 cascades, `shadowNoCast`), `gx_shader.cpp` (`shadowLit`) |
| Ambient Occlusion (+ Strength, Distance, Quality, Update Rate, Mario Ambient Occlusion, Ambient Occlusion Near Grass), Depth Buffer View | `port_shadows.cpp` calls `GXPC_AOFrame`, `GXPC_AOMario` and `GXPC_AOGrass` | `gx_render.cpp` (`kAoTraceFs`, `kAoApplyFs`, `kAoViewFs`, `aoApply`) |
| 3D Grass and flattening | `ext/core/game/port_grass.cpp` | `gx_render.cpp` (`GXPC_Grass*`), `gx_shader.cpp` (`kGrassVs`, `kGrassFs`) |
| Moving Vegetation (Off to Very Windy; palm fronds are solid only while a tree is touched: `port_tree_touched`) | `ext/core/game/port_vegetation.cpp`, `decomp-patches/zzzzz-ext-32-vegetation-trees.patch` | `gx_render.cpp` (`GXPC_SwayRegister`), `gx_fifo.cpp` (`callDisplayList`) |
| Puddles Around Fountains (Reflections page; Off, on in every preset but Original): each arched fountain effect (`TEffectPinnaFunsui`, `TEffectBiancoFunsui`, found in `gpConductor->unk40`) gets a ring of 12 large FLUDD puddles out to where its jets land, the middle dry (one big puddle showed an unseen box round the nozzle through it: FLUDD puddles show what writes only alpha), topped up while Mario is within 4500 and drying in about a minute after (lifetime 60 s divided by Puddle Lifetime's scale). Never flying, so it hits no actor; not on actor-owned ground or goop. Logged as `[puddles] N fountains` | `ext/core/game/port_fountains.cpp` | (FLUDD's puddles, as ever) |
| 3D Ladders (Off; on in Remastered and the Performance presets): a draw whose first texture is a known ladder's (`kLadderTextures` in `gx_render.cpp`, found with the grab log; Erto Rock's is `tex1_512x512_00b6be1aafdb87d0_14`) is drawn again ten times behind itself, darker each, so rails and rungs have sides. The older shape matching (flat map shapes on `BG_TYPE_CLIMBABLE_FENCE` collision) found none in Erto Rock (its ladders share a material with the whole level) and stays only as a fallback | `ext/core/game/port_ladders.cpp` (the fallback, `GXPC_LadderRegister`) | `gx_render.cpp` (`ladderTexture`, `ladderLayers`), `gx_fifo.cpp` (the fallback's layers), `gx_shader.cpp` (`depthOnly == 5`, `u_shell`) |
| FLUDD held in view (First Person mod option "FLUDD in view", Off): the game's own FLUDD model moved each frame in front of the view like a gun (`api->held_fludd`, combined-plus SDK; the mod finds it after `files` and `reflections` by its magic, so it still loads on Chase's builds). Its turn: the nozzle's way to the crosshair, worked out from a still pose (house animation frame 0, nozzle out), held still during flips; sway, kick-back, right arm hidden, back and side flips animated as a double jump. Only with the spray and underwater nozzles, off Yoshi | `ext/modapi/game/modapi_held_fludd.cpp`, `decomp-patches/zzzzzzz-held-fludd*.patch` | (FLUDD's own model) |
| FLUDD weapons (mod options "FLUDD weapons" and "FLUDD weapons can defeat enemies", both Off): Normal spray, Shotgun, Rifle, Bazooka on D-pad Left and Right. With a weapon chosen the mod keeps R from the game (no stream, pump or sound) and passes it on; the game fires on its own clock (seconds, not frames: the game ticks at the frame rate, 120). Drops carry the weapon and shot in their attack's upper bits (`getParticleAttack` gives the game only the lower 8). Rifle hitscan (the line traced in 2000-unit pieces inside the stage: one long line crashed the map's lookup), rifle and bazooka drops fly straight; bazooka bursts over the stage's enemy managers (`gpConductor->unk20`, `unk10`). Normal spray is the game's own, never changed. Super Mario Eclipse's engine replaces FLUDD's emit, so weapons hook `emitRequest`, not the nozzle | `ext/modapi/game/modapi_spray_weapons.cpp`, `decomp-patches/zzzzzzz-held-fludd-4-weapons.patch` | |
| FLUDD weapons' Long range (FLUDD Weapons mod, Off to Insane: 1.5 to 10 times): enemies and characters drawn and moving further away (`TLiveManager::clipActorsAux`'s far distance times `far_scale`; map objects as ever) | `decomp-patches/zzzzzzz-held-fludd-5-reach.patch`, `modapi_spray_weapons.cpp` (`PortSprayWeaponFarScale`) | |
| Global Illumination (was Fire Lights; fires and Shine Sprites are the lights, `Fire Light Intensity` sets their reach; the saved key is still `firelights`) | `ext/core/game/port_lights.cpp` | `gx_shader.cpp` (`u_plight`) |

Hooks into the game's own code are the `decomp-patches/*.patch` files, applied in name order with
`patch -p1` (`cmake/patches.cmake`). When editing one, keep each hunk's line counts right.

### Renderer facts worth knowing

- Texture units: 0 to 7 the game's, 8 AO Near Grass's ground map, 9 grass depth after the blades, 10 shadow map,
  11 and 12 grass copy (colour, depth), 13 grass flat map, 14 AO depth, 15 AO shade.
- Post passes use `kPostVs` and `compileProgram`, read the picture with `efbReadFbo` plus a blit,
  and end with `glcInvalidate()`.
- AO runs once a frame when the main camera's 3D scene ends (the first orthographic projection after it).
  The game's depth is stored in steps, so slopes and normals use points **3 pixels apart**, not the next pixel.
- 3D Grass blades are known per pixel (depth before and after the blades), so AO never shades them.
- Ambient Occlusion Near Grass (Kept or Removed): as the grass is done (`GXPC_GrassEnd`), `grassMask()` draws the
  blades drawn this frame again with the grass program in a second mode (`u_gtile.z` 1): a flat disc 26 units round each
  root, writing the ground's view z into a map of the picture (unit 8). The AO trace leaves a pixel alone whose depth is
  that ground's (within 18 units and 1%); anything standing on it (Mario, characters, walls) is nearer the camera and keeps
  its shade. An earlier try that guessed "ground" from the picture's shape was slow and unreliable and was replaced.
  Its third choice, **All But Characters**, instead draws the blades from above into a 1024 map (6144 units round
  Mario, each texel the ground's height within 70 units of a blade: `grassTop`), and Mario's (`g.shadowTag`) and the
  registered characters' draws again into a depth map of their own (`charCast`, `g.charMask`). The AO trace takes each
  pixel back to the world (`u_inv`): no shade near that ground (fading out 70 to 180 units above it, so wall feet and
  trunks clear too), unless the pixel is a character's. Units 6 and 7 hold those two maps during the AO pass only.
- Reflections: the mirror's copies (characters' and, with Reflect the Level, the whole stage's) are drawn only while
  a puddle lies within the Reflection Distance of Mario (`port_puddles_near`) or the water reflects. Reflection Quality
  (Reflections page) sets the reflection web's density (48x96, 30x60, 18x36) and Reflection Blur's taps (7 or 4).
- Mario's AO is limited with a capsule around him (`u_mario`), not his exact shape. Doing his exact
  shape would need a stencil buffer or two extra depth copies a frame (slower, riskier).
- Round Shadows Under Characters: characters', enemies' and Yoshi's display lists are registered at
  stage setup (`GXPC_ShadowCharacter`) and cast no sun shadow while it is on; Mario's draw is known by `g.shadowTag`.
- Keep shaders small: big shaders caused stutters. Mario-only code goes only into Mario's program.
- A texture that is uploaded again gets a fresh GL texture (that fixed stutters too).
- Long frames (45 ms or more) are logged by default as `hitchFrame`, useful for stutter reports.
- **The grab log** (`ext/core/game/port_grab.cpp`, `GXPC_GrabProbe` in `gx_render.cpp`): as Mario takes hold of
  something, `[grab]` lines give his status, the wall's collision type (0x10A: a climbable fence) and the next frame's
  draws near his hands with their textures (`hiresName`, as the texture pack names them; CI textures without the
  palette part). Use it to find a ladder's texture: the draws are listed with their nearest corner's distance from
  his hands, vertex count and whether they have see-through holes (an alpha test).
- **3D Ladders by texture** (`kLadderTextures` in `gx_render.cpp`, `ladderTexture`, `ladderLayers`): a draw whose
  first texture is listed there is drawn again ten times behind itself (each corner moved along its own normal, away
  from the camera). Add a ladder's texture name from the grab log to that list.
- **Startup time** (done Nov 2026: from about 3 minutes to about 6 seconds into Delfino Plaza on Allebale's AMD card):
  the shader cache (`programCacheLoad`, `gx_shader.cpp`) times its first 12 program binaries; AMD's driver takes about
  0.12 s for each, one at a time (all handed over at once did not help), while it compiles from source side by side at
  about 0.015 s each. When they are slower than 40 ms each, the rest are not loaded: their keys go to the warm-up
  (`s_slowBinaries`, `s_cacheKeysToWarm`), which compiles them from source as stages load. NVIDIA loads binaries fast
  and keeps them. The `[startup]` clock (`port_runtime.cpp`, every 2 s for 5 minutes) shows where a start spends its
  time; it is off unless the environment variable `SMS_STARTUP_CLOCK=1` is set (it filled the log).
- **Never `new` in an extension's `stageSetup` (or anything during a stage) and keep the pointer for later
  stages**: the stage's heap is the current one there, freed when the stage ends, and the next stage's objects
  are then written over. 3D Ladders did that once (`s_fences`) and Pokeys crashed in Bianco Hills and Erto Rock.
  Use static arrays.

## Checking your work without running the game

- **C++**: syntax-check the files you changed with `g++ -fsyntax-only -Dfar= -Dnear=`
  and the include folders they need (`src/port_include`, `platform/gx/include`, `platform/gx/src`).
  Game files under `ext/` need the decomp's headers as well.
  **Game files under `ext/` are built as `-std=gnu++03` (no C++11: no `unordered_set`, `auto`, `nullptr`, range-for)**,
  so check them with `-std=gnu++03 -fno-gnu-keywords -fpermissive -DTARGET_PC -DVERSION_GMSE01 -include src/port_compat.h`.
  The launcher log shows the exact compile line. The renderer under `platform/gx` is newer C++.
- **Shaders**: copy the GLSL out of the `R"( ... )"` string (or generate a program's text by building a
  tiny program that includes `gx_shader.cpp`) and run `glslangValidator` on it.
- **Patches**: apply them in order to a copy of the decomp file with `patch -p1 --dry-run`.
- Then push to `remastered-test` and ask Allebale to build it. Tell him what to look at and where.
- Depth Buffer View (Graphics page) shows depth, normals, the AO shade alone and depth edges: ask for
  screenshots of it when AO or depth looks wrong.

## Planned features

- **No grass through goop**: where goop covers grassy ground, the goop shows (no blades through it).
- **No double world drawing with Reflect the Level Off**: when the Reflections option Reflect the Level is Off, the stage should not be drawn a second time for the mirror.
- **Smooth vertex normals**: smooth the shading of low-poly models (averaged vertex normals), so they look less blocky.
- **First person pipes**: in first person (the First Person mod), going through a pipe shows no pipe walls around you: looking up, the world outside shows through. Allebale will describe it more.
- **First person manhole tunnels** (Delfino Plaza): they have only a floor (the game's camera looks down there). Walls and a ceiling drawn from the collision (Nov 2026) kept a faint haze of the surface over them that nothing found removed (not the specular pass's alpha, the see-through pass, the screen composite or drawing them last), so it was removed; Allebale will do it with a level editor later.

## Ideas tried and dropped (do not bring back unasked)

God Rays, Cloud Shadows, Moving Clouds, Moving Cloth, Swaying Hanging Things, Grass Stains (and Grass
Stain Duration; removed on request), Grass Thinned Far Away,
the Realistic Soft Shadow Edges option, and Softer and Very Soft Shadow Edges.
Frame generation and upscaling (FSR, DLSS and others): dropped unless a way is found that works with OpenGL.

**Vulkan** (the launcher's renderer choice) was tried on branch `remastered-test-vulkan` (Nov 2026): it runs
(missing GL entry points, shadow compare samplers, line varyings and the mod layer were fixed there, and shadow
casts were moved to a side command buffer), but it stayed slower than OpenGL on the RX 9070 XT (OpenGL 110-120 fps,
Vulkan 35-100). Allebale decided to stay on OpenGL. The branch is kept as it is; nothing from it is on
`remastered-test` or `remastered`.
