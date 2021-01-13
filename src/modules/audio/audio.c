#include "audio/audio.h"
#include "data/soundData.h"
#include "data/blob.h"
#include "core/arr.h"
#include "core/ref.h"
#include "core/os.h"
#include "core/util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "lib/miniaudio/miniaudio.h"
#include "audio/spatializer.h"

static const ma_format miniAudioFormatFromLovr[] = {
  [SAMPLE_I16] = ma_format_s16,
  [SAMPLE_F32] = ma_format_f32
};

#define OUTPUT_FORMAT SAMPLE_F32
#define OUTPUT_CHANNELS 2
#define CAPTURE_CHANNELS 1

struct Source {
  Source* next;
  SoundData* sound;
  ma_data_converter* converter;
  uint32_t offset;
  float volume;
  bool tracked;
  bool playing;
  bool looping;
  bool spatial;
  float transform[16];
};

typedef struct {
  char *deviceName;
  int sampleRate;
  SampleFormat format;
} AudioConfig;

static inline int outputChannelCountForSource(Source *source) { return source->spatial ? 1 : OUTPUT_CHANNELS; }

static struct {
  bool initialized;
  ma_context context;
  AudioConfig config[AUDIO_TYPE_COUNT];
  ma_device devices[AUDIO_TYPE_COUNT];
  ma_mutex playbackLock;
  Source* sources;
  ma_pcm_rb captureRingbuffer;
  arr_t(ma_data_converter*) converters;
  Spatializer* spatializer;
} state;

// Device callbacks

static bool mix(Source* source, float* output, uint32_t count) {
  float raw[2048];
  float aux[2048];
  float mix[4096];

  // TODO
  // frameLimitIn =
  // frameLimitOut =

  while (count > 0) {
    uint32_t chunk = MIN(sizeof(raw) / SampleFormatBytesPerFrame(source->sound->channels, source->sound->format),
        ma_data_converter_get_required_input_frame_count(source->converter, count));
        // ^^^ Note need to min `count` with 'capacity of aux buffer' and 'capacity of mix buffer'
        // could skip min-ing with one of the buffers if you can guarantee that one is bigger/equal to the other (you can because their formats are known)
    ma_uint64 framesIn = source->sound->read(source->sound, source->offset, chunk, raw);
    ma_uint64 framesOut = sizeof(aux) / (sizeof(float) * outputChannelCountForSource(source));

    ma_data_converter_process_pcm_frames(source->converter, raw, &framesIn, aux, &framesOut);

    if (source->spatial) {
      state.spatializer->apply(source, source->transform, aux, mix, framesOut);
    } else {
      memcpy(mix, aux, framesOut * SampleFormatBytesPerFrame(OUTPUT_CHANNELS, SAMPLE_F32));
    }

    for (uint32_t i = 0; i < framesOut * OUTPUT_CHANNELS; i++) {
      output[i] += mix[i] * source->volume;
    }

    if (framesIn == 0) {
      source->offset = 0;
      if (!source->looping) {
        source->playing = false;
        return false;
      }
    } else {
      source->offset += framesIn;
    }

    count -= framesOut;
    output += framesOut * OUTPUT_CHANNELS;
  }

  return true;
}

static void onPlayback(ma_device* device, void* output, const void* _, uint32_t count) {
  ma_mutex_lock(&state.playbackLock);

  // For each Source, remove it if it isn't playing or process it and remove it if it stops
  for (Source** list = &state.sources, *source = *list; source != NULL; source = *list) {
    if (source->playing && mix(source, output, count)) {
      list = &source->next;
    } else {
      *list = source->next;
      source->tracked = false;
      lovrRelease(Source, source);
    }
  }

  ma_mutex_unlock(&state.playbackLock);
}

static void onCapture(ma_device* device, void* output, const void* input, uint32_t frames) {
  // note: ma_pcm_rb is lockless
  void *store;
  size_t bytesPerFrame = SampleFormatBytesPerFrame(CAPTURE_CHANNELS, state.config[AUDIO_CAPTURE].format);
  while(frames > 0) {
    uint32_t availableFrames = frames;
    ma_result acquire_status = ma_pcm_rb_acquire_write(&state.captureRingbuffer, &availableFrames, &store);
    if (acquire_status != MA_SUCCESS) {
      return;
    }
    memcpy(store, input, availableFrames * bytesPerFrame);
    ma_result commit_status = ma_pcm_rb_commit_write(&state.captureRingbuffer, availableFrames, store);
    if (commit_status != MA_SUCCESS || availableFrames == 0) {
      return;
    }
    frames -= availableFrames;
    input += availableFrames * bytesPerFrame;
  }
}

