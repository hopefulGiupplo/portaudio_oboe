/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Android Oboe implementation of PortAudio based on Sanne Raymaekers' work with OpenSLES.
 *
 ****************************************************************************************
 *      Copyright (c) 2022-2023 by NetResults S.r.l. ( https://www.netresults.it )      *
 *      Author:                                                                         *
 *              Carlo Benfatti          <benfatti@netresults.it>                        *
 ****************************************************************************************
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"

/** @file
 @ingroup hostapi_src
 @brief Oboe implementation of support for a host API.
*/
#include "pa_allocation.h"
#include "pa_cpuload.h"
#include "pa_debugprint.h"
#include "pa_hostapi.h"
#include "pa_process.h"
#include "pa_stream.h"
#include "pa_unix_util.h"
#include "pa_util.h"

#include <pthread.h>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <cstring>
#include <memory>
#include <cstdint>
#include <vector>
#include "oboe/Oboe.h"

#include <android/log.h>
#include <android/api-level.h>

#include "pa_oboe.h"

#define MODULE_NAME "PaOboe"

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, MODULE_NAME, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, MODULE_NAME, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,MODULE_NAME, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,MODULE_NAME, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL,MODULE_NAME, __VA_ARGS__)

// Copied from @{pa_opensles.c}.
#define ENSURE(expr, errorText)                                             \
    do                                                                      \
    {                                                                       \
        PaError m_err;                                                      \
        if (UNLIKELY((m_err = (expr)) < paNoError))                         \
        {                                                                   \
            PaUtil_DebugPrint(("Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( \
                                                                __LINE__ ) "\n")); \
            PaUtil_SetLastHostErrorInfo(paInDevelopment, m_err, errorText); \
            m_error = m_err;                                               \
            goto error;                                                     \
        }                                                                   \
    } while (0);

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaOboe_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex);

#ifdef __cplusplus
}
#endif /* __cplusplus */

static void Terminate(struct PaUtilHostApiRepresentation *hostApi);

static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate);

static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
                          PaStream **s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData);

static PaError CloseStream(PaStream *stream);

static PaError StartStream(PaStream *stream);

static PaError StopStream(PaStream *stream);

static PaError AbortStream(PaStream *stream);

static PaError IsStreamStopped(PaStream *s);

static PaError IsStreamActive(PaStream *stream);

static PaTime GetStreamTime(PaStream *stream);

static double GetStreamCpuLoad(PaStream *stream);

static PaError ReadStream(PaStream *stream, void *buffer, unsigned long frames);

static PaError WriteStream(PaStream *stream, const void *buffer, unsigned long frames);

static void StreamProcessingCallback(void *userData);

static signed long GetStreamReadAvailable(PaStream *stream);

static signed long GetStreamWriteAvailable(PaStream *stream);

static unsigned long GetApproximateLowBufferSize();

int32_t getSelectedDevice(oboe::Direction direction);

// Commonly used parameters initialized.
static unsigned long nativeBufferSize = 0;
static unsigned numberOfBuffers = 2;

using namespace oboe;

int32_t inputDeviceId = kUnspecified;
int32_t outputDeviceId = kUnspecified;

/**
 * Stream structure, useful to store relevant information. It's needed by Portaudio.
 */
typedef struct OboeStream{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    bool isBlocking;
    bool isStopped;
    bool isActive;
    bool doStop;
    bool doAbort;
    bool hasOutput;
    bool hasInput;

    int callbackResult;
    DataCallbackResult oboeCallbackResult;

    PaStreamCallbackFlags cbFlags;
    //PaUnixThread streamThread;

    // Buffers are managed by the callback function in Oboe.
    void **outputBuffers;
    int currentOutputBuffer;
    void **inputBuffers;
    int currentInputBuffer;

    long engineAddress;
    unsigned long framesPerHostCallback;
    unsigned bytesPerFrame;
} OboeStream;


/**
 * Stream engine, representing the host API - Oboe.
 */
class OboeEngine : public AudioStreamCallback {
public:
    OboeEngine();

    DataCallbackResult onAudioReady(AudioStream *audioStream, void *audioData,
                                    int32_t numFrames) override;
    void onErrorAfterClose(AudioStream *audioStream, oboe::Result error) override;

    PaError openStream(Direction direction, int32_t sampleRate, Usage outputUsage, InputPreset inputPreset);
    bool tryStream(Direction direction, int32_t sampleRate, int32_t channelCount);
    bool startStream();
    bool writeStream(const void* buffer, int32_t framesToWrite);
    bool readStream(void* buffer, int32_t framesToRead);
    bool stopStream();
    bool abortStream();
    bool closeStream();
    bool restartStream();

    //Get access to private data:
    OboeStream* initializeOboeStream(){
        oboeStream = (OboeStream *) PaUtil_AllocateMemory(sizeof (OboeStream));
        return oboeStream;
    }

    void setEngineAddress(long address){ oboeStream->engineAddress = address; }

    void resetCallbackCounters(){
        framesProcessed =0;
        timeInfo = {0, 0, 0};
    }

private:
    OboeStream* oboeStream;

    //Output and input streams
    std::shared_ptr<AudioStream> outputStream; // To be built as output. Was: audioPlayer.
    AudioStreamBuilder outputBuilder;          // Was: outputMixObject.

    std::shared_ptr<AudioStream> inputStream; // To be built as input. Was: audioRecorder.
    AudioStreamBuilder inputBuilder;

    //callback utils
    unsigned long framesProcessed{};
    PaStreamCallbackTimeInfo timeInfo{};
    struct timespec timeSpec{};
};


/**
 * Structure used by Portaudio to interface with the HostApi - in this case, Oboe.
 */
typedef struct PaOboeHostApiRepresentation {
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    OboeEngine *oboeEngine;
} PaOboeHostApiRepresentation;


//OboeEngine functions definitions.
/**
    \brief  Initializes an instance of the engine.
 */
OboeEngine::OboeEngine() {
    oboeStream = nullptr;
}


/**
 * \brief Oboe's callback routine.
 */
