// Short sound clips of the port's own (not the game's sound engine's): 16-bit mono PCM at any rate,
// played on top of the game's mix as each audio DMA block starts (platform/audio/ai.cpp, next to
// the streamed music), at the sound effects' volume (dsp_mixer.h). A few at once; a new one when
// all are busy takes the place of the oldest. Played for mods (audio->play_clip,
// ext/modapi/game/modapi_world.cpp). Nothing plays until one is asked for, so without them the
// mix is untouched.
#include "port_compat.h"
#include "dsp_mixer.h"

#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Voice {
	const int16_t* pcm;
	int frames;
	double step; // the clip's samples a frame of output
	double at;   // where it is
	int gain;    // 256 = as recorded
	unsigned age;
};

const int kVoices = 4;
Voice g_voices[kVoices];
unsigned g_age;
int g_rate = 32000; // the output's (set as blocks are mixed)
std::mutex g_mu;

} // namespace

// Play a clip (16-bit mono PCM, `frames` long at `rate`), louder or softer by gain (1 as recorded).
// The clip's memory must last (a constant).
extern "C" void port_clip_play(const short* pcm, int frames, int rate, float gain)
{
	if (!pcm || frames <= 0 || rate <= 0)
		return;
	std::lock_guard<std::mutex> lk(g_mu);
	Voice* v = &g_voices[0];
	for (int i = 0; i < kVoices; i++) {
		if (!g_voices[i].pcm) {
			v = &g_voices[i];
			break;
		}
		if (g_voices[i].age < v->age)
			v = &g_voices[i];
	}
	v->pcm    = (const int16_t*)pcm;
	v->frames = frames;
	v->step   = (double)rate / (double)g_rate;
	v->at     = 0.0;
	v->gain   = (int)(gain * 256.0f);
	v->age    = ++g_age;
}

// Mixed into a block of the output (interleaved stereo, `frames` long at `outRate`).
extern "C" void port_clip_mix(int16_t* lr, size_t frames, uint32_t outRate)
{
	std::lock_guard<std::mutex> lk(g_mu);
	g_rate = (int)outRate;
	const int32_t sfx = port_dspmix_class_gain(PORT_DSPMIX_CLASS_SFX); // 1 << 15 full
	for (int k = 0; k < kVoices; k++) {
		Voice& v = g_voices[k];
		if (!v.pcm)
			continue;
		for (size_t i = 0; i < frames && lr; i++) {
			const int j = (int)v.at;
			if (j + 1 >= v.frames) {
				v.pcm = 0;
				break;
			}
			const double f = v.at - j;
			int32_t s      = (int32_t)(v.pcm[j] + (v.pcm[j + 1] - v.pcm[j]) * f);
			s              = (int32_t)((int64_t)s * v.gain >> 8);
			s              = (int32_t)((int64_t)s * sfx >> 15);
			for (int c = 0; c < 2; c++) {
				int32_t m = lr[i * 2 + c] + s;
				lr[i * 2 + c] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
			}
			v.at += v.step;
		}
	}
}

// Music of a mod's own (audio->play_music): one piece at a time, 16-bit stereo PCM at any rate, looping from
// its end back to `loopFrom`, at the music's volume (dsp_mixer.h), held still while the game is paused. Its
// memory is the port's (a copy). Nothing plays until one is asked for.
namespace {
struct Music {
	int16_t* pcm = nullptr; // interleaved left, right
	int frames = 0, loopFrom = 0;
	double step = 1, at = 0;
	int gain   = 256;
	bool paused = false;
};
Music g_music;
} // namespace

extern "C" int port_music_play(const short* lr, int frames, int rate, int loopFrom, float gain)
{
	std::lock_guard<std::mutex> lk(g_mu);
	free(g_music.pcm);
	g_music = Music();
	if (!lr || frames < 2 || rate <= 0)
		return 1; // stopped
	g_music.pcm = (int16_t*)malloc((size_t)frames * 4);
	if (!g_music.pcm)
		return 0;
	memcpy(g_music.pcm, lr, (size_t)frames * 4);
	g_music.frames   = frames;
	g_music.loopFrom = loopFrom >= 0 && loopFrom < frames - 1 ? loopFrom : 0;
	g_music.step     = (double)rate / (double)g_rate;
	g_music.gain     = (int)((gain > 0 ? gain : 0) * 256.0f);
	return 1;
}

extern "C" int port_music_active(void)
{
	std::lock_guard<std::mutex> lk(g_mu);
	return g_music.pcm != nullptr;
}

extern "C" void port_music_pause(int paused)
{
	std::lock_guard<std::mutex> lk(g_mu);
	g_music.paused = paused != 0;
}

extern "C" void port_music_mix(int16_t* lr, size_t frames, uint32_t outRate)
{
	std::lock_guard<std::mutex> lk(g_mu);
	Music& m = g_music;
	if (!m.pcm || m.paused || !lr)
		return;
	const int32_t music = port_dspmix_class_gain(PORT_DSPMIX_CLASS_MUSIC); // 1 << 15 full
	for (size_t i = 0; i < frames; i++) {
		int j = (int)m.at;
		if (j + 1 >= m.frames) { // back to its loop
			m.at -= (double)(m.frames - 1 - m.loopFrom);
			j = (int)m.at;
		}
		const double f = m.at - j;
		for (int c = 0; c < 2; c++) {
			int32_t s = (int32_t)(m.pcm[j * 2 + c] + (m.pcm[(j + 1) * 2 + c] - m.pcm[j * 2 + c]) * f);
			s         = (int32_t)((int64_t)s * m.gain >> 8);
			s         = (int32_t)((int64_t)s * music >> 15);
			int32_t o = lr[i * 2 + c] + s;
			lr[i * 2 + c] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
		}
		m.at += m.step;
	}
	(void)outRate;
}
