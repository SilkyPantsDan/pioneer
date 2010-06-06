/*
 * From an old project of mine, way back in 2001 or so.
 */

/*
 * This is tom's dodgy SDL sound code v1.0
 */

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <vorbis/vorbisfile.h>
#include "Sound.h"
#include "Body.h"
#include "Pi.h"
#include "Player.h"

namespace Sound {

#define FREQ            44100
#define BUF_SIZE	2048
#define MAX_WAVSTREAMS	16

eventid BodyMakeNoise(const Body *b, const char *sfx, float vol)
{
	vector3d pos;
       
	if (b == Pi::player) {
		pos = vector3d(0.0);
	} else {
		pos = b->GetPositionRelTo(Pi::player->GetFrame()) - Pi::player->GetPosition();
		matrix4x4d m;
		Pi::player->GetRotMatrix(m);
		pos = m.InverseOf() * pos;
	}

	float len = pos.Length();
	float v[2];
	if (len != 0) {
		vol = vol / (0.002*len);
		double dot = vector3d::Dot(pos.Normalized(), vector3d(vol, 0, 0));

		v[0] = vol * (2.0f - (1.0+dot));
		v[1] = vol * (1.0 + dot);
	} else {
		v[0] = v[1] = vol;
	}
	v[0] = CLAMP(v[0], 0.0f, 1.0f);
	v[1] = CLAMP(v[1], 0.0f, 1.0f);

	return Sound::PlaySfx(sfx, v[0], v[1], false);
}

struct Sample {
	Uint8 *buf;
	Uint32 buf_len;
	Uint32 channels;
};

struct SoundEvent {
	const Sample *sample;
	Uint32 buf_pos;
	float volume[2]; // left and right channels
	eventid identifier;
	Uint32 op;