DataCallbackResult
OboeEngine::onAudioReady(AudioStream *audioStream, void *audioData, int32_t numFrames) {

    clock_gettime(CLOCK_REALTIME, &timeSpec);
    timeInfo.currentTime = (PaTime) (timeSpec.tv_sec + (timeSpec.tv_nsec / 1000000000.0));
    timeInfo.outputBufferDacTime = (PaTime) (oboeStream->framesPerHostCallback
                                               /
                                               oboeStream->streamRepresentation.streamInfo.sampleRate
                                               + timeInfo.currentTime);
    timeInfo.inputBufferAdcTime = (PaTime) (oboeStream->framesPerHostCallback
                                              /
                                              oboeStream->streamRepresentation.streamInfo.sampleRate
                                              + timeInfo.currentTime);

    /* check if StopStream or AbortStream was called */
    if (oboeStream->doStop) {
        oboeStream->callbackResult = paComplete;
        LOGV("Callback: doStop -> paComplete");
    }
    else if (oboeStream->doAbort) {
        oboeStream->callbackResult = paAbort;
        LOGV("Callback: doAbort -> paAbort");
    }

    PaUtil_BeginCpuLoadMeasurement(&oboeStream->cpuLoadMeasurer);
    PaUtil_BeginBufferProcessing(&oboeStream->bufferProcessor,
                                 &timeInfo, oboeStream->cbFlags);

    if (oboeStream->hasOutput) {
        LOGV("Callback: hasOutput");
        oboeStream->outputBuffers[oboeStream->currentOutputBuffer] = audioData;
        PaUtil_SetOutputFrameCount(&oboeStream->bufferProcessor, numFrames);
        PaUtil_SetInterleavedOutputChannels(&oboeStream->bufferProcessor, 0,
                                            (void *) ((float **) oboeStream->outputBuffers)[oboeStream->currentOutputBuffer],
                                            0);
    }
    if (oboeStream->hasInput) {
        LOGV("Callback: hasInput");
        audioData = oboeStream->inputBuffers[oboeStream->currentInputBuffer];
        PaUtil_SetInputFrameCount(&oboeStream->bufferProcessor, 0);
        PaUtil_SetInterleavedInputChannels(&oboeStream->bufferProcessor, 0,
                                           (void *) ((float **) oboeStream->inputBuffers)[oboeStream->currentInputBuffer],
                                           0);
    }

    /* continue processing user buffers if cbresult is pacontinue or if cbresult is  pacomplete and userbuffers aren't empty yet  */
    if (oboeStream->callbackResult == paContinue
        || (oboeStream->callbackResult == paComplete
            && !PaUtil_IsBufferProcessorOutputEmpty(&oboeStream->bufferProcessor))) {
        LOGV("Callback: paContinue or (paComplete and buffers not empty)");
        framesProcessed = PaUtil_EndBufferProcessing(&oboeStream->bufferProcessor,
                                                       &oboeStream->callbackResult);
    }

    /* enqueue a buffer only when there are frames to be processed,
     * this will be 0 when paComplete + empty buffers or paAbort
     */
    if (framesProcessed > 0) {
        LOGV("Callback: frameProcessed > 0");
        if (oboeStream->hasOutput) {
            oboeStream->currentOutputBuffer =
                    (oboeStream->currentOutputBuffer + 1) % numberOfBuffers;
            LOGV("Callback: hasOutput -> scroll buffers");
        }
        if (oboeStream->hasInput) {
            oboeStream->currentInputBuffer = (oboeStream->currentInputBuffer + 1) % numberOfBuffers;
            LOGV("Callback: hasInput -> scroll buffers");
        }
    }

    PaUtil_EndCpuLoadMeasurement(&oboeStream->cpuLoadMeasurer, framesProcessed);

    /* StopStream was called */
    if (framesProcessed == 0 && oboeStream->doStop) {
        LOGV("Callback: StopStream was called");
        oboeStream->oboeCallbackResult = DataCallbackResult::Stop;
    }

        /* if AbortStream or StopStream weren't called, stop from the cb */
    else if (framesProcessed == 0 && !(oboeStream->doAbort || oboeStream->doStop)) {
        LOGV("Callback: AbortStream or StopStream weren't called, but it's time to stop");
        oboeStream->isActive = false;
        oboeStream->isStopped = true;
        if (oboeStream->streamRepresentation.streamFinishedCallback != nullptr)
            oboeStream->streamRepresentation.streamFinishedCallback(
                    oboeStream->streamRepresentation.userData);
        oboeStream->oboeCallbackResult = DataCallbackResult::Stop;
    }

    return oboeStream->oboeCallbackResult;
}


/**
 * \brief If the data callback ends without returning DataCallbackResult::Stop, this routine tells
 * what error occurred.
 */
void OboeEngine::onErrorAfterClose(AudioStream *audioStream, oboe::Result error) {
    LOGE("onErrorAfterClose - Error was %s", oboe::convertToText(error));
}


/**
 * \brief Tries to open a stream with the direction @direction, sample rate @sampleRate and/or
 * channel count @channelCount. It then checks if the stream was in fact opened with the desired
 * settings, and then closes the stream.
 * @param direction the Direction of the stream;
 * @param sampleRate the sample rate we want to try;
 * @param channelCount the channel count we want to try;
 * @return true if the sample rate / channel count are supported by the device, false if they aren't
 *          or if tryStream couldn't open a stream.
 */
bool OboeEngine::tryStream(Direction direction, int32_t sampleRate, int32_t channelCount) {
    AudioStreamBuilder m_builder;
    Result m_result;
    bool m_outcome = false;

    m_builder.setSharingMode(SharingMode::Exclusive)
            ->setFormat(AudioFormat::Float)
            ->setDeviceId(getSelectedDevice(direction))
            ->setDirection(direction)
            ->setSampleRate(sampleRate)
            ->setChannelCount(channelCount);

    if(direction == Direction::Input) {
        LOGI("TRYSTREAM - Direction: Input");
        m_result = m_builder.openStream(inputStream);
    }
    else {
        LOGI("TRYSTREAM - Direction: Output");
        m_result = m_builder.openStream(outputStream);
    }

    if(m_result != Result::OK) {
        LOGE("Couldn't open the stream in TryStream. Error: %s", convertToText(m_result));
        return m_outcome;
    }

    if(sampleRate != kUnspecified) {
        m_outcome = (sampleRate == m_builder.getSampleRate());
        LOGV("Requested sampleRate = %d, builder sampleRate = %d",
             sampleRate, m_builder.getSampleRate());
    }
    else if(channelCount != kUnspecified) {
        m_outcome = (channelCount == m_builder.getChannelCount());
        LOGI("Requested channelCount = %d, builder channelCount = %d",
             channelCount, m_builder.getChannelCount());
    }
    else {
        LOGE("Logic failed in TryStream.");
        m_outcome = false;
    }

    if(direction == Direction::Input)
        inputStream->close();
    else
        outputStream->close();

    return m_outcome;
}


/**
 * \brief Closes an oboeStream - both input and output streams are checked and closed if active.
 * @return true if the stream is closed successfully, otherwise returns false.
 */
