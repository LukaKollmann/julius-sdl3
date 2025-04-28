#include "core/file.h"
#include "core/log.h"
#include "sound/device.h"
#include "game/settings.h"
#include "platform/platform.h"
#include "platform/vita/vita.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

#define AUDIO_RATE 22050
#define AUDIO_FORMAT SDL_AUDIO_S16LE
#define AUDIO_CHANNELS 2

#define MAX_CHANNELS 150

#ifdef __vita__
static struct {
    char filename[FILE_NAME_MAX];
    char *buffer;
    int size;
} vita_music_data;
#endif

// Emulate SDL_RWFromFP
#if defined(__vita__) || defined(__ANDROID__)
typedef struct IOStreamStdioFPData {
    FILE *fp;
    bool autoclose;
} IOStreamStdioFPData;

static Sint64 SDLCALL stdio_seek(void *userdata, Sint64 offset, int whence)
{
    FILE *fp = ((IOStreamStdioFPData *) userdata)->fp;
    int stdiowhence;

    switch (whence) {
        case SDL_IO_SEEK_SET:
            stdiowhence = SEEK_SET;
            break;
        case SDL_IO_SEEK_CUR:
            stdiowhence = SEEK_CUR;
            break;
        case SDL_IO_SEEK_END:
            stdiowhence = SEEK_END;
            break;
        default:
            SDL_SetError("Unknown value for 'whence'");
            return -1;
    }

    if (fseek(fp, (fseek_off_t) offset, stdiowhence) == 0) {
        const Sint64 pos = ftell(fp);
        if (pos < 0) {
            SDL_SetError("Couldn't get stream offset");
            return -1;
        }
        return pos;
    }
    SDL_SetError("Couldn't seek in stream");
    return -1;
}

static size_t SDLCALL stdio_read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    FILE *fp = ((IOStreamStdioFPData *) userdata)->fp;
    const size_t bytes = fread(ptr, 1, size, fp);
    if (bytes == 0 && ferror(fp)) {
        SDL_SetError("Couldn't read stream");
    }
    return bytes;
}

static size_t SDLCALL stdio_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    FILE *fp = ((IOStreamStdioFPData *) userdata)->fp;
    const size_t bytes = fwrite(ptr, 1, size, fp);
    if (bytes == 0 && ferror(fp)) {
        SDL_SetError("Couldn't write stream");
    }
    return bytes;
}

static bool SDLCALL stdio_close(void *userdata)
{
    IOStreamStdioData *rwopsdata = (IOStreamStdioData *) userdata;
    bool status = true;
    if (rwopsdata->autoclose) {
        if (fclose(rwopsdata->fp) != 0) {
            SDL_SetError("Couldn't close stream");
            status = false;
        }
    }
    return status;
}

SDL_IOStream *SDL_RWFromFP(FILE *fp, bool autoclose)
{
    SDL_IOStreamInterface iface;
    IOStreamStdioFPData *rwopsdata;
    SDL_IOStream *rwops;

    rwopsdata = (IOStreamStdioFPData *) SDL_malloc(sizeof(*rwopsdata));
    if (!rwopsdata) {
        return NULL;
    }

    SDL_INIT_INTERFACE(&iface);
    /* There's no stdio_size because SDL_GetIOSize emulates it the same way we'd do it for stdio anyhow. */
    iface.seek = stdio_seek;
    iface.read = stdio_read;
    iface.write = stdio_write;
    iface.close = stdio_close;

    rwopsdata->fp = fp;
    rwopsdata->autoclose = autoclose;

    rwops = SDL_OpenIO(&iface, rwopsdata);
    if (!rwops) {
        iface.close(rwopsdata);
    }
    return rwops;
}
#endif

typedef struct {
    const char *filename;
    Mix_Chunk *chunk;
} sound_channel;

static struct {
    int initialized;
    Mix_Music *music;
    sound_channel channels[MAX_CHANNELS];
} data;

static struct {
    SDL_AudioFormat format;
    SDL_AudioFormat dst_format;
    SDL_AudioStream *stream;
} custom_music;

static float percentage_to_volume(int percentage)
{
    return percentage / 100.f;
}

static void init_channels(void)
{
    data.initialized = 1;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        data.channels[i].chunk = 0;
    }
}

