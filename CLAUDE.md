# Notes for Claude on the first-person-goop branch

## Before sending a pull request to Chase (chasem-dev)

**Remind Allebale, and remove the freeze detector first** (commit d3d8ce3): `GXPC_FreezeBeat` in
`platform/gx/src/gx_platform.cpp` and `platform/port_runtime.cpp`, and the "Freeze detector" part and
`port_freeze_beat` at the end of `platform/os/windows_crash.cpp`. It was added only to find a freeze
when co-op's player 2 joins. Reverting that commit removes all of it.