bool OboeEngine::closeStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream->hasOutput) {
        m_outputResult = outputStream->close();
        if(m_outputResult == Result::ErrorClosed) {
            m_outputResult = Result::OK;
            LOGW("Tried to close output stream, but was already closed.");
        }
    }
    if(oboeStream->hasInput) {
        m_inputResult = inputStream->close();
        if(m_inputResult == Result::ErrorClosed) {
            m_inputResult = Result::OK;
            LOGW("Tried to close input stream, but was already closed.");
        }
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief Opens a stream with direction @direction, sample rate @sampleRate and, depending on the
 *        direction of the stream, sets its usage (if direction is output) or its preset (if
 *        direction is input). Moreover, this function checks if the stream will use a callback, and
 *        sets it in that case.
 * @param direction the Direction of the stream we want to open;
 * @param sampleRate the sample rate of the stream we want to open;
 * @param androidOutputUsage the Usage of the output stream we want to open
 *                              (only matters with Android Api level >= 28);
 * @param androidInputPreset the Preset of the input stream we want to open
 *                              (only matters with Android Api level >= 28);
 * @return paNoError if everything goes as expected, paUnanticipatedHostError if Oboe fails to open
 *          a stream, and paInsufficientMemory if the memory allocation of the buffers fails.
 */
PaError OboeEngine::openStream(Direction direction,
                               int32_t sampleRate,
                               Usage androidOutputUsage,
                               InputPreset androidInputPreset){
    PaError m_error = paNoError;

    if(sampleRate < 16000)
        sampleRate = 44100;

    if(direction == Direction::Input) {
        inputBuilder.setChannelCount(oboeStream->bufferProcessor.inputChannelCount)
                ->setFormat(AudioFormat::Float)
                ->setSharingMode(SharingMode::Exclusive)
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Input)
                ->setDeviceId(getSelectedDevice(Direction::Input))
                ->setInputPreset(androidInputPreset)
                ->setFramesPerCallback(oboeStream->framesPerHostCallback);

        if(!(oboeStream->isBlocking)) {
            resetCallbackCounters();
            inputBuilder.setDataCallback(this)
                ->setPerformanceMode(PerformanceMode::LowLatency);
        }

        if (inputBuilder.openStream(inputStream) != Result::OK) {
            LOGE("Oboe couldn't open the input stream.");
            m_error = paUnanticipatedHostError;
            return m_error;
        }

        inputStream->setBufferSizeInFrames(inputStream->getFramesPerBurst() * numberOfBuffers);
        oboeStream->inputBuffers =
                (void **) PaUtil_AllocateMemory(numberOfBuffers * sizeof(int32_t *));

        for (int i = 0; i < numberOfBuffers; ++i) {
            oboeStream->inputBuffers[i] = (void *) PaUtil_AllocateMemory(
                    oboeStream->framesPerHostCallback *
                            oboeStream->bytesPerFrame *
                            oboeStream->bufferProcessor.inputChannelCount);

            if (!oboeStream->inputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(oboeStream->inputBuffers[j]);
                PaUtil_FreeMemory(oboeStream->inputBuffers);
                inputStream->close();
                m_error = paInsufficientMemory;
                break;
            }
        }
        oboeStream->currentInputBuffer = 0;
    } else {
        outputBuilder.setChannelCount(oboeStream->bufferProcessor.outputChannelCount)
                ->setFormat(AudioFormat::Float)
                ->setSharingMode(SharingMode::Exclusive)
                ->setSampleRate(sampleRate)
                ->setDirection(Direction::Output)
                ->setDeviceId(getSelectedDevice(Direction::Output))
                ->setUsage(androidOutputUsage)
                ->setFramesPerCallback(oboeStream->framesPerHostCallback);

        if(!(oboeStream->isBlocking)) {
            resetCallbackCounters();
            outputBuilder.setDataCallback(this)
                ->setPerformanceMode(PerformanceMode::LowLatency);
        }

        if (outputBuilder.openStream(outputStream) != Result::OK) {
            LOGE("Oboe couldn't open the output stream.");
            m_error = paUnanticipatedHostError;
            return m_error;
        }

        outputStream->setBufferSizeInFrames(outputStream->getFramesPerBurst() * numberOfBuffers);
        oboeStream->outputBuffers =
                (void **) PaUtil_AllocateMemory(numberOfBuffers * sizeof(int32_t *));

        for (int i = 0; i < numberOfBuffers; ++i) {
            oboeStream->outputBuffers[i] = (void *) PaUtil_AllocateMemory(
                    oboeStream->framesPerHostCallback *
                            oboeStream->bytesPerFrame *
                            oboeStream->bufferProcessor.outputChannelCount);

            if (!oboeStream->outputBuffers[i]) {
                for (int j = 0; j < i; ++j)
                    PaUtil_FreeMemory(oboeStream->outputBuffers[j]);
                PaUtil_FreeMemory(oboeStream->outputBuffers);
                outputStream->close();
                m_error = paInsufficientMemory;
                break;
            }
        }
        oboeStream->currentOutputBuffer = 0;
    }

    return m_error;
}


/**
 * \brief Starts an oboeStream.
 * @return true if the output stream and the input stream are started successfully, false otherwise.
 */
