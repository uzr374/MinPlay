#include "audioresampler.hpp"

AudioResampler::AudioResampler() {}
AudioResampler::~AudioResampler(){
    if(swr_ctx){
        swr_free(&swr_ctx);
    }
}

void AudioResampler::setOutputFmt(int rate, const CAVChannelLayout& lout, AVSampleFormat fmt){
    out_rate = rate;
    out_ch_layout = lout;
    out_fmt = fmt;
}

bool AudioResampler::convert(AVFrameView aframe, std::vector<uint8_t>& dst, int wanted_nb_samples, bool final){
    if (aframe.sampleFmt() != src_fmt || src_ch_layout != aframe.chLayout() ||
        aframe.sampleRate() != src_rate || (wanted_nb_samples != aframe.nbSamples() && !swr_ctx)) {
        swr_free(&swr_ctx);
        const int ret = swr_alloc_set_opts2(&swr_ctx, &out_ch_layout.constAv(), out_fmt, out_rate,
                                  &aframe.chLayout(), aframe.sampleFmt(), aframe.sampleRate(), 0, NULL);
        if (ret < 0 || swr_init(swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   aframe.sampleRate(), av_get_sample_fmt_name(aframe.sampleFmt()), aframe.chCount(),
                   out_rate, av_get_sample_fmt_name(out_fmt), out_ch_layout.nbChannels());
            swr_free(&swr_ctx);
            return false;
        }
        src_ch_layout = aframe.chLayout();
        src_rate = aframe.sampleRate();
        src_fmt = aframe.sampleFmt();
    }

    if (swr_ctx) {
        const int out_count = (int64_t)wanted_nb_samples * out_rate / aframe.sampleRate() + 256;
        const int out_size  = av_samples_get_buffer_size(NULL, out_ch_layout.nbChannels(), out_count, out_fmt, 0);
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return false;
        }
        if (wanted_nb_samples != aframe.nbSamples()) {
            if (swr_set_compensation(swr_ctx, (wanted_nb_samples - aframe.nbSamples()) * out_rate / aframe.sampleRate(),
                                     wanted_nb_samples * out_rate / aframe.sampleRate()) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return false;
            }
        }
        dst.resize(out_size);
        uint8_t* out[] = {dst.data()};
        const auto len2 = swr_convert(swr_ctx, out, out_count, aframe.extData(), aframe.nbSamples());
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return false;
        } else if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(swr_ctx) < 0)
                swr_free(&swr_ctx);
        }

        const auto resampled_data_size = len2 * out_ch_layout.nbChannels() * av_get_bytes_per_sample(out_fmt);
        dst.resize(resampled_data_size);
    } else {
        const auto data_size = av_samples_get_buffer_size(NULL, aframe.chCount(),
                                aframe.nbSamples(), aframe.sampleFmt(), 1);
        dst.resize(data_size);
        std::copy(aframe.constDataPlane(0), aframe.constDataPlane(0) + data_size, dst.begin());
    }
    return true;
}