	float targetVolume[2];
	float rateOfChange[2]; // per sample
	bool ascend[2];
};	

static std::map<std::string, Sample> sfx_samples;
struct SoundEvent wavstream[MAX_WAVSTREAMS];

static Sample *GetSample(const char *filename)
{
	if (sfx_samples.find(filename) != sfx_samples.end()) {
		return &sfx_samples[filename];
	} else {
		Warning("Unknown sound sample: %s", filename);
		return 0;
	}
}

static SoundEvent *GetEvent(eventid id)
{
	for (int i=0; i<MAX_WAVSTREAMS; i++) {
		if (wavstream[i].sample && (wavstream[i].identifier == id))
			return &wavstream[i];
	}
	return 0;
}

bool SetOp(eventid id, Op op)
{
	if (id == 0) return false;
	bool ret = false;
	SDL_LockAudio();
	SoundEvent *se = GetEvent(id);
	if (se) {
		se->op = op;
		ret = true;
	}
	SDL_UnlockAudio();
	return ret;
}

/*
 * Volume should be 0-65535
 */
eventid PlaySfx (const char *fx, float volume_left, float volume_right, Op op)
{
	SDL_LockAudio();
	static Uint32 identifier = 1;
	int idx;
	Uint32 age;
	/* find free wavstream */
	for (idx=0; idx<MAX_WAVSTREAMS; idx++) {
		if (wavstream[idx].sample == NULL) break;
	}
	if (idx == MAX_WAVSTREAMS) {
		/* otherwise overwrite oldest one */
		age = 0; idx = 0;
		for (int i=0; i<MAX_WAVSTREAMS; i++) {
			if ((i==0) || (wavstream[i].buf_pos > age)) {
				idx = i;
				age = wavstream[i].buf_pos;
			}
		}
	}
	wavstream[idx].sample = GetSample(fx);
	wavstream[idx].volume[0] = volume_left;
	wavstream[idx].volume[1] = volume_right;
	wavstream[idx].op = op;
	wavstream[idx].identifier = identifier;
	wavstream[idx].targetVolume[0] = volume_left;
	wavstream[idx].targetVolume[1] = volume_right;
	wavstream[idx].rateOfChange[0] = wavstream[idx].rateOfChange[1] = 0.0f;
	SDL_UnlockAudio();
	return identifier++;
}

static void fill_audio (void *udata, Uint8 *dsp_buf, int len)
{
	int written = 0;
	int i;
	float val[2];
	int buf_end;
	
	for (i=0; i<MAX_WAVSTREAMS; i++) {
		if (wavstream[i].sample == NULL) continue;
		for (int chan=0; chan<2; chan++) {
			if (wavstream[i].targetVolume[chan] > wavstream[i].volume[chan]) {
				wavstream[i].ascend[chan] = true;
			} else {
				wavstream[i].ascend[chan] = false;
			}
		}
		if (wavstream[i].op & OP_STOP_AT_TARGET_VOLUME) {
			if ((wavstream[i].targetVolume[0] == wavstream[i].volume[0]) &&
			    (wavstream[i].targetVolume[1] == wavstream[i].volume[1])) {
				wavstream[i].sample = 0;
			}
		}	
	}
	
	while (written < len) {
		/* Mix them */
		buf_end = 0;
		while ((written < len) && (!buf_end)) {
			val[0] = val[1] = 0;

			for (i=0; i<MAX_WAVSTREAMS; i++) {
				if (wavstream[i].sample == NULL) continue;
				
				for (int chan=0; chan<2; chan++) {
					if (wavstream[i].ascend[chan]) {
						wavstream[i].volume[chan] = MIN(wavstream[i].volume[chan] + wavstream[i].rateOfChange[chan], wavstream[i].targetVolume[chan]);
					} else {
						wavstream[i].volume[chan] = MAX(wavstream[i].volume[chan] - wavstream[i].rateOfChange[chan], wavstream[i].targetVolume[chan]);
					}
				}

				const Sample *s = wavstream[i].sample;
				if (s->channels == 2) {
					val[0] += wavstream[i].volume[0] *
						(float) ((Sint16*)s->buf)[wavstream[i].buf_pos/2];
					wavstream[i].buf_pos += 2;
					val[1] += wavstream[i].volume[1] *
						(float) ((Sint16*)s->buf)[wavstream[i].buf_pos/2];
					wavstream[i].buf_pos += 2;
				} else {
					float v = wavstream[i].volume[0] *
						(float) ((Sint16*)s->buf)[wavstream[i].buf_pos/2];
					val[0] += v;
					val[1] += v;
					wavstream[i].buf_pos += 2;
				}
				if (wavstream[i].buf_pos >= s->buf_len) {
					wavstream[i].buf_pos = 0;
					if (!(wavstream[i].op & OP_REPEAT)) {
						wavstream[i].sample = 0;
					}
				}
			}
			val[0] = CLAMP(val[0], -32768.0, 32767.0);
			val[1] = CLAMP(val[1], -32768.0, 32767.0);
			((Sint16*)dsp_buf)[written/2] = (Sint16)val[0];
			written+=2;
			((Sint16*)dsp_buf)[written/2] = (Sint16)val[1];
			written+=2;
		}
	}
}

void DestroyAllEvents()
{
	/* silence any sound events */
	for (int idx=0; idx<MAX_WAVSTREAMS; idx++) {
		wavstream[idx].sample = 0;
	}
}

static void load_sound(const std::string &basename, const std::string &path)
{
	if (is_file(path)) {
		if (basename.size() < 4) return;
		if (basename.substr(basename.size()-4) != ".ogg") return;
		printf("Loading %s\n", path.c_str());

		Sample sample;
		OggVorbis_File oggv;

		FILE *f = fopen_or_die(path.c_str(), "rb");
		if (ov_open(f, &oggv, NULL, 0) < 0) {
			Error("Vorbis could not open %s", path.c_str());
		}
		struct vorbis_info *info;
		info = ov_info(&oggv, -1);

		if ((info->rate != FREQ) && (info->rate != (FREQ>>1))) {
			Error("Vorbis file %s is not %dHz or %dHz. Bad!", path.c_str(), FREQ, FREQ>>1);
		}
		if ((info->channels < 1) || (info->channels > 2)) {
			Error("Vorbis file %s is not mono or stereo. Bad!", path.c_str());
		}
		
		int resample_multiplier = ((info->rate == (FREQ>>1)) ? 2 : 1);
		const Sint64 num_samples = ov_pcm_total(&oggv, -1);
		// since samples are 16 bits we have:
		sample.buf = new Uint8[num_samples*2*resample_multiplier];
		sample.buf_len = num_samples*2;
		sample.channels = info->channels;

		int i=0;
		for (;;) {
			int music_section;
			int amt = ov_read(&oggv, (char*)&sample.buf[i],
					sample.buf_len - i, 0, 2, 1, &music_section);
			i += amt;
			if (amt == 0) break;
		}

		/* for sample rate of exactly half native pioneer rate (ie
		 * 22050), do a dodgy up-sampling */
		if (resample_multiplier == 2) {
			Uint16 *buf = (Uint16*)sample.buf;
			for (int i=num_samples-1; i>=0; i--) {
				Uint16 s = buf[i];
				buf[i*2] = s;
				buf[i*2+1] = s;
			}
			sample.buf_len *= 2;
		}

		// sample keyed by basename minus the .ogg
		sfx_samples[basename.substr(0, basename.size()-4)] = sample;

		ov_clear(&oggv);

	} else if (is_dir(path)) {
		foreach_file_in(path, &load_sound);
	}
}

bool Init ()
{
	static bool isInitted = false;

	if (!isInitted) {
		isInitted = true;
		SDL_AudioSpec wanted;
		
		if (SDL_Init (SDL_INIT_AUDIO) == -1) {
			fprintf (stderr, "Count not initialise SDL: %s.\n", SDL_GetError ());
			return false;
		}

		wanted.freq = FREQ;
		wanted.channels = 2;
		wanted.format = AUDIO_S16;
		wanted.samples = BUF_SIZE;
		wanted.callback = fill_audio;
		wanted.userdata = NULL;

		if (SDL_OpenAudio (&wanted, NULL) < 0) {
			fprintf (stderr, "Could not open audio: %s\n", SDL_GetError ());
			return false;
		}

		// load all the wretched effects
		foreach_file_in(PIONEER_DATA_DIR "/sounds", &load_sound);
	}

	/* silence any sound events */
	DestroyAllEvents();

	return true;
}

void Close ()
{
	SDL_CloseAudio ();
}

void Pause (int on)
{
	SDL_PauseAudio (on);
}

void Event::Play(const char *fx, float volume_left, float volume_right, Op op)
{
	Stop();
	eid = PlaySfx(fx, volume_left, volume_right, op);
}

bool Event::Stop()
{
	if (eid) {
		SDL_LockAudio();
		SoundEvent *s = GetEvent(eid);
		if (s) {
			s->sample = 0;
		}
		SDL_UnlockAudio();
		return s != 0;
	} else {
		return false;
	}
}

bool Event::IsPlaying() const
{
	if (eid == 0) return false;
	else return GetEvent(eid) != 0;
}

bool Event::SetOp(Op op) {
	if (eid == 0) return false;
	bool ret = false;
	SDL_LockAudio();
	SoundEvent *se = GetEvent(eid);
	if (se) {
		se->op = op;
		ret = true;
	}
	SDL_UnlockAudio();
	return ret;
}

bool Event::VolumeAnimate(float targetVols[2], float dv_dt[2])
{
	SDL_LockAudio();
	SoundEvent *ev = GetEvent(eid);
	if (ev) {
		ev->targetVolume[0] = targetVols[0];
		ev->targetVolume[1] = targetVols[1];
		ev->rateOfChange[0] = dv_dt[0] / (float)FREQ;
		ev->rateOfChange[1] = dv_dt[1] / (float)FREQ;
	}
	SDL_UnlockAudio();
	return (ev != 0);
}

bool Event::SetVolume(float vol_left, float vol_right)
{
	SDL_LockAudio();
	bool status = false;
	for (int i=0; i<MAX_WAVSTREAMS; i++) {
		if (wavstream[i].sample && (wavstream[i].identifier == eid)) {
			wavstream[i].volume[0] = vol_left;
			wavstream[i].volume[1] = vol_right;
			status = true;
			break;
		}
	}
	SDL_UnlockAudio();
	return status;
}

} /* namespace Sound */
