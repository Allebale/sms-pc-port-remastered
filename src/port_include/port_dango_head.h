// A Dango's head (ext/core/game/port_dango_head.cpp): a Dango stack whose model folder also has a
// head.bmd shows it on whichever ball is on top, as a snowman's head on its body. A folder without
// one (every Dango of the game's own) is drawn as ever.
#ifndef PORT_DANGO_HEAD_H
#define PORT_DANGO_HEAD_H

class TDangoHamuKuri;

void port_dango_head_init(TDangoHamuKuri* dango);
void port_dango_head_update(TDangoHamuKuri* dango);

#endif