bool OboeEngine::startStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream->hasInput) {
        m_inputResult = inputStream->requestStart();
        if (m_inputResult != Result::OK)
            LOGE("Couldn't start the input stream.");
    }
    if(oboeStream->hasOutput) {
        m_outputResult = outputStream->requestStart();
        if (m_outputResult != Result::OK)
            LOGE("Couldn't start the output stream.");
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief Requests the stop of an oboeStream.
 * @return true if the output stream and the input stream are stopped successfully, false otherwise.
 */
bool OboeEngine::stopStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream->hasInput) {
        m_inputResult = inputStream->requestStop();
        if (m_inputResult != Result::OK)
            LOGE("Couldn't stop the input stream.");
    }
    if(oboeStream->hasOutput) {
        m_outputResult = outputStream->requestStop();
        if (m_outputResult != Result::OK)
            LOGE("Couldn't stop the output stream.");
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief Forcefully stops an oboeStream.
 * @return true if the output stream and the input stream are stopped successfully, false otherwise.
 */
bool OboeEngine::abortStream() {
    Result m_outputResult = Result::OK, m_inputResult = Result::OK;

    if(oboeStream->hasInput) {
        m_inputResult = inputStream->stop();
        if (m_inputResult != Result::OK)
            LOGE("Couldn't force the input stream to stop.");
    }
    if(oboeStream->hasOutput) {
        m_outputResult = outputStream->stop();
        if (m_outputResult != Result::OK)
            LOGE("Couldn't force the output stream to stop.");
    }

    return (m_outputResult == Result::OK && m_inputResult == Result::OK);
}


/**
 * \brief Writes buffers on an oboeStream. This calls a blocking method.
 * @return true if the output stream and the input stream are started successfully, false otherwise.
 */
bool OboeEngine::writeStream(const void* buffer, int32_t framesToWrite) {
    bool m_outcome = true;

    ResultWithValue<int32_t> m_result = outputStream->write(buffer, framesToWrite, TIMEOUT_NS);

    if(m_result.error() == Result::ErrorDisconnected){
        if(OboeEngine::restartStream())
            goto restarted;
    }

    if(!m_result){
        LOGE("Error writing stream: %s", convertToText(m_result.error()));
        m_outcome = false;
    }
    return m_outcome;

restarted:
    return m_outcome;
}


/**
 * \brief Reads buffers of an oboeStream. This calls a blocking method.
 * @return true if the output stream and the input stream are started successfully, false otherwise.
 */
bool OboeEngine::readStream(void* buffer, int32_t framesToRead) {
    bool m_outcome = true;

    ResultWithValue<int32_t> m_result = inputStream->read(buffer, framesToRead, TIMEOUT_NS);
    if(!m_result){
        LOGE("Error reading stream: %s", convertToText(m_result.error()));
        m_outcome = false;
    }
    return m_outcome;
}


bool OboeEngine::restartStream() {
    bool m_outcome = true;

    LOGI("Restarting Stream(s).");

    OboeEngine::stopStream();
    OboeEngine::closeStream();

    if(oboeStream->hasInput){
        if (inputBuilder.openStream(inputStream) != Result::OK) {
            LOGE("Oboe couldn't reopen the input stream.");
            goto error;
        }

    }

    if(oboeStream->hasOutput){
        if(outputBuilder.openStream(outputStream) != Result::OK) {
            LOGE("Oboe couldn't reopen the output stream.");
            goto error;
        }
    }

    if(!(OboeEngine::startStream())){
        LOGE("Oboe couldn't restart the stream(s).");
        goto error;
    }

    return m_outcome;

error:
    m_outcome = false;
    return m_outcome;
}


/**
    \brief  Check if the requested sample rate is supported by trying to open an output stream with said sample rate.
            This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  sampleRate is the sample rate we want to check.
*/
PaError IsOutputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                             sampleRate,
                                             kUnspecified)))
        LOGW("Output Sample Rate was changed by Oboe. It can be because the device doesn't support it.");

    return paNoError;
}


/**
    \brief  Check if the requested sample rate is supported by trying to open an input stream with said sample rate.
            This function is used by PaOboe_Initialize, IsFormatSupported, and OpenStream.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  sampleRate is the sample rate we want to check.
*/
PaError IsInputSampleRateSupported(PaOboeHostApiRepresentation *oboeHostApi, double sampleRate) {
    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                             sampleRate,
                                             kUnspecified)))
        LOGW("Input Sample Rate was changed by Oboe. It can be because the device doesn't support it.");

    return paNoError;
}


/**
    \brief  Checks if the selected channel count is supported by the output device by opening an oboeStream for a moment.
            Used by PaOboe_Initialize.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  numOfChannels is the value of the number of channels we want to check if it's supported.
*/
static PaError IsOutputChannelCountSupported(
            PaOboeHostApiRepresentation *oboeHostApi,
            int32_t numOfChannels) {
    if (numOfChannels > 2 || numOfChannels == 0) {
        LOGE("Requested channel count is an invalid number!");
        return paInvalidChannelCount;
    }

    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Output,
                                             kUnspecified,
                                             numOfChannels)))
        LOGW("Output Channel Count was changed by Oboe. It can be because the device doesn't support it.");

    return paNoError;
}


/**
    \brief  Checks if the selected channel count is supported by the input device by opening an oboeStream for a moment.
            Used by PaOboe_Initialize.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  numOfChannels is the value of the number of channels we want to check if it's supported.
*/
static PaError IsInputChannelCountSupported(
            PaOboeHostApiRepresentation *oboeHostApi,
            int32_t numOfChannels) {
    if (numOfChannels > 2 || numOfChannels == 0) {
        LOGE("Requested channel count is an invalid number!");
        return paInvalidChannelCount;
    }

    if (!(oboeHostApi->oboeEngine->tryStream(Direction::Input,
                                             kUnspecified,
                                             numOfChannels)))
        LOGW("Input Channel Count was changed by Oboe. It can be because the device doesn't support it.");

    return paNoError;
}


