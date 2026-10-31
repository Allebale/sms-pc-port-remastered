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
| Kept-aside experiments | `ambient-occlusion`, `hanging-things` (Swaying Hanging Things, removed), `ao-thin-option` (local only, never pushed) |
| First Person mod | `Allebale/sms-first-person-mod` (the launcher uses its GitHub releases) |
| SDK fork | `Allebale/sms-mod-sdk-modtesting`, branch `combined-plus` |
| Chase's port (upstream, read only) | `chasem-dev/sms-pc-port`, branch `main` (the launcher's Beta builds the newest commit there) |

### Moving test to main

`remastered-test` normally contains everything on `remastered`, so moving it over is a fast-forward:
`git push origin origin/remastered-test:refs/heads/remastered`. Make a `fallback-N` branch of the old
`remastered` first.

### Bringing in Chase's updates

Fetch `https://github.com/chasem-dev/sms-pc-port.git main`, merge it into `remastered-test`, resolve
conflicts by **keeping both** Chase's change and the fork's, compile-check, push, and let Allebale test
before it goes to `remastered`.

### If the launcher fails with `EPERM: operation not permitted, rename ...sms-pc-port.releases...`

Windows (usually the antivirus scanning new files, or an Explorer window) held the launcher's download
folder. Close Explorer windows there, delete leftover `*.download-*` folders in
`%AppData%\\sms-launcher\\sms-pc-port.releases`, and build again. Last time Allebale had to delete the release folders and let the launcher download everything again. Memory cards are backed up first (`SMS Launcher Backups`).

## What the fork adds, and where it lives

All of the fork's settings are in **`platform/misc/port_puddle_settings.cpp`**: the Reflections page,
the fork's rows on the Graphics page, saving to `puddles.txt` (beside the memory card, separate from
saves), and the two presets:

- **Reflections page `Preset`**: Original or Remastered (`kPresets`).
- **Graphics page `Effects Preset`**: Original or Remastered (`kEffects`: each Graphics row by its label
  and the value name for each preset). **When you add a Graphics setting, add it to `kEffects`.**

Saves are never touched by the fork: no file it changes deals with the memory card or progress.

| Feature | Game side | Renderer side |
| --- | --- | --- |
| Puddle and water reflections, ripples, glints, water depth, wet footprints, Puddle Lifetime | `ext/core/game/port_reflections.cpp`, `port_water_reflections.cpp`, `platform/misc/port_puddle_options.cpp` | `platform/gx/src/gx_render.cpp`, `gx_shader.cpp` |
| Wet Mario, Sandy Mario (drawn on Mario only) | `port_water_reflections.cpp` (`port_wet_mario_draw`), `port_sandy.cpp` | `gx_shader.cpp`: only compiled into Mario's own program (`depthOnly == 3`, set by `GXPC_ShadowTag`) |
| Real Shadows, Soft Shadow Edges, Round Shadows Under Characters | `ext/core/game/port_shadows.cpp`, `decomp-patches/zzzzz-ext-31-shadows*.patch` | `gx_render.cpp` (`GXPC_ShadowFrame`, two 4096 cascades, `shadowNoCast`), `gx_shader.cpp` (`shadowLit`) |
| Ambient Occlusion (+ Strength, Distance, Quality, Update Rate, Mario Ambient Occlusion), Depth Buffer View | `port_shadows.cpp` calls `GXPC_AOFrame` and `GXPC_AOMario` | `gx_render.cpp` (`kAoTraceFs`, `kAoApplyFs`, `kAoViewFs`, `aoApply`) |
| 3D Grass and flattening | `ext/core/game/port_grass.cpp` | `gx_render.cpp` (`GXPC_Grass*`), `gx_shader.cpp` (`kGrassVs`, `kGrassFs`) |
| Moving Vegetation | `ext/core/game/port_vegetation.cpp` | `gx_render.cpp` (`GXPC_SwayRegister`), `gx_fifo.cpp` (`callDisplayList`) |
| Fire Lights | `ext/core/game/port_lights.cpp` | `gx_shader.cpp` (`u_plight`) |

Hooks into the game's own code are the `decomp-patches/*.patch` files, applied in name order with
`patch -p1` (`cmake/patches.cmake`). When editing one, keep each hunk's line counts right.

### Renderer facts worth knowing

- Texture units: 0 to 7 the game's, 8 free, 9 grass depth after the blades, 10 shadow map,
  11 and 12 grass copy (colour, depth), 13 grass flat map, 14 AO depth, 15 AO shade.
- Post passes use `kPostVs` and `compileProgram`, read the picture with `efbReadFbo` plus a blit,
  and end with `glcInvalidate()`.
- AO runs once a frame when the main camera's 3D scene ends (the first orthographic projection after it).
  The game's depth is stored in steps, so slopes and normals use points **3 pixels apart**, not the next pixel.
- 3D Grass blades are known per pixel (depth before and after the blades), so AO never shades them.
- Mario's AO is limited with a capsule around him (`u_mario`), not his exact shape. Doing his exact
  shape would need a stencil buffer or two extra depth copies a frame (slower, riskier).
- Round Shadows Under Characters: characters', enemies' and Yoshi's display lists are registered at
  stage setup (`GXPC_ShadowCharacter`) and cast no sun shadow while it is on; Mario's draw is known by `g.shadowTag`.
- Keep shaders small: big shaders caused stutters. Mario-only code goes only into Mario's program.
- A texture that is uploaded again gets a fresh GL texture (that fixed stutters too).
- Long frames (45 ms or more) are logged by default as `hitchFrame`, useful for stutter reports.

## Planned features

- ** No AO Near Grass**: exclude Ambient Occlusion from the ground among and around 3D grass, while keeping AO on Mario, NPCs and enemies standing in or near the grass. This needs reliable per-pixel character identification; it is not implemented and must not be exposed as a setting until that works.
- **3D Ladders**: add 3D ladder geometry to replace or enhance the flat ladder visuals.
- **Code Quality Review**: review the codebase for correctness, maintainability, performance issues, and unnecessary complexity; fix issues where appropriate.
- **Faster Game Start**: investigate and reduce game startup time without changing gameplay or introducing regressions.
- **Puddles Around Fountains**: add puddles around fountains.
- **Shine Sprites Global Illumination**: use light from Shine Sprites to illuminate the surrounding environment.
- **Frame generation and Upscaling** FSR/DLSS/Other.
- **Disalbe Double World Rendering is the graphic optinon "Reflect the Level" is Off**
- **No goop on grass** Goop takes priority.

## Ideas tried and dropped (do not bring back unasked)

God Rays, Cloud Shadows, Moving Clouds, Moving Cloth, Swaying Hanging Things, Grass Stains
Duration; removed on request), Grass Thinned Far Away,
the Realistic Soft Shadow Edges option, and Softer and Very Soft Shadow Edges.