static const ma_device_callback_proc callbacks[] = { onPlayback, onCapture };

static Spatializer *spatializers[] = {
  &dummySpatializer,
};

// Entry

bool lovrAudioInit() {
  if (state.initialized) return false;

  state.config[AUDIO_PLAYBACK] = (AudioConfig){ .format = SAMPLE_F32, .sampleRate = 44100 };
  state.config[AUDIO_CAPTURE] = (AudioConfig){ .format = SAMPLE_F32, .sampleRate = 44100 };

  if (ma_context_init(NULL, 0, NULL, &state.context)) {
    return false;
  }

  int mutexStatus = ma_mutex_init(&state.playbackLock);
  lovrAssert(mutexStatus == MA_SUCCESS, "Failed to create audio mutex");

  ma_result rbstatus = ma_pcm_rb_init(miniAudioFormatFromLovr[OUTPUT_FORMAT], CAPTURE_CHANNELS, state.config[AUDIO_CAPTURE].sampleRate * 1.0, NULL, NULL, &state.captureRingbuffer);
  if (rbstatus != MA_SUCCESS) {
    lovrAudioDestroy();
    return false;
  }

  for (size_t i = 0; i < sizeof(spatializers) / sizeof(spatializers[0]); i++) {
    if (spatializers[i]->init()) {
      state.spatializer = spatializers[i];
      break;
    }
  }
  lovrAssert(state.spatializer != NULL, "Must have at least one spatializer");

  arr_init(&state.converters);

  return state.initialized = true;
}

void lovrAudioDestroy() {
  if (!state.initialized) return;
  lovrAudioStop(AUDIO_PLAYBACK);
  lovrAudioStop(AUDIO_CAPTURE);

  ma_mutex_uninit(&state.playbackLock);
  ma_context_uninit(&state.context);
  if (state.spatializer) state.spatializer->destroy();
  for(int i = 0; i < state.converters.length; i++) {
    ma_data_converter_uninit(state.converters.data[i]);
    free(state.converters.data[i]);
  }
  arr_free(&state.converters);
  free(state.config[0].deviceName);
  free(state.config[1].deviceName);
  memset(&state, 0, sizeof(state));
}

bool lovrAudioInitDevice(AudioType type) {

  ma_device_info *playbackDevices;
  ma_uint32 playbackDeviceCount;
  ma_device_info *captureDevices;
  ma_uint32 captureDeviceCount;
  ma_result gettingStatus = ma_context_get_devices(&state.context, &playbackDevices, &playbackDeviceCount, &captureDevices, &captureDeviceCount);
  lovrAssert(gettingStatus == MA_SUCCESS, "Failed to enumerate audio devices during initialization: %s (%d)", ma_result_description(gettingStatus), gettingStatus);

  ma_device_config config;
  if (type == AUDIO_PLAYBACK) {
    ma_device_type deviceType = ma_device_type_playback;
    config = ma_device_config_init(deviceType);

    lovrAssert(state.config[AUDIO_PLAYBACK].format == OUTPUT_FORMAT, "Only f32 playback format currently supported");
    config.playback.format = miniAudioFormatFromLovr[state.config[AUDIO_PLAYBACK].format];
    for(int i = 0; i < playbackDeviceCount && state.config[AUDIO_PLAYBACK].deviceName; i++) {
      if (strcmp(playbackDevices[i].name, state.config[AUDIO_PLAYBACK].deviceName) == 0) {
        config.playback.pDeviceID = &playbackDevices[i].id;
      }
    }
    if (state.config[AUDIO_PLAYBACK].deviceName && config.playback.pDeviceID == NULL) {
      lovrLog(LOG_WARN, "audio", "No audio playback device called '%s'; falling back to default.", state.config[AUDIO_PLAYBACK].deviceName);
    }
    config.playback.channels = OUTPUT_CHANNELS;
  } else { // if AUDIO_CAPTURE
    ma_device_type deviceType = ma_device_type_capture;
    config = ma_device_config_init(deviceType);

    config.capture.format = miniAudioFormatFromLovr[state.config[AUDIO_CAPTURE].format];
    for(int i = 0; i < captureDeviceCount && state.config[AUDIO_CAPTURE].deviceName; i++) {
      if (strcmp(captureDevices[i].name, state.config[AUDIO_CAPTURE].deviceName) == 0) {
        config.capture.pDeviceID = &playbackDevices[i].id;
      }
    }
    if (state.config[AUDIO_CAPTURE].deviceName && config.capture.pDeviceID == NULL) {
      lovrLog(LOG_WARN, "audio", "No audio capture device called '%s'; falling back to default.", state.config[AUDIO_CAPTURE].deviceName);
    }
    config.capture.channels = CAPTURE_CHANNELS;
  }
  config.performanceProfile = ma_performance_profile_low_latency;
  config.dataCallback = callbacks[type];
  config.sampleRate = state.config[type].sampleRate;


  ma_result err = ma_device_init(&state.context, &config, &state.devices[type]);
  if (err != MA_SUCCESS) {
    lovrLog(LOG_WARN, "audio", "Failed to enable %s audio device: %s (%d)\n", type == AUDIO_PLAYBACK ? "playback" : "capture", ma_result_description(err), err);
    return false;
  }
  return true;
}