/**
    \brief  Initializes common parameters and allocates the memory necessary to start the audio streams.
    \param  hostApi points towards a *HostApiRepresentation, which is a structure representing the interface
            to a host API (see struct in "pa_hostapi.h").
    \param  hostApiIndex is a PaHostApiIndex, the type used to enumerate the host APIs at runtime.
*/
PaError PaOboe_Initialize(PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex) {
    PaError m_result = paNoError;
    int m_deviceCount;
    PaOboeHostApiRepresentation *m_oboeHostApi;
    PaDeviceInfo *m_deviceInfoArray;
    char *m_deviceName;

    LOGV("Initializing PaOboe...");

    m_oboeHostApi = (PaOboeHostApiRepresentation *)PaUtil_AllocateMemory(
        sizeof(PaOboeHostApiRepresentation));
    if (!m_oboeHostApi) {
        m_result = paInsufficientMemory;
        goto error;
    }

    m_oboeHostApi->oboeEngine = new OboeEngine();

    m_oboeHostApi->allocations = PaUtil_CreateAllocationGroup();
    if (!m_oboeHostApi->allocations) {
        m_result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &m_oboeHostApi->inheritedHostApiRep;
    // Initialization of infos.
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "android Oboe";
    (*hostApi)->info.defaultOutputDevice = 0;
    (*hostApi)->info.defaultInputDevice = 0;
    (*hostApi)->info.deviceCount = 0;


    m_deviceCount = 1;
    (*hostApi)->deviceInfos = (PaDeviceInfo **)PaUtil_GroupAllocateMemory(
        m_oboeHostApi->allocations, sizeof(PaDeviceInfo *) * m_deviceCount);

    if (!(*hostApi)->deviceInfos) {
        m_result = paInsufficientMemory;
        goto error;
    }

    /* allocate all device info structs in a contiguous block */
    m_deviceInfoArray = (PaDeviceInfo *)PaUtil_GroupAllocateMemory(
        m_oboeHostApi->allocations, sizeof(PaDeviceInfo) * m_deviceCount);
    if (!m_deviceInfoArray) {
        m_result = paInsufficientMemory;
        goto error;
    }

    for (int i = 0; i < m_deviceCount; ++i) {
        PaDeviceInfo *m_deviceInfo = &m_deviceInfoArray[i];
        m_deviceInfo->structVersion = 2;
        m_deviceInfo->hostApi = hostApiIndex;

        /* OboeEngine will handle device selection through the implementation of
         * PaOboe_SetSelectedDevice */

        m_deviceInfo->name = "default";

        /* Try channels in order of preference - Stereo > Mono. */
        const int32_t m_channelsToTry[] = {2, 1};
        const int32_t m_channelsToTryLength = 2;

        m_deviceInfo->maxOutputChannels = 0;
        m_deviceInfo->maxInputChannels = 0;

        for (i = 0; i < m_channelsToTryLength; ++i) {
            if (IsOutputChannelCountSupported(m_oboeHostApi, m_channelsToTry[i]) == paNoError) {
                m_deviceInfo->maxOutputChannels = m_channelsToTry[i];
                break;
            }
        }
        for (i = 0; i < m_channelsToTryLength; ++i) {
            if (IsInputChannelCountSupported(m_oboeHostApi, m_channelsToTry[i]) == paNoError) {
                m_deviceInfo->maxInputChannels = m_channelsToTry[i];
                break;
            }
        }

        /* check samplerates in order of preference */
        const int32_t m_sampleRates[] = {48000, 44100, 32000, 24000, 16000};
        const int32_t m_numberOfSampleRates = 5;

        m_deviceInfo->defaultSampleRate = m_sampleRates[0];

        for (i = 0; i < m_numberOfSampleRates; ++i) {
            if (IsOutputSampleRateSupported(
                    m_oboeHostApi, m_sampleRates[i]) == paNoError &&
                IsInputSampleRateSupported(
                    m_oboeHostApi, m_sampleRates[i]) == paNoError) {
                m_deviceInfo->defaultSampleRate = m_sampleRates[i];
                break;
            }
        }
        if (m_deviceInfo->defaultSampleRate == 0)
            goto error;

        /* If the user has set nativeBufferSize by querying the optimal buffer size via java,
         * use the user-defined value since that will offer the lowest possible latency
         */
        if (nativeBufferSize != 0) {
            m_deviceInfo->defaultLowInputLatency =
                (double)nativeBufferSize / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultLowOutputLatency =
                (double)nativeBufferSize / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighInputLatency =
                (double)nativeBufferSize * 4 / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighOutputLatency =
                (double)nativeBufferSize * 4 / m_deviceInfo->defaultSampleRate;
        } else {
            m_deviceInfo->defaultLowInputLatency =
                (double)GetApproximateLowBufferSize() / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultLowOutputLatency =
                (double)GetApproximateLowBufferSize() / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighInputLatency =
                (double)GetApproximateLowBufferSize() * 4 / m_deviceInfo->defaultSampleRate;
            m_deviceInfo->defaultHighOutputLatency =
                (double)GetApproximateLowBufferSize() * 4 / m_deviceInfo->defaultSampleRate;
        }

        (*hostApi)->deviceInfos[i] = m_deviceInfo;
        ++(*hostApi)->info.deviceCount;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface(&m_oboeHostApi->callbackStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     GetStreamCpuLoad, PaUtil_DummyRead,
                                     PaUtil_DummyWrite,
                                     PaUtil_DummyGetReadAvailable,
                                     PaUtil_DummyGetWriteAvailable);

    PaUtil_InitializeStreamInterface(&m_oboeHostApi->blockingStreamInterface,
                                     CloseStream, StartStream, StopStream,
                                     AbortStream, IsStreamStopped,
                                     IsStreamActive, GetStreamTime,
                                     PaUtil_DummyGetCpuLoad, ReadStream,
                                     WriteStream, GetStreamReadAvailable,
                                     GetStreamWriteAvailable);

    if(m_result == 0)
        LOGV("PaOboe initialized correctly");
    else
        LOGE("PaOboe Initialized with error code %d", m_result);
    return m_result;

error:
    if (m_oboeHostApi) {
        if (m_oboeHostApi->allocations) {
            PaUtil_FreeAllAllocations(m_oboeHostApi->allocations);
            PaUtil_DestroyAllocationGroup(m_oboeHostApi->allocations);
        }

        PaUtil_FreeMemory(m_oboeHostApi);
    }
    LOGE("PaOboe_Initialize; error code: %d", m_result);
    return m_result;
}


/**
    \brief  Interrupts the stream and frees the memory that was allocated to sustain the stream.
    \param  hostApi points towards a HostApiRepresentation, which is a structure representing the interface
            to a host API (see struct in "pa_hostapi.h").
*/
static void Terminate(struct PaUtilHostApiRepresentation *hostApi) {
    auto *m_oboeHostApi = (PaOboeHostApiRepresentation *)hostApi;

    if (!(m_oboeHostApi->oboeEngine->closeStream()))
        LOGW("Couldn't close the streams correctly.");

    if (m_oboeHostApi->allocations) {
        PaUtil_FreeAllAllocations(m_oboeHostApi->allocations);
        PaUtil_DestroyAllocationGroup(m_oboeHostApi->allocations);
    }

    PaUtil_FreeMemory(m_oboeHostApi);
}


/**
    \brief  Checks if the initialized values are supported by the selected device(s).
    \param  hostApi points towards a HostApiRepresentation, which is a structure representing the interface
            to a host API (see struct in "pa_hostapi.h").
    \param  inputParameters points towards the parameters given to the input stream.
    \param  outputParameters points towards the parameters given to the output stream.
    \param  sampleRate is the value of the sample rate we want to check if it's supported.
*/
static PaError IsFormatSupported(struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate) {
    int m_inputChannelCount, m_outputChannelCount;
    PaSampleFormat m_inputSampleFormat, m_outputSampleFormat;
    auto *m_oboeHostApi = (PaOboeHostApiRepresentation *)hostApi;
    LOGI("Checking format...");

    if (inputParameters) {
        m_inputChannelCount = inputParameters->channelCount;
        m_inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (m_inputSampleFormat & paCustomFormat)
            return paSampleFormatNotSupported;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if (m_inputChannelCount >
            hostApi->deviceInfos[inputParameters->device]->maxInputChannels
            )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo)
        {
            // Only has an effect on ANDROID_API>=28. TODO: Check if it needs a rework.
            InputPreset m_androidRecordingPreset =
                ((PaOboeStreamInfo *)outputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (m_androidRecordingPreset != InputPreset::Generic &&
                m_androidRecordingPreset != InputPreset::Camcorder &&
                m_androidRecordingPreset != InputPreset::VoiceRecognition &&
                m_androidRecordingPreset != InputPreset::VoiceCommunication
                // Should I add compatibility with VoicePerformance?
                )
                return paIncompatibleHostApiSpecificStreamInfo;
        }
    } else {
        m_inputChannelCount = 0;
    }

    if (outputParameters) {
        m_outputChannelCount = outputParameters->channelCount;
        m_outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if (m_outputSampleFormat & paCustomFormat)
            return paSampleFormatNotSupported;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if (m_outputChannelCount >
            hostApi->deviceInfos[outputParameters->device]->maxOutputChannels
            )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28. TODO: Check if it needs a rework.
            Usage m_androidOutputUsage =
                ((PaOboeStreamInfo *)outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (m_androidOutputUsage != Usage::Media &&
                m_androidOutputUsage != Usage::Notification &&
                m_androidOutputUsage != Usage::NotificationEvent &&
                m_androidOutputUsage != Usage::NotificationRingtone &&
                m_androidOutputUsage != Usage::VoiceCommunication &&
                m_androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                m_androidOutputUsage != Usage::Alarm
                // See if more are needed.
                )
                return paIncompatibleHostApiSpecificStreamInfo;
        }
    } else {
        m_outputChannelCount = 0;
    }

    if (m_outputChannelCount > 0) {
        if (IsOutputSampleRateSupported(m_oboeHostApi, sampleRate) != paNoError)
            return paInvalidSampleRate;
    }
    if (m_inputChannelCount > 0) {
        if (IsInputSampleRateSupported(m_oboeHostApi, sampleRate) != paNoError)
            return paInvalidSampleRate;
    }

    return paFormatIsSupported;
}


/**
    \brief  Initializes output stream parameters and allocates the memory of the output buffers.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  androidOutputUsage is an attribute that expresses why we are opening the output stream. This information
            can be used by certain platforms to make more refined volume or routing decisions. It only has an effect
            on Android API 28+.
    \param  sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
            the correct amount of memory.
*/
static PaError InitializeOutputStream(PaOboeHostApiRepresentation *oboeHostApi,
                                      Usage androidOutputUsage, double sampleRate) {
    LOGV("Initializing output stream.");
    return oboeHostApi->oboeEngine->openStream(Direction::Output,
                                                sampleRate,
                                                androidOutputUsage,
                                                Generic //Won't be used, so we put the default value.
                                                );
}

/**
    \brief  Initializes input stream parameters and allocates the memory of the input buffers.
    \param  oboeHostApi points towards a OboeHostApiRepresentation, which is a structure representing the interface
            to the Oboe Host API (see struct defined at the top of this file).
    \param  androidInputPreset is an attribute that defines the audio source. This information
            defines both a default physical source of audio signal, and a recording configuration.
            It only has an effect on Android API 28+.
    \param  sampleRate is the sample rate we want for the audio stream we want to initialize. This is used to allocate
            the correct amount of memory.
*/
static PaError InitializeInputStream(PaOboeHostApiRepresentation *oboeHostApi,
                                     InputPreset androidInputPreset, double sampleRate) {
    LOGV("Initializing input stream.");
    return oboeHostApi->oboeEngine->openStream(Direction::Input,
                                               sampleRate,
                                               Usage::Media,   //Won't be used, so we put the default value.
                                               androidInputPreset
                                               );
}


/**
    \brief  Opens the audio stream(s).
    \param  hostApi points towards a HostApiRepresentation, which is a structure representing the interface
            to a host API (see struct in "pa_hostapi.h").
    \param  s points to a pointer to a PaStream, which is an audiostream used and built by portaudio.
    \param  inputParameters points towards the parameters given to the input stream.
    \param  outputParameters points towards the parameters given to the output stream.
    \param  sampleRate is the value of the sample rate we want for our stream.
    \param  framesPerBuffer stores the number of frames per buffer we want for our stream.
    \param  streamFlags flags used to control the behavior of a stream.
    \param  streamCallback points to a callback function that allows the stream to recieve or transmit data.
    \param  userData ...
*/
static PaError OpenStream(struct PaUtilHostApiRepresentation *hostApi,
                          PaStream **s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData) {
    PaError m_error = paNoError;
    auto m_oboeHostApi = (PaOboeHostApiRepresentation *)hostApi;
    unsigned long m_framesPerHostBuffer; /* these may not be equivalent for all implementations */
    int m_inputChannelCount, m_outputChannelCount;
    PaSampleFormat m_inputSampleFormat, m_outputSampleFormat;
    PaSampleFormat m_hostInputSampleFormat, m_hostOutputSampleFormat;

    Usage m_androidOutputUsage = Usage::VoiceCommunication;
    InputPreset m_androidInputPreset = InputPreset::Generic;

    LOGI("OpenStream Called.");

    if (inputParameters) {
        m_inputChannelCount = inputParameters->channelCount;
        m_inputSampleFormat = inputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but for now we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default.*/
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if (m_inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels)
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if (inputParameters->hostApiSpecificStreamInfo) {
            // Only has an effect on ANDROID_API>=28. TODO: Check if it needs a rework.
            m_androidInputPreset =
                ((PaOboeStreamInfo *)outputParameters->hostApiSpecificStreamInfo)->androidInputPreset;
            if (m_androidInputPreset != InputPreset::Generic &&
                m_androidInputPreset != InputPreset::Camcorder &&
                m_androidInputPreset != InputPreset::VoiceRecognition &&
                m_androidInputPreset != InputPreset::VoiceCommunication
                // Should I add compatibility with VoicePerformance?
                )
                return paIncompatibleHostApiSpecificStreamInfo;
        }
        m_hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(
            paInt16, m_inputSampleFormat);
    } else {
        m_inputChannelCount = 0;
        m_inputSampleFormat = m_hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings. */
    }

    if (outputParameters) {
        m_outputChannelCount = outputParameters->channelCount;
        m_outputSampleFormat = outputParameters->sampleFormat;

        /* Oboe supports alternate device specification with API>=28, but for now we reject the use of
            paUseHostApiSpecificDeviceSpecification and stick with the default.*/
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification)
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if (m_outputChannelCount >
            hostApi->deviceInfos[outputParameters->device]->maxOutputChannels)
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if (outputParameters->hostApiSpecificStreamInfo) {
            m_androidOutputUsage =
                ((PaOboeStreamInfo *)outputParameters->hostApiSpecificStreamInfo)->androidOutputUsage;
            if (m_androidOutputUsage != Usage::Media &&
                m_androidOutputUsage != Usage::Notification &&
                m_androidOutputUsage != Usage::NotificationEvent &&
                m_androidOutputUsage != Usage::NotificationRingtone &&
                m_androidOutputUsage != Usage::VoiceCommunication &&
                m_androidOutputUsage != Usage::VoiceCommunicationSignalling &&
                m_androidOutputUsage != Usage::Alarm
                // See if more are needed.
                )
                return paIncompatibleHostApiSpecificStreamInfo;
        }

        m_hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(
            paInt16, m_outputSampleFormat);
    } else {
        m_outputChannelCount = 0;
        m_outputSampleFormat = m_hostOutputSampleFormat = paInt16;
    }

    /* validate platform specific flags */
    if ((streamFlags & paPlatformSpecificFlags) != 0)
        return paInvalidFlag; /* unexpected platform specific flag */

    if (framesPerBuffer == paFramesPerBufferUnspecified) {
        if (outputParameters) {
            m_framesPerHostBuffer =
                (unsigned long)(outputParameters->suggestedLatency * sampleRate);
        } else {
            m_framesPerHostBuffer =
                (unsigned long)(inputParameters->suggestedLatency * sampleRate);
        }
    } else {
        m_framesPerHostBuffer = framesPerBuffer;
    }

    OboeStream* m_oboeStream = m_oboeHostApi->oboeEngine->initializeOboeStream();

    if (!m_oboeStream) {
        m_error = paInsufficientMemory;
        goto error;
    }

    m_oboeHostApi->oboeEngine->setEngineAddress(
            reinterpret_cast<long>(m_oboeHostApi->oboeEngine));

    if (streamCallback) {
        PaUtil_InitializeStreamRepresentation(&(m_oboeStream->streamRepresentation),
                                              &m_oboeHostApi->callbackStreamInterface,
                                              streamCallback, userData);
    } else {
        PaUtil_InitializeStreamRepresentation(&(m_oboeStream->streamRepresentation),
                                              &m_oboeHostApi->blockingStreamInterface,
                                              streamCallback, userData);
    }

    PaUtil_InitializeCpuLoadMeasurer(&(m_oboeStream->cpuLoadMeasurer), sampleRate);

    m_error = PaUtil_InitializeBufferProcessor(&(m_oboeStream->bufferProcessor),
                                                m_inputChannelCount,
                                                m_inputSampleFormat,
                                                m_hostInputSampleFormat,
                                                m_outputChannelCount,
                                                m_outputSampleFormat,
                                                m_hostOutputSampleFormat,
                                                sampleRate, streamFlags,
                                                framesPerBuffer,
                                                m_framesPerHostBuffer,
                                                paUtilFixedHostBufferSize,
                                                streamCallback, userData);
    if (m_error != paNoError)
        goto error;

    m_oboeStream->streamRepresentation.streamInfo.sampleRate = sampleRate;
    m_oboeStream->isBlocking = (streamCallback == nullptr);
    m_oboeStream->framesPerHostCallback = m_framesPerHostBuffer;
    m_oboeStream->bytesPerFrame = sizeof(int32_t);
    m_oboeStream->cbFlags = 0;
    m_oboeStream->isStopped = true;
    m_oboeStream->isActive = false;

    if (!(m_oboeStream->isBlocking)){}
//        PaUnixThreading_Initialize();

    if (m_inputChannelCount > 0) {
        m_oboeStream->hasInput = true;
        m_oboeStream->streamRepresentation.streamInfo.inputLatency =
                ((PaTime) PaUtil_GetBufferProcessorInputLatencyFrames(
                        &(m_oboeStream->bufferProcessor)) +
                 m_oboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeInputStream(m_oboeHostApi,
                                     m_androidInputPreset, sampleRate),
               "Initializing inputstream failed")
    } else { m_oboeStream->hasInput = false; }

    if (m_outputChannelCount > 0) {
        m_oboeStream->hasOutput = true;
        m_oboeStream->streamRepresentation.streamInfo.outputLatency =
                ((PaTime) PaUtil_GetBufferProcessorOutputLatencyFrames(
                        &m_oboeStream->bufferProcessor)
                 + m_oboeStream->framesPerHostCallback) / sampleRate;
        ENSURE(InitializeOutputStream(m_oboeHostApi,
                                      m_androidOutputUsage, sampleRate),
               "Initializing outputstream failed");
    } else { m_oboeStream->hasOutput = false; }

    *s = (PaStream *) m_oboeStream;
    return m_error;

error:
    if (m_oboeStream)
        PaUtil_FreeMemory(m_oboeStream);

    LOGE("Error opening stream. Error code: %d", m_error);

    return m_error;
}


/**
    \brief  Closes a stream and frees the memory that was allocated to sustain the stream. When CloseStream() is called,
            the multi-api layer ensures that the stream has already been stopped or aborted.
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
*/
static PaError CloseStream(PaStream *s) {
    PaError m_result = paNoError;
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    LOGI("CloseStream called.");

    if (!(m_oboeEngine->closeStream()))
        LOGW("Couldn't close the streams correctly.");

    PaUtil_TerminateBufferProcessor(&m_stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&m_stream->streamRepresentation);

    for (int i = 0; i < numberOfBuffers; ++i) {
        if (m_stream->hasOutput)
            PaUtil_FreeMemory(m_stream->outputBuffers[i]);
        if (m_stream->hasInput)
            PaUtil_FreeMemory(m_stream->inputBuffers[i]);
    }

    if (m_stream->hasOutput)
        PaUtil_FreeMemory(m_stream->outputBuffers);
    if (m_stream->hasInput)
        PaUtil_FreeMemory(m_stream->inputBuffers);

    PaUtil_FreeMemory(m_stream);
    return m_result;
}


/**
    \brief  Starts a stream and sets the memory required by it. 
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
*/
static PaError StartStream(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);

    LOGI("StartStream called.");

    PaUtil_ResetBufferProcessor(&m_stream->bufferProcessor);

    if(m_stream->isActive) {
        LOGW("Stream was already active");
        StopStream(s);
        LOGW("Restarting...");
        StartStream(s);
    }

    m_stream->currentOutputBuffer = 0;
    m_stream->currentInputBuffer = 0;

    /* Initialize buffers */
    for (int i = 0; i < numberOfBuffers; ++i) {
        if (m_stream->hasOutput) {
            memset(m_stream->outputBuffers[m_stream->currentOutputBuffer], 0,
                   m_stream->framesPerHostCallback * m_stream->bytesPerFrame *
                    m_stream->bufferProcessor.outputChannelCount
                );
            m_stream->currentOutputBuffer = (m_stream->currentOutputBuffer + 1) % numberOfBuffers;
        }
        if (m_stream->hasInput) {
            memset(m_stream->inputBuffers[m_stream->currentInputBuffer], 0,
                   m_stream->framesPerHostCallback * m_stream->bytesPerFrame *
                    m_stream->bufferProcessor.inputChannelCount
                );
            m_stream->currentInputBuffer = (m_stream->currentInputBuffer + 1) % numberOfBuffers;
        }
    }

    /* Start the processing thread.*/
    if (!m_stream->isBlocking) {
        m_stream->callbackResult = paContinue;
        m_stream->oboeCallbackResult = DataCallbackResult::Continue;

        //PaUnixThread_New(&(m_stream->streamThread), (void*) StreamProcessingCallback,
        //                 (void *) m_stream, 0, 0);
    }

    m_stream->isStopped = false;
    m_stream->isActive = true;
    m_stream->doStop = false;
    m_stream->doAbort = false;

    if(!(m_oboeEngine->startStream()))
        return paUnanticipatedHostError;
    else
        return paNoError;
}

