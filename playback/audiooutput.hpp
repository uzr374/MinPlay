#ifndef AUDIOOUTPUT_HPP
#define AUDIOOUTPUT_HPP

#include <mutex>

#include <QtGlobal>

/* A simple audio output based on SDL3. It can accept any sample rate and channel count since
 * SDL_AudioStream can manage these conversions internally, but the samples ought to always be in
 * the Float32 interleaved format. */
class AudioOutput final
{
    Q_DISABLE_COPY_MOVE(AudioOutput);
private:
    struct SDL_AudioStream* astream = nullptr;
    std::mutex ao_mtx;
    bool change_req = false;
    int ao_rate = 0, ao_channels = 0, req_rate = 0, req_channels = 0;
    float volume = 1.0;
    bool muted = false, paused = false;

public:
    AudioOutput();
    ~AudioOutput();

    /*Requests a change for audio output. If either rate or
     * channels is equal to 0, then the output is closed*/
    void requestChange(int new_rate, int new_channels);
    int rate() const;
    int channels() const;
    void setVolume(float vol);
    void setPauseStatus(bool paused);
    void flushBuffers();
    bool sendData(const uint8_t* src, size_t byte_len, bool final);
    bool forceReopen();
    /*Returns true if there was a change*/
    bool maybeHandleChange();
    bool isOpen() const;
    int bitrate() const;

    /*returns the buffered duration in seconds*/
    double getLatency() const;

};

#endif // AUDIOOUTPUT_HPP
