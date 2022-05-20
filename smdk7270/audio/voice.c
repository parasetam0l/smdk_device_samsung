/*
 * Copyright (C) 2017 Christopher N. Hesse <raymanfx@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_hw_voice"
#define LOG_NDEBUG 0
/*#define VERY_VERY_VERBOSE_LOGGING*/
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include <stdlib.h>
#include <pthread.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include <samsung_audio.h>

#include "audio_hw.h"
#include "voice.h"

static struct pcm_config pcm_config_voicecall = {
    .channels = 1,
    .rate = 8000,
    .period_size = 256,
    .period_count = 2,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_voice_sco = {
    .channels = 2,
    .rate = SCO_DEFAULT_SAMPLING_RATE,
    .period_size = SCO_PERIOD_SIZE,
    .period_count = SCO_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

struct pcm_config pcm_config_voice_sco_wb = {
    .channels = 2,
    .rate = SCO_WB_SAMPLING_RATE,
    .period_size = SCO_PERIOD_SIZE,
    .period_count = SCO_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
};

/* Prototypes */
int start_voice_call(struct audio_device *adev);
int stop_voice_call(struct audio_device *adev);

/*
 * This decides based on the output device, if we enable
 * two mic control
 */
void prepare_voice_session(struct voice_session *session,
                           audio_devices_t active_out_devices)
{
    ALOGV("%s: active_out_devices: 0x%x", __func__, active_out_devices);

    session->out_device = active_out_devices;

    session->two_mic_control = false;
}

/*
 * This must be called with the hw device mutex locked, OK to hold other
 * mutexes.
 */
void stop_voice_session_bt_sco(struct audio_device *adev) {
    ALOGV("%s: Closing SCO PCMs", __func__);

    if (adev->pcm_sco_rx != NULL) {
        pcm_stop(adev->pcm_sco_rx);
        pcm_close(adev->pcm_sco_rx);
        adev->pcm_sco_rx = NULL;
    }

    if (adev->pcm_sco_tx != NULL) {
        pcm_stop(adev->pcm_sco_tx);
        pcm_close(adev->pcm_sco_tx);
        adev->pcm_sco_tx = NULL;
    }

    /* audio codecs like wm5201 need open modem pcm while using bt sco */
    if (adev->mode != AUDIO_MODE_IN_CALL)
        stop_voice_session(adev->voice.session);
}

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
int start_voice_session(struct voice_session *session)
{
    struct pcm_config *voice_config;

    if (session->pcm_voice_rx != NULL || session->pcm_voice_tx != NULL) {
        ALOGW("%s: Voice PCMs already open!\n", __func__);
        return 0;
    }

    ALOGV("%s: Opening voice PCMs", __func__);

    voice_config = &pcm_config_voicecall;

    /* Open modem PCM channels */
    session->pcm_voice_rx = pcm_open(SOUND_CARD,
                                     SOUND_PLAYBACK_VOICE_DEVICE,
                                     PCM_OUT|PCM_MONOTONIC,
                                     voice_config);
    if (session->pcm_voice_rx != NULL && !pcm_is_ready(session->pcm_voice_rx)) {
        ALOGE("%s: cannot open PCM voice RX stream: %s",
              __func__,
              pcm_get_error(session->pcm_voice_rx));

        pcm_close(session->pcm_voice_tx);
        session->pcm_voice_tx = NULL;

        return -ENOMEM;
    }

    session->pcm_voice_tx = pcm_open(SOUND_CARD,
                                     SOUND_CAPTURE_VOICE_DEVICE,
                                     PCM_IN|PCM_MONOTONIC,
                                     voice_config);
    if (session->pcm_voice_tx != NULL && !pcm_is_ready(session->pcm_voice_tx)) {
        ALOGE("%s: cannot open PCM voice TX stream: %s",
              __func__,
              pcm_get_error(session->pcm_voice_tx));

        pcm_close(session->pcm_voice_rx);
        session->pcm_voice_rx = NULL;

        return -ENOMEM;
    }

    pcm_start(session->pcm_voice_rx);
    pcm_start(session->pcm_voice_tx);

    return 0;
}

/*
 * This function must be called with hw device mutex locked, OK to hold other
 * mutexes
 */
void stop_voice_session(struct voice_session *session)
{
    int status = 0;

    ALOGV("%s: Closing active PCMs", __func__);

    if (session->pcm_voice_rx != NULL) {
        pcm_stop(session->pcm_voice_rx);
        pcm_close(session->pcm_voice_rx);
        session->pcm_voice_rx = NULL;
        status++;
    }

    if (session->pcm_voice_tx != NULL) {
        pcm_stop(session->pcm_voice_tx);
        pcm_close(session->pcm_voice_tx);
        session->pcm_voice_tx = NULL;
        status++;
    }

    session->out_device = AUDIO_DEVICE_NONE;

    ALOGV("%s: Successfully closed %d active PCMs", __func__, status);
}

void set_voice_session_volume(struct voice_session *session __unused, float volume __unused)
{

}

void set_voice_session_mic_mute(struct voice_session *session __unused, bool state __unused)
{
    //enum _MuteCondition mute_condition = state ? TX_MUTE : TX_UNMUTE;

    //ril_set_mute(&session->ril, mute_condition);
}

bool voice_session_uses_wideband(struct voice_session *session __unused)
{
    /*if (session->out_device & AUDIO_DEVICE_OUT_ALL_SCO) {
        return session->vdata->bluetooth_wb;
    }

    return session->wb_amr_type >= 1;*/

    return false;
}

struct voice_session *voice_session_init(struct audio_device *adev)
{
    struct voice_session *session;

    session = calloc(1, sizeof(struct voice_session));
    if (session == NULL) {
        return NULL;
    }

    session->vdata = &adev->voice;

    return session;
}

void voice_session_deinit(struct voice_session *session)
{
    free(session);
}