bool lovrAudioStart(AudioType type) {
  bool initResult = lovrAudioInitDevice(type);
  if (initResult == false && type == AUDIO_CAPTURE) {
    lovrPlatformRequestPermission(AUDIO_CAPTURE_PERMISSION);
    // lovrAudioStart will be retried from boot.lua upon permission granted event
    return false;
  }
  ma_result status = ma_device_start(&state.devices[type]);
  return status == MA_SUCCESS;
}

bool lovrAudioStop(AudioType type) {
  ma_result stoppingResult = ma_device_stop(&state.devices[type]);
  ma_device_uninit(&state.devices[type]);

  return stoppingResult == MA_SUCCESS;
}

float lovrAudioGetVolume() {
  float volume = 0.f;
  ma_device_get_master_volume(&state.devices[AUDIO_PLAYBACK], &volume);
  return volume;
}

void lovrAudioSetVolume(float volume) {
  ma_device_set_master_volume(&state.devices[AUDIO_PLAYBACK], volume);
}

void lovrAudioSetListenerPose(float position[4], float orientation[4]) {
  state.spatializer->setListenerPose(position, orientation);
}

double lovrAudioConvertToSeconds(uint32_t sampleCount, AudioType context) {
  return sampleCount / (double)state.config[context].sampleRate;
}

// Source

static void _lovrSourceAssignConverter(Source *source) {
  source->converter = NULL;
  for (size_t i = 0; i < state.converters.length; i++) {
    ma_data_converter* converter = state.converters.data[i];
    if (converter->config.formatIn != miniAudioFormatFromLovr[source->sound->format]) continue;
    if (converter->config.sampleRateIn != source->sound->sampleRate) continue;
    if (converter->config.channelsIn != source->sound->channels) continue;
    if (converter->config.channelsOut != outputChannelCountForSource(source)) continue;
    source->converter = converter;
    break;
  }

  if (!source->converter) {
    ma_data_converter_config config = ma_data_converter_config_init_default();
    config.formatIn = miniAudioFormatFromLovr[source->sound->format];
    config.formatOut = miniAudioFormatFromLovr[OUTPUT_FORMAT];
    config.channelsIn = source->sound->channels;
    config.channelsOut = outputChannelCountForSource(source);
    config.sampleRateIn = source->sound->sampleRate;
    config.sampleRateOut = state.config[AUDIO_PLAYBACK].sampleRate;

    ma_data_converter *converter = malloc(sizeof(ma_data_converter));
    ma_result converterStatus = ma_data_converter_init(&config, converter);
    lovrAssert(converterStatus == MA_SUCCESS, "Problem creating Source data converter #%d: %s (%d)", state.converters.length, ma_result_description(converterStatus), converterStatus);

    arr_expand(&state.converters, 1);
    state.converters.data[state.converters.length++] = source->converter = converter;
  }
}

