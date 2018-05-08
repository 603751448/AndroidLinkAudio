//
// Created by julien@macmini on 07/05/2018.
//

#include "AudioEngine.h"
#include "logging_macros.h"
#include <inttypes.h>
#include <cstring>

constexpr int64_t kNanosPerMillisecond = 1000000; // Use int64_t to avoid overflows in calculations
constexpr int32_t kDefaultChannelCount = 2; // Stereo

// ------------------------------------------------------------------------------------------------
//                                           RENDERING
// ------------------------------------------------------------------------------------------------

DataCallbackResult
AudioEngine::onAudioReady(AudioStream *audioStream, void *audioData, int32_t numFrames) {

    int32_t bufferSize = audioStream->getBufferSizeInFrames();

    if (mBufferSizeSelection == kBufferSizeAutomatic) {
        mLatencyTuner->tune();
    } else if (bufferSize != (mBufferSizeSelection * mFramesPerBurst)) {
        audioStream->setBufferSizeInFrames(mBufferSizeSelection * mFramesPerBurst);
        bufferSize = audioStream->getBufferSizeInFrames();
    }

    int32_t channelCount = audioStream->getChannelCount();

    // grab link session state
    auto sessionState = link.captureAudioSessionState();
    const auto hostTime = mHostTimeFilter.sampleTimeToHostTime(mSampleTime);
    mSampleTime += numFrames;

    std::chrono::milliseconds latencyMs(static_cast<std::int64_t>(mCurrentOutputLatencyMillis));
    std::chrono::microseconds latencyMuSec(latencyMs);
    const std::chrono::microseconds bufferBeginAtOutput = hostTime + latencyMuSec;
    const auto microsPerSample = 1e6 / mSampleRate;


    if (audioStream->getFormat() == oboe::AudioFormat::Float) {

        // Logic: successive renders are ADDED to the initial zero buffer.

        // zero-fill buffer
        memset(static_cast<float *>(audioData), 0,
               sizeof(float) * channelCount * numFrames);

        //
        for (int i = 0; i < channelCount; ++i) {

            renderBarClick(static_cast<float *>(audioData) + i, channelCount,
                           numFrames, sessionState, bufferBeginAtOutput, microsPerSample);

            mOscillators[i].render(static_cast<float *>(audioData) + i, channelCount, numFrames);
        }

    } else
        // ---------------------------------------------------------------------------------
        //                  RENDER TO INT16 INSTEAD OF FLOAT
        // ---------------------------------------------------------------------------------
    {
        // zero-fill buffer
        memset(static_cast<uint16_t *>(audioData), 0,
               sizeof(int16_t) * channelCount * numFrames);

    }

    return DataCallbackResult::Continue;
}


void AudioEngine::prepareOscillators() {

    double frequency = 440.0;
    constexpr double interval = 110.0;
    constexpr float amplitude = 0.4;

    for (SineGenerator &osc : mOscillators){
        osc.setup(frequency, mSampleRate, amplitude);
        frequency += interval;
    }
}

void AudioEngine::renderBarClick(float *buffer,
                                     int32_t channelStride,
                                     int32_t numFrames,
                                     ableton::Link::SessionState sessionState,
                                     std::chrono::microseconds bufferBeginAtOutput,
                                     double microsPerSample) {
    for (int i = 0, sampleIndex = 0; i < numFrames; i++) {

        const auto sampleHostTime = bufferBeginAtOutput + std::chrono::microseconds(llround(i * microsPerSample));
        float sample = 0;
        const double barPhase = sessionState.phaseAtTime(sampleHostTime, mQuantum);
        if ((barPhase - mLastBarPhase) < -(mQuantum/2)) {
            // uncomment if you want to render a click on each bar.
            sample = 1.0;
            LOGD("BAR PHASE AT TICK: %f, lastBarPhase %f", barPhase, mLastBarPhase);
            timeAtLastBar = sampleHostTime;
        }
        buffer[sampleIndex] +=  sample;
        sampleIndex += channelStride;
        mLastBarPhase = barPhase;
    }
}