void sound_device_open(void)
{
    SDL_AudioSpec spec = {
      .freq = AUDIO_RATE,
      .format = AUDIO_FORMAT,
      .channels = AUDIO_CHANNELS,
    };
    if (Mix_OpenAudio(0,&spec)) {
        init_channels();
        return;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Sound failed to initialize using default driver: %s", SDL_GetError());
    int max = SDL_GetNumAudioDrivers();
    SDL_Log("Number of audio devices: %d", max);
    for (int i = 0; i < max; i++) {
        SDL_Log("Audio device: %s", SDL_GetAudioDeviceName(i));
    }
}

void sound_device_close(void)
{
    if (data.initialized) {
        for (int i = 0; i < MAX_CHANNELS; i++) {
            sound_device_stop_channel(i);
        }
        Mix_CloseAudio();
        data.initialized = 0;
    }
}

static Mix_Chunk *load_chunk(const char *filename)
{
    if (filename[0]) {
#if defined(__vita__) || defined(__ANDROID__)
        FILE *fp = file_open(filename, "rb");
        if (!fp) {
            return NULL;
        }
        SDL_IOStream *sdl_fp = SDL_RWFromFP(fp, SDL_TRUE);
        return Mix_LoadWAV_IO(sdl_fp, 1);
#else
        return Mix_LoadWAV(filename);
#endif
    } else {
        return NULL;
    }
}

static int load_channel(sound_channel *channel)
{
    if (!channel->chunk && channel->filename) {
        channel->chunk = load_chunk(channel->filename);
    }
    return channel->chunk ? 1 : 0;
}

void sound_device_init_channels(int num_channels, char filenames[][CHANNEL_FILENAME_MAX])
{
    if (data.initialized) {
        if (num_channels > MAX_CHANNELS) {
            num_channels = MAX_CHANNELS;
        }
        Mix_AllocateChannels(num_channels);
        log_info("Loading audio files", 0, 0);
        for (int i = 0; i < num_channels; i++) {
            data.channels[i].chunk = 0;
            data.channels[i].filename = filenames[i][0] ? filenames[i] : 0;
        }
    }
}

int sound_device_is_channel_playing(int channel)
{
    return data.channels[channel].chunk && Mix_Playing(channel);
}

void sound_device_set_music_volume(int volume_pct)
{
    Mix_VolumeMusic(percentage_to_volume(volume_pct) * MIX_MAX_VOLUME);
}

void sound_device_set_channel_volume(int channel, int volume_pct)
{
    if (data.channels[channel].chunk) {
        Mix_VolumeChunk(data.channels[channel].chunk, percentage_to_volume(volume_pct) * MIX_MAX_VOLUME);
    }
}

#ifdef __vita__
static void load_music_for_vita(const char *filename)
{
    if (vita_music_data.buffer) {
        if (strcmp(filename, vita_music_data.filename) == 0) {
            return;
        }
        free(vita_music_data.buffer);
        vita_music_data.buffer = 0;
    }
    strncpy(vita_music_data.filename, filename, FILE_NAME_MAX - 1);
    SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0777);
    if (fd < 0) {
        return;
    }
    vita_music_data.size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    vita_music_data.buffer = malloc(sizeof(char) * vita_music_data.size);
    sceIoRead(fd, vita_music_data.buffer, vita_music_data.size);
    sceIoClose(fd);
}
#endif

int sound_device_play_music(const char *filename, int volume_pct)
{
    if (data.initialized) {
        sound_device_stop_music();
        if (!filename) {
            return 0;
        }
#ifdef __vita__
        load_music_for_vita(filename);
        if (!vita_music_data.buffer) {
            return 0;
        }
        SDL_IOStream *sdl_music = SDL_IOFromMem(vita_music_data.buffer, vita_music_data.size);
        data.music = Mix_LoadMUSType_IO(sdl_music, file_has_extension(filename, "mp3") ? MUS_MP3 : MUS_WAV, true);
#elif defined(__ANDROID__)
        FILE *fp = file_open(filename, "rb");
        if (!fp) {
            return 0;
        }
        SDL_IOStream *sdl_fp = SDL_RWFromFP(fp, SDL_TRUE);
        data.music = Mix_LoadMUSType_IO(sdl_fp, file_has_extension(filename, "mp3") ? MUS_MP3 : MUS_WAV, true);
#else
        data.music = Mix_LoadMUS(filename);
#endif
        if (!data.music) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Error opening music file '%s'. Reason: %s", filename, SDL_GetError());
        } else {
            if (!Mix_PlayMusic(data.music, -1)) {
                data.music = 0;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Error playing music file '%s'. Reason: %s", filename, SDL_GetError());
            } else {
                sound_device_set_music_volume(volume_pct);
            }
        }
        return data.music ? 1 : 0;
    }
    return 0;
}

void sound_device_play_file_on_channel(const char *filename, int channel, int volume_pct)
{
    if (data.initialized) {
        sound_device_stop_channel(channel);
        data.channels[channel].chunk = load_chunk(filename);
        if (data.channels[channel].chunk) {
            sound_device_set_channel_volume(channel, volume_pct);
            Mix_PlayChannel(channel, data.channels[channel].chunk, 0);
        }
    }
}

