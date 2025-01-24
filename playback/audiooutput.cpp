#include "audiooutput.hpp"

#include <SDL3/SDL.h>

AudioOutput::AudioOutput() {
    if(!SDL_WasInit(SDL_INIT_AUDIO)){
        throw std::runtime_error("SDL audio subsistem was not initialized(somehow!)");
    }
}
AudioOutput::~AudioOutput(){
    if(astream){
        SDL_DestroyAudioStream(astream);
    }
}

void AudioOutput::requestChange(int rate, int chn){
    std::scoped_lock lck(ao_mtx);
    req_rate = rate;
    req_channels = chn;
    change_req = true;
}

void AudioOutput::setVolume(float vol){
    volume = vol;
    muted = volume == 0.0;

    if(astream){
        SDL_SetAudioStreamGain(astream, volume);
    }
}

void AudioOutput::setPauseStatus(bool pause_status){
    if(astream && pause_status != paused){
        paused = pause_status;
        if(paused)
            SDL_ResumeAudioStreamDevice(astream);
        else
            SDL_PauseAudioStreamDevice(astream);
    }
}

void AudioOutput::flushBuffers(){
    if(astream){
        SDL_ClearAudioStream(astream);
    }
}

bool AudioOutput::sendData(const uint8_t* src, size_t byte_len, bool final){
    if(!astream) return false;
    bool success = SDL_PutAudioStreamData(astream, src, byte_len);
    if(final)
        success |= SDL_FlushAudioStream(astream);
    return success;
}

double AudioOutput::getLatency() const{
    double latency = 0.0;
    if(astream){
        const auto bytes_queued = SDL_GetAudioStreamQueued(astream);
        latency = double(bytes_queued) / bitrate();
    }

    return latency;
}

static SDL_AudioStream* audio_open(int wanted_nb_channels, int wanted_sample_rate)
{
    const SDL_AudioSpec spec{.format = SDL_AUDIO_F32, .channels = wanted_nb_channels, .freq = wanted_sample_rate};
    const auto astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);

    if (astream) {
        SDL_ResumeAudioStreamDevice(astream);
    }

    return astream;
}

bool AudioOutput::maybeHandleChange(){
    std::scoped_lock lck(ao_mtx);

    const auto rate = req_rate, chn = req_channels;
    const bool change = change_req;
    change_req = false;

    if(change){
        if(astream){
            SDL_DestroyAudioStream(astream);
            astream = nullptr;
            ao_rate = ao_channels = 0;
            paused = false;
            if(rate <= 0 && chn <= 0){
                return true;
            }
        }

        if(rate > 0 && chn > 0){
            if((astream = audio_open(chn, rate))){
                ao_rate = rate;
                ao_channels = chn;
                return true;
            }
        }
    }

    return false;
}

bool AudioOutput::isOpen() const{
    return astream;
}

int AudioOutput::bitrate() const{
    return rate() * channels() * sizeof(float);
}

int AudioOutput::rate() const{
    return ao_rate;
}

int AudioOutput::channels() const{
    return ao_channels;
}
