// SPDX-License-Identifier: LGPL-3.0-or-later
#include "app/audio_settings.h"

#include "app/app_state.h"

#include <mutex>

ControllerAudioPolicy audioPolicyFromConfig() {
    std::lock_guard<TrackedMutex> lk(g_configMtx);
    return ControllerAudioPolicy{g_config.controllerAudioMic, g_config.controllerAudioSpeaker,
                                 g_config.controllerAudioHaptics};
}

bool controllerAudioEnabled() {
    std::lock_guard<TrackedMutex> lk(g_configMtx);
    return g_config.controllerAudio;
}

bool keepDefaultAudioDevice() {
    std::lock_guard<TrackedMutex> lk(g_configMtx);
    return g_config.controllerAudioKeepDefaultDevice;
}