/**
    \brief  Stops a stream and finishes the stream callback. 
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
*/
static PaError StopStream(PaStream *s) {
    PaError m_error = paNoError;
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    LOGI("StopStream called.");

    if(m_stream->isStopped){
        LOGW("StopStream was called, but stream was already stopped.");
    }
    else {
        if (!(m_stream->isBlocking)) {
            m_stream->doStop = true;
//        PaUnixThread_Terminate(&m_stream->streamThread, 1, &m_error);
        }
        if (!(m_oboeEngine->stopStream())) {
            LOGE("Couldn't stop the stream(s).");
            m_error = paUnanticipatedHostError;
        }

        m_stream->isActive = false;
        m_stream->isStopped = true;
        if (m_stream->streamRepresentation.streamFinishedCallback != nullptr)
            m_stream->streamRepresentation.streamFinishedCallback(
                    m_stream->streamRepresentation.userData);
    }

    return m_error;
}


/**
    \brief  Forcefully stops a stream without enqueue, and finishes the stream callback. 
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
*/
static PaError AbortStream(PaStream *s) {
    PaError m_error = paNoError;
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    LOGI("AbortStream called.");

    if (!m_stream->isBlocking) {
        m_stream->doAbort = true;
//        PaUnixThread_Terminate(&m_stream->streamThread, 0, &m_error);
    }

    /* stop immediately so enqueue has no effect */
    if (!(m_oboeEngine->abortStream())) {
        LOGE("Couldn't abort the stream.");
        m_error = paUnanticipatedHostError;
    }

    m_stream->isActive = false;
    m_stream->isStopped = true;
    if (m_stream->streamRepresentation.streamFinishedCallback != nullptr)
        m_stream->streamRepresentation.streamFinishedCallback(
            m_stream->streamRepresentation.userData);

    return m_error;
}