Source* lovrSourceCreate(SoundData* sound, bool spatial) {
  Source* source = lovrAlloc(Source);
  source->sound = sound;
  lovrRetain(source->sound);
  source->volume = 1.f;

  source->spatial = spatial;
  mat4_identity(source->transform);
  _lovrSourceAssignConverter(source);

  return source;
}

void lovrSourceDestroy(void* ref) {
  Source* source = ref;
  lovrRelease(SoundData, source->sound);
}

void lovrSourcePlay(Source* source) {
  ma_mutex_lock(&state.playbackLock);

  source->playing = true;

  if (!source->tracked) {
    lovrRetain(source);
    source->tracked = true;
    source->next = state.sources;
    state.sources = source;
  }

  ma_mutex_unlock(&state.playbackLock);
}

void lovrSourcePause(Source* source) {
  source->playing = false;
}

void lovrSourceStop(Source* source) {
  lovrSourcePause(source);
  lovrSourceSetTime(source, 0);
}

bool lovrSourceIsPlaying(Source* source) {
  return source->playing;
}

bool lovrSourceIsLooping(Source* source) {
  return source->looping;
}

void lovrSourceSetLooping(Source* source, bool loop) {
  lovrAssert(loop == false || lovrSoundDataIsStream(source->sound) == false, "Can't loop streams");
  source->looping = loop;
}

float lovrSourceGetVolume(Source* source) {
  return source->volume;
}

void lovrSourceSetVolume(Source* source, float volume) {
  ma_mutex_lock(&state.playbackLock);
  source->volume = volume;
  ma_mutex_unlock(&state.playbackLock);
}

bool lovrSourceGetSpatial(Source *source) {
  return source->spatial;
}

void lovrSourceSetPose(Source *source, float position[4], float orientation[4]) {
  ma_mutex_lock(&state.playbackLock);
  mat4_identity(source->transform);
  mat4_translate(source->transform, position[0], position[1], position[2]);
  mat4_rotate(source->transform, orientation[0], orientation[1], orientation[2], orientation[3]);
  ma_mutex_unlock(&state.playbackLock);
}

uint32_t lovrSourceGetTime(Source* source) {
  if (lovrSoundDataIsStream(source->sound)) {
    return 0;
  } else {
    return source->offset;
  }
}

void lovrSourceSetTime(Source* source, uint32_t time) {
  ma_mutex_lock(&state.playbackLock);
  source->offset = time;
  ma_mutex_unlock(&state.playbackLock);
}

SoundData* lovrSourceGetSoundData(Source* source) {
  return source->sound;
}

// Capture

uint32_t lovrAudioGetCaptureSampleCount() {
  // note: must only be called from ONE thread!! ma_pcm_rb only promises
  // thread safety with ONE reader and ONE writer thread.
  return ma_pcm_rb_available_read(&state.captureRingbuffer);
}

static const char *format2string(SampleFormat f) { switch(f) {
  case SAMPLE_F32: return "f32";
  case SAMPLE_I16: return "i16";
  case SAMPLE_INVALID: return "invalid";
}}

struct SoundData* lovrAudioCapture(uint32_t frameCount, SoundData *soundData, uint32_t offset) {

  uint32_t bufferedFrames = lovrAudioGetCaptureSampleCount();
  if (frameCount == 0 || frameCount > bufferedFrames) {
    frameCount = bufferedFrames;
  }

  if (frameCount == 0) {
    return NULL;
  }

  if (soundData == NULL) {
    soundData = lovrSoundDataCreateRaw(frameCount, CAPTURE_CHANNELS, state.config[AUDIO_CAPTURE].sampleRate, state.config[AUDIO_CAPTURE].format, NULL);
  } else {
    lovrAssert(soundData->channels == CAPTURE_CHANNELS, "Capture (%d) and SoundData (%d) channel counts must match", CAPTURE_CHANNELS, soundData->channels);
    lovrAssert(soundData->sampleRate == state.config[AUDIO_CAPTURE].sampleRate, "Capture (%d) and SoundData (%d) sample rates must match", state.config[AUDIO_CAPTURE].sampleRate, soundData->sampleRate);
    lovrAssert(soundData->format == state.config[AUDIO_CAPTURE].format, "Capture (%s) and SoundData (%s) formats must match", format2string(state.config[AUDIO_CAPTURE].format), format2string(soundData->format));
    lovrAssert(offset + frameCount <= soundData->frames, "Tried to write samples past the end of a SoundData buffer");
  }

