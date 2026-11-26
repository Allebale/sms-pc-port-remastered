# ext/levels: custom levels

A level made in SMS Launcher's World editor plays in a stage slot the disc leaves empty, so no stage of the game is replaced. The slot is the first area of the game's stage table (`data/stageArc.bin`) whose episodes' scene files are not on the disc, the developers' "scale" area by preference. Its episodes are the level and the levels its stage exits lead to.

## A level's folder

| File | What it is |
| --- | --- |
| `level.ini` | `id`, `name`, `author`, `description`, `shines` and `blue_coins` (how many it has) as `key = value` lines |
| `stage0.szs` | the level: the slot's episode 0 |
| `stage1.szs` ... | the levels its stage exits lead to: episodes 1, 2... |

`SMS_LEVEL=<folder>` opens a level as the game starts (the World editor's Play, with `SMS_WARP=<area>,0`). `port_levels_open(folder)` opens one later, `port_levels_close()` closes it, and `port_levels_area()` says the slot's area. An open level's stages are laid over the slot's scene files (`port_dvd_lay_file`, platform/dvd).

## Its own name

The title card and the pause menu name the slot's area by the level's `name` (the extension's `stageName` hook; `zzzzzzzzzzz-ext-26-custom-levels.patch`), with no episode name. The game's own names go by world, and the slot counts as Delfino Plaza's.

## Its own Shines

While Mario is in the level, the save file's Shine flags, Delfino Plaza's blue coin flags (the slot is the plaza's world), the Shine and blue coin counts and the slot's coin record are the level's own. The file's are kept aside and put back when he leaves. So the HUD counts the level's Shines, and the level's Shines never count toward the game's 120. The pause menu's Shine and blue coin tracker opens on a page of the level's own (its name, what Mario has of its own of `shines` and `blue_coins`), and its pages of the game's areas count the save file's own (`port_levels_file_flag`).

What he gets there is kept at once in `<memory card folder>/levels/<id>-<file>.txt` (`file` a, b or c: the save file), as `shine N`, `blue N` and `coins N` lines. A save made in the level (the save prompt, the pause menu, auto save) writes the file's own flags: the extension puts them back for the save (the `flagsSaving` hook, from `TFlagManager::save` and `saveSuccess`).
