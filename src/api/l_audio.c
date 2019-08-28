#include "api.h"
#include "audio/audio.h"
#include "data/blob.h"
#include "data/soundData.h"
#include "core/maf.h"
#include "core/ref.h"
#include <stdlib.h>
#include "api/api.h"

const char* TimeUnits[] = {
  [TIME_FRAMES] = "frames",
  [TIME_SECONDS] = "seconds",
  NULL
};

static int l_lovrAudioNewSource(lua_State* L) {
  SoundData* soundData = luax_totype(L, 1, SoundData);
  Decoder* decoder = soundData ? lovrDecoderCreateRaw(soundData) : NULL;
  
  if (!decoder) {
    Blob* blob = luax_readblob(L, 1, "Sound");
    bool stream = lua_toboolean(L, 2);
  
    if (stream) {
      decoder = lovrDecoderCreateOgg(blob);
    } else {
      SoundData* soundData = lovrSoundDataCreateFromBlob(blob);
      decoder = lovrDecoderCreateRaw(soundData);
      lovrRelease(SoundData, soundData);
    }
  }

  lovrAssert(decoder->channels == 1, "Audio sources must be mono");
  
  Source* source = lovrSourceCreate(decoder);
  luax_pushtype(L, Source, source);
  lovrRelease(Source, source);
  lovrRelease(Decoder, decoder);
  return 1;
}

static const luaL_Reg lovrAudio[] = {
  { "newSource", l_lovrAudioNewSource },
  { NULL, NULL }
};

int luaopen_lovr_audio(lua_State* L) {
  lua_newtable(L);
  luaL_register(L, NULL, lovrAudio);
  luax_registertype(L, Source);
  if (lovrAudioInit()) {
    luax_atexit(L, lovrAudioDestroy);
  }
  return 1;
}