  uint32_t bytesPerFrame = SampleFormatBytesPerFrame(CAPTURE_CHANNELS, state.config[AUDIO_CAPTURE].format);
  while(frameCount > 0) {
    uint32_t availableFramesInRB = frameCount;
    void *store;
    ma_result acquire_status = ma_pcm_rb_acquire_read(&state.captureRingbuffer, &availableFramesInRB, &store);
    if (acquire_status != MA_SUCCESS) {
      lovrThrow("Failed to acquire ring buffer for read: %s (%d)\n", ma_result_description(acquire_status), acquire_status);
      return NULL;
    }
    memcpy(soundData->blob->data + offset * bytesPerFrame, store, availableFramesInRB * bytesPerFrame);
    ma_result commit_status = ma_pcm_rb_commit_read(&state.captureRingbuffer, availableFramesInRB, store);
    if (commit_status != MA_SUCCESS) {
      lovrThrow("Failed to commit ring buffer for read: %s (%d)\n", ma_result_description(commit_status), commit_status);
      return NULL;
    }
    frameCount -= availableFramesInRB;
    offset += availableFramesInRB;
  }

  return soundData;
}

AudioDeviceArr* lovrAudioGetDevices(AudioType type) {
  ma_device_info *playbackDevices;
  ma_uint32 playbackDeviceCount;
  ma_device_info *captureDevices;
  ma_uint32 captureDeviceCount;
  ma_result gettingStatus = ma_context_get_devices(&state.context, &playbackDevices, &playbackDeviceCount, &captureDevices, &captureDeviceCount);
  lovrAssert(gettingStatus == MA_SUCCESS, "Failed to enumerate audio devices: %s (%d)", ma_result_description(gettingStatus), gettingStatus);

  ma_uint32 count = type == AUDIO_PLAYBACK ? playbackDeviceCount : captureDeviceCount;
  ma_device_info *madevices = type == AUDIO_PLAYBACK ? playbackDevices : captureDevices;
  AudioDeviceArr *devices = calloc(1, sizeof(AudioDeviceArr));
  devices->capacity = devices->length = count;
  devices->data = calloc(count, sizeof(AudioDevice));

  for(int i = 0; i < count; i++) {
    ma_device_info *mainfo = &madevices[i];
    AudioDevice *lovrInfo = &devices->data[i];
    lovrInfo->name = strdup(mainfo->name);
    lovrInfo->type = type;
    lovrInfo->isDefault = mainfo->isDefault;
  }
  return devices;
}

void lovrAudioFreeDevices(AudioDeviceArr *devices) {
  for(int i = 0; i < devices->length; i++) {
    free((void*)devices->data[i].name);
  }
  arr_free(devices);
}

void lovrAudioSetCaptureFormat(SampleFormat format, int sampleRate)
{
  if (sampleRate) state.config[AUDIO_CAPTURE].sampleRate = sampleRate;
  if (format != SAMPLE_INVALID) state.config[AUDIO_CAPTURE].format = format;

  // restart device if needed
  ma_uint32 previousState = state.devices[AUDIO_CAPTURE].state;
  if (previousState != MA_STATE_UNINITIALIZED && previousState != MA_STATE_STOPPED) {
    lovrAudioStop(AUDIO_CAPTURE);
    lovrAudioStart(AUDIO_CAPTURE);
  }
}

void lovrAudioUseDevice(AudioType type, const char *deviceName) {
  free(state.config[type].deviceName);
  state.config[type].deviceName = strdup(deviceName);

  // restart device if needed
  ma_uint32 previousState = state.devices[type].state;
  if (previousState != MA_STATE_UNINITIALIZED && previousState != MA_STATE_STOPPED) {
    lovrAudioStop(type);
    lovrAudioStart(type);
  }
}