// ------------------------------------------------------------------------------------------------
//                                           LIFECYCLE
// ------------------------------------------------------------------------------------------------
AudioEngine::AudioEngine(): link(240.) {
    mPlayStatus = stopped;
    mLastBeatPhase = 0.;
    mLastBarPhase = 0.;
    mSampleTime = 0.0;
    mChannelCount = kDefaultChannelCount;

}

AudioEngine::~AudioEngine() {
    closeOutputStream();
}

void AudioEngine::createPlaybackStream() {

    oboe::AudioStreamBuilder builder;
    setupPlaybackStreamParameters(&builder);

    oboe::Result result = builder.openStream(&mPlayStream);

    if (result == oboe::Result::OK && mPlayStream != nullptr) {

        mSampleRate = mPlayStream->getSampleRate();
        mFramesPerBurst = mPlayStream->getFramesPerBurst();

        int channelCount = mPlayStream->getChannelCount();
        if (channelCount != mChannelCount){
            LOGW("Requested %d channels but received %d", mChannelCount, channelCount);
        }

        // Set the buffer size to the burst size - this will give us the minimum possible latency
        mPlayStream->setBufferSizeInFrames(mFramesPerBurst);

        prepareOscillators();

        // Create a latency tuner which will automatically tune our buffer size.
        mLatencyTuner = std::unique_ptr<oboe::LatencyTuner>(new oboe::LatencyTuner(*mPlayStream));
        // Start the stream - the dataCallback function will start being called
        result = mPlayStream->requestStart();
        if (result != oboe::Result::OK) {
            LOGE("Error starting stream. %s", oboe::convertToText(result));
        }

        mIsLatencyDetectionSupported = (mPlayStream->getTimestamp(CLOCK_MONOTONIC, 0, 0) !=
                                        oboe::Result::ErrorUnimplemented);

    } else {
        LOGE("Failed to create stream. Error: %s", oboe::convertToText(result));
    }
}

void AudioEngine::closeOutputStream() {

    if (mPlayStream != nullptr) {
        oboe::Result result = mPlayStream->requestStop();
        if (result != oboe::Result::OK) {
            LOGE("Error stopping output stream. %s", oboe::convertToText(result));
        }

        result = mPlayStream->close();
        if (result != oboe::Result::OK) {
            LOGE("Error closing output stream. %s", oboe::convertToText(result));
        }
    }
}

void AudioEngine::setupPlaybackStreamParameters(oboe::AudioStreamBuilder *builder) {
    builder->setAudioApi(mAudioApi);
    builder->setDeviceId(mPlaybackDeviceId);
    builder->setChannelCount(mChannelCount);

    // We request EXCLUSIVE mode since this will give us the lowest possible latency.
    // If EXCLUSIVE mode isn't available the builder will fall back to SHARED mode.
    builder->setSharingMode(oboe::SharingMode::Exclusive);
    builder->setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder->setCallback(this);
}



/**
 * If there is an error with a stream this function will be called. A common example of an error
 * is when an audio device (such as headphones) is disconnected. It is safe to restart the stream
 * in this method. There is no need to create a new thread.
 *
 * @param audioStream the stream with the error
 * @param error the error which occured, a human readable string can be obtained using
 * oboe::convertToText(error);
 *
 * @see oboe::StreamCallback
 */
void AudioEngine::onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) {
    if (error == oboe::Result::ErrorDisconnected) restartStream();
}

void AudioEngine::restartStream() {

    LOGI("Restarting stream");

    if (mRestartingLock.try_lock()) {
        closeOutputStream();
        createPlaybackStream();
        mRestartingLock.unlock();
    } else {
        LOGW("Restart stream operation already in progress - ignoring this request");
        // We were unable to obtain the restarting lock which means the restart operation is currently
        // active. This is probably because we received successive "stream disconnected" events.
        // Internal issue b/63087953
    }
}


// ------------------------------------------------------------------------------------------------
//                                           API
// ------------------------------------------------------------------------------------------------

void AudioEngine::enableLink(bool enableFlag){
    link.enable(enableFlag);
}

void AudioEngine::createStream(){
    createPlaybackStream();
}