void sound_device_play_channel(int channel, int volume_pct)
{
    if (data.initialized) {
        sound_channel *ch = &data.channels[channel];
        if (load_channel(ch)) {
            sound_device_set_channel_volume(channel, volume_pct);
            Mix_PlayChannel(channel, ch->chunk, 0);
        }
    }
}

void sound_device_play_channel_panned(int channel, int volume_pct, int left_pct, int right_pct)
{
    if (data.initialized) {
        sound_channel *ch = &data.channels[channel];
        if (load_channel(ch)) {
            Mix_SetPanning(channel, left_pct * 255 / 100, right_pct * 255 / 100);
            sound_device_set_channel_volume(channel, volume_pct);
            Mix_PlayChannel(channel, ch->chunk, 0);
        }
    }
}

void sound_device_stop_music(void)
{
    if (data.initialized) {
        if (data.music) {
            Mix_HaltMusic();
            Mix_FreeMusic(data.music);
            data.music = 0;
        }
    }
}

void sound_device_stop_channel(int channel)
{
    if (data.initialized) {
        sound_channel *ch = &data.channels[channel];
        if (ch->chunk) {
            Mix_HaltChannel(channel);
            Mix_FreeChunk(ch->chunk);
            ch->chunk = 0;
        }
    }
}

static void free_custom_audio_stream(void)
{
  if (custom_music.stream) {
      SDL_DestroyAudioStream(custom_music.stream);
      custom_music.stream = 0;
  }
}

static int create_custom_audio_stream(SDL_AudioFormat src_format, Uint8 src_channels, int src_rate,
                                      SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate)
{
    free_custom_audio_stream();

    custom_music.dst_format = dst_format;

    SDL_AudioSpec src_spec = {
        .freq = src_rate,
        .format = src_format,
        .channels = src_channels,
    };
    SDL_AudioSpec dst_spec = {
        .freq = dst_rate,
        .format = dst_format,
        .channels = dst_channels,
    };
    custom_music.stream = SDL_CreateAudioStream(&src_spec,&dst_spec);
    return custom_music.stream != 0;
}

static int custom_audio_stream_active(void)
{
    return custom_music.stream != 0;
}

static int put_custom_audio_stream(const Uint8 *audio_data, int len)
{
    if (!audio_data || len <= 0 || !custom_audio_stream_active()) {
        return 0;
    }

    return SDL_PutAudioStreamData(custom_music.stream, audio_data, len) == 0;
}

static int get_custom_audio_stream(Uint8 *dst, int len)
{
    // Write silence
    memset(dst, 0, len);

    if (!dst || len <= 0 || !custom_audio_stream_active()) {
        return 0;
    }
    int bytes_copied = 0;

    // Mix audio to sound effect volume
    Uint8 *mix_buffer = (Uint8 *) malloc(len);
    if (!mix_buffer) {
        return 0;
    }
    memset(mix_buffer, 0, len);

    bytes_copied = SDL_GetAudioStreamData(custom_music.stream, mix_buffer, len);
    if (bytes_copied <= 0) {
        return 0;
    }

    SDL_MixAudio(dst, mix_buffer,
        custom_music.dst_format, bytes_copied,
        percentage_to_volume(setting_sound(SOUND_EFFECTS)->volume));
    free(mix_buffer);

    return bytes_copied;
}

static void custom_music_callback(void *dummy, Uint8 *stream, int len)
{
    get_custom_audio_stream(stream, len);
}

void sound_device_use_custom_music_player(int bitdepth, int num_channels, int rate,
                                          const unsigned char *audio_data, int len)
{
    SDL_AudioFormat format;
    if (bitdepth == 8) {
        format = SDL_AUDIO_U8;
    } else if (bitdepth == 16) {
        format = SDL_AUDIO_S16;
    } else {
        log_error("Custom music bitdepth not supported:", 0, bitdepth);
        return;
    }
    int device_rate;
    SDL_AudioFormat device_format;
    int device_channels;
    Mix_QuerySpec(&device_rate, &device_format, &device_channels);
    custom_music.format = format;

    int result = create_custom_audio_stream(
        format, num_channels, rate,
        device_format, device_channels, device_rate
    );
    if (!result) {
        return;
    }

    sound_device_write_custom_music_data(audio_data, len);

    Mix_HookMusic(custom_music_callback, 0);
}

void sound_device_write_custom_music_data(const unsigned char *audio_data, int len)
{
    if (!audio_data || len <= 0 || !custom_audio_stream_active()) {
        return;
    }
    put_custom_audio_stream(audio_data, len);
}

void sound_device_use_default_music_player(void)
{
    Mix_HookMusic(0, 0);
    free_custom_audio_stream();
}