static PaError IsStreamStopped(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    return m_stream->isStopped;
}


static PaError IsStreamActive(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    LOGI("Checking if stream is active.");
    return m_stream->isActive;
}


static PaTime GetStreamTime(PaStream *s) {
    LOGI("Getting Stream Time.");
    return PaUtil_GetTime();
}


static double GetStreamCpuLoad(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    LOGI("Getting Stream CPU load");
    return PaUtil_GetCpuLoad(&m_stream->cpuLoadMeasurer);
}


/**
    \brief  Reads an input stream buffer by buffer. 
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
    \param  buffer is the address of the first sample.
    \param  frames is the total number of frames to read.
*/
static PaError ReadStream(PaStream *s, void *buffer, unsigned long frames) {
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    void *m_userBuffer = buffer;
    unsigned m_framesToRead = frames;
    PaError m_error = paNoError;

    while( frames > 0 )
    {
        m_framesToRead = PA_MIN( m_stream->framesPerHostCallback, frames );
        PaUtil_SetInputFrameCount( &m_stream->bufferProcessor, m_framesToRead );
        PaUtil_SetInterleavedInputChannels( &m_stream->bufferProcessor, 0,
                                             m_stream->inputBuffers[m_stream->currentInputBuffer], 0 );
        PaUtil_CopyInput( &m_stream->bufferProcessor, &m_userBuffer, m_framesToRead);
        if(!(m_oboeEngine->readStream(m_userBuffer,
                                       m_framesToRead * m_stream->bufferProcessor.inputChannelCount)))
            m_error = paInternalError;

        m_stream->currentInputBuffer = (m_stream->currentInputBuffer + 1) % numberOfBuffers;
        frames -= m_framesToRead;
    }

    return m_error;
}


