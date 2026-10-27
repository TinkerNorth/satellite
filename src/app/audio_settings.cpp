// SPDX-License-Identifier: LGPL-3.0-or-later
#include "app/audio_settings.h"

#include "app/app_state.h"

ControllerAudioPolicy audioPolicyFromConfig() { return g_configStore->audioPolicy(); }

bool controllerAudioEnabled() { return g_configStore->current()->controllerAudio; }

bool keepDefaultAudioDevice() { return g_configStore->current()->controllerAudioKeepDefaultDevice; }
