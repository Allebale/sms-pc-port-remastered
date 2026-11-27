# ext/levels: custom levels

A level made in SMS Launcher's World editor plays in a stage slot the disc leaves empty, so no stage of the game is replaced. The slot is the first area of the game's stage table (`data/stageArc.bin`) whose episodes' scene files are not on the disc, the developers' "scale" area by preference. Its episodes are the level and the levels its stage exits lead to.

## A level's folder

| File | What it is |
| --- | --- |
| `level.ini` | `id`, `name`, `author`, `description`, `shines` and `blue_coins` (how many it has) as `key = value` lines; for a level with episodes, `episodes` (2 to 8: its first stages), and `episode0`, `episode1`... (each one's name) with `episode0_shine`... (the Shine that is its own, -1 none); `music`, the world whose music it plays (below) |
| `stage0.szs` | the level: the slot's episode 0 |
| `stage1.szs` ... | the levels its stage exits lead to: episodes 1, 2... |

`SMS_LEVEL=<folder>` opens a level as the game starts (the World editor's Play, with `SMS_WARP=<area>,0`). `port_levels_open(folder)` opens one later, `port_levels_close()` closes it, and `port_levels_area()` says the slot's area. An open level's stages are laid over the slot's scene files (`port_dvd_lay_file`, platform/dvd).

## Its own name

The title card and the pause menu name the slot's area by the level's `name` (the extension's `stageName` hook; `zzzzzzzzzzz-ext-26-custom-levels.patch`), with no episode name. The game's own names go by world, and the slot counts as Delfino Plaza's.

## Its own Shines

While Mario is in the level, the save file's Shine flags, Delfino Plaza's blue coin flags (the slot is the plaza's world) and the slot's coin record are the level's own. The Shine and blue coin counts stay the file's: a Shine or blue coin got in the level doesn't add to them. The file's are kept aside and put back when he leaves. So the level's Shines never count toward the game's 120. The HUD shows them as special Shines, on a row of their own under its counters (an ice blue Shine on a blue brushstroke): every custom level's got with this save file (`port_levels_special_total`), always in the pause menu and for a moment once the HUD is back after one is got. The pause menu's Shine and blue coin tracker opens on a page of the level's own (its name, what Mario has of its own of `shines` and `blue_coins`), and its pages of the game's areas count the save file's own (`port_levels_file_flag`).

What he gets there is kept at once in `<memory card folder>-mods/custom-levels/<id>-<file>.txt` (beside the mods' saves, so the launcher backs it up with the card) (`file` a, b or c: the save file), as `shine N`, `blue N` and `coins N` lines. A save made in the level (the save prompt, the pause menu, auto save) writes the file's own flags: the extension puts them back for the save (the `flagsSaving` hook, from `TFlagManager::save` and `saveSuccess`).

## Levels installed

`SMS_LEVELS_DIR` names the launcher's levels folder, a level folder each (the World editor's Export, Custom level). Mods find them through `api->levels` (the mod SDK's `SmsLevelsApi`): how many there are, each one's name, author, description, folder and Shines (how many it has and how many the save file has), the special Shines of them all, and `enter`, which opens one and takes Mario there as a stage exit does. While a level is open, `game->area_name` gives its name for the slot's area.

## Episodes

A level with `episodes` has an episode select as the worlds do (port_ext.h's `areaEpisodes`, `areaEpisodeName`, `areaEpisodeGot`). Entered without an episode chosen (Map Teleport's Custom levels page, `port_levels_enter`), the game opens the select (TMarDirector::moveStage): it shows the level's name, its episodes' names, and offers each once the Shine of the one before is got (TSelectMenu::initData; no world's tables are read for it, and its coin record shows 0). The title card and the pause menu name the episode. Its stages after the episodes are its sublevels, shared by every episode: their way back to the first stage leads to the episode Mario came from.

A level with `music` plays that world's music instead of the slot's (port_ext.h's `areaMusic`; `zzzzzzzzzzzz-ext-28-level-music.patch`, in MSMainProc::setMSoundEnterStage): `airstrip`, `plaza`, `bianco`, `ricco`, `gelato`, `pinna-beach`, `sirena`, `hotel`, `pianta`, `noki`, `pinna`, `casino`, `title`, `undersea`, `secret`, `corona` or `boss`. Each stage plays it as that world's episode of the same number has it (episode 1 of `bianco` its first episode's), so a stage taken from a world keeps its music; the world's distance and cube fades are left out.

After a Shine or a lost life, Mario goes back where he came from, as from a world. A mod can choose otherwise (`api->levels->set_after_shine`, Map Teleport's Custom levels page): leaving a level with episodes after a Shine got on its stage leads to the next episode whose Shine he lacks (after that one, then from the first; every one got: back as before; `chooseNextStage`), with the title card; a lost life starts the stage he was on again (`missNextArea`), in any custom level, the last one too, with three lives and no game over screen (`takeGameOver`).