/**
    \brief  Writes on an output stream buffer by buffer. 
    \param  s points to a PaStream, which is an audiostream used and built by portaudio.
    \param  buffer is the address of the first sample.
    \param  frames is the total number of frames to write.
*/
static PaError WriteStream(PaStream *s, const void *buffer, unsigned long frames) {
    auto *m_stream = (OboeStream *)s;
    auto *m_oboeEngine = reinterpret_cast<OboeEngine *>(m_stream->engineAddress);
    const void *m_userBuffer = buffer;
    unsigned m_framesToWrite;
    PaError m_error = paNoError;

    while( frames > 0 )
    {
        m_framesToWrite = PA_MIN( m_stream->framesPerHostCallback, frames );
        PaUtil_SetOutputFrameCount( &m_stream->bufferProcessor, m_framesToWrite );
        PaUtil_SetInterleavedOutputChannels( &m_stream->bufferProcessor, 0,
                                             m_stream->outputBuffers[m_stream->currentOutputBuffer], 0 );
        PaUtil_CopyOutput( &m_stream->bufferProcessor, &m_userBuffer, m_framesToWrite);
        if(!(m_oboeEngine->writeStream(m_userBuffer,
                                       m_framesToWrite * m_stream->bufferProcessor.outputChannelCount)))
            m_error = paInternalError;

        m_stream->currentOutputBuffer = (m_stream->currentOutputBuffer + 1) % numberOfBuffers;
        frames -= m_framesToWrite;
    }

    return m_error;
}


static signed long GetStreamReadAvailable(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    LOGI("GetStreamReadAvailable called.");
    /*StreamState m_state;

    m_state = m_stream->inputStream->getState();*/
    return m_stream->framesPerHostCallback * (numberOfBuffers);
}


static signed long GetStreamWriteAvailable(PaStream *s) {
    auto *m_stream = (OboeStream *)s;
    LOGI("GetStreamWriteAvailable called.");
    /*StreamState m_state;

    m_state = m_stream->outputStream->getState();*/
    return m_stream->framesPerHostCallback * (numberOfBuffers);
}


static unsigned long GetApproximateLowBufferSize() {
    LOGV("Getting approximate low buffer size.");

//  This function should return the following values, but was changed in order to add compatibility
//  with KCTI for android.

//    if (__ANDROID_API__ <= 23)
//        return 256;
//    else
//        return 192;

    return 1024;
}


int32_t getSelectedDevice(Direction direction){
    if(direction == Direction::Input)
        return inputDeviceId;
    else
        return outputDeviceId;
}


int32_t millisToNanos (int32_t timeInMillis){
    return timeInMillis * 1000000;
}


void PaOboe_SetSelectedDevice(Direction direction, int32_t deviceID){
    LOGI("Selectng device...");
    if(direction == Direction::Input)
        inputDeviceId = deviceID;
    else
        outputDeviceId = deviceID;
}


void PaOboe_SetNativeBufferSize(unsigned long bufferSize) {
    nativeBufferSize = bufferSize;
}


void PaOboe_SetNumberOfBuffers(unsigned buffers) {
    numberOfBuffers = buffers;
}