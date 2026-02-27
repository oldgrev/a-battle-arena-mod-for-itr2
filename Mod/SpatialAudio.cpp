/*
    SpatialAudio.cpp – Windows waveOut 3D positional audio engine.

    Architecture:
      - A background audio thread runs the waveOut callback pump.
      - Each active sound holds its parsed PCM data, current playback offset,
        and spatial parameters (source position, gain L/R, LPF state).
      - On every Tick() (called from the game thread) spatial parameters are
        recomputed from current source/listener positions at ~10 fps.
      - The waveOut callback thread reads spatial params (atomics / lock-free
        snapshot) to mix each buffer fill.

    3D model:
      1. Distance attenuation – linear falloff from kInnerRadius to kOuterRadius.
      2. Stereo panning (ILD) – dot-product of source direction with listener
         right vector gives pan [-1, +1].
      3. Low-pass filter – one-pole IIR whose alpha decreases with distance,
         simulating air absorption.

    AILEARNINGS (spatial audio):
      - waveOut is synchronous-enough for VR; 4x40ms buffers at 48kHz gives
        160ms total latency headroom, which is acceptable for environmental and
        voice-line audio.
      - Prediction placeholders allow future integration of head-tracking
        prediction or NPC movement extrapolation to reduce perceived latency.
*/

#include "SpatialAudio.hpp"
#include "Logging.hpp"
#include "ModTuning.hpp"

#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace Mod::SpatialAudio
{
    // =====================================================================
    // Tuning aliases (canonical values live in ModTuning.hpp)
    // =====================================================================

    static constexpr float kInnerRadius       = Mod::Tuning::kSpatialInnerRadius;
    static constexpr float kOuterRadius       = Mod::Tuning::kSpatialOuterRadius;
    static constexpr float kLpfStartRadius    = Mod::Tuning::kSpatialLpfStartRadius;
    static constexpr float kLpfEndRadius      = Mod::Tuning::kSpatialLpfEndRadius;
    static constexpr float kLpfAlphaMin       = Mod::Tuning::kSpatialLpfAlphaMin;
    static constexpr float kLpfAlphaMax       = Mod::Tuning::kSpatialLpfAlphaMax;
    static constexpr float kOutputGain        = Mod::Tuning::kSpatialOutputGain;
    static constexpr float kPositionUpdateHz  = Mod::Tuning::kSpatialPositionUpdateHz;
    static constexpr int   kOutputSampleRate  = Mod::Tuning::kSpatialOutputSampleRate;
    static constexpr int   kOutputChannels    = Mod::Tuning::kSpatialOutputChannels;
    static constexpr int   kOutputBitsPerSample = Mod::Tuning::kSpatialOutputBitsPerSample;
    static constexpr int   kBufferCount       = Mod::Tuning::kSpatialBufferCount;
    static constexpr int   kBufferDurationMs  = Mod::Tuning::kSpatialBufferDurationMs;

    // =====================================================================
    // Internal types
    // =====================================================================

    struct ParsedWav
    {
        std::vector<int16_t> samples; // interleaved 16-bit PCM
        uint16_t numChannels = 0;
        uint32_t sampleRate  = 0;
    };

    // Spatial snapshot: the DSP thread reads these per-buffer-fill.
    struct SpatialParams
    {
        float gainL       = 1.0f;
        float gainR       = 1.0f;
        float lpfAlpha    = 1.0f;   // 1.0 = passthrough
        float distance    = 0.0f;   // for diagnostics
        bool  is3D        = true;
    };

    struct SoundInstance
    {
        SoundHandle       handle         = kInvalidHandle;
        ParsedWav         wav;
        size_t            playbackOffset = 0;  // in samples (per-channel frames * channels)
        float             masterVolume   = 1.0f;
        bool              loop           = false;
        std::atomic<bool> stopped{false};
        std::string       descriptor;          // for diagnostics / tag matching
        std::string       tag;                 // actor/file tag for StopByTag

        // Spatial state (written by game thread, read by audio thread)
        Vec3              sourcePos      = {};
        SpatialParams     spatial;
        
        // LPF state (per channel, maintained by audio thread)
        float             lpfStateL      = 0.0f;
        float             lpfStateR      = 0.0f;

        // Prediction overrides (cleared each Tick)
        bool              hasPredictedPos = false;
        Vec3              predictedPos   = {};
    };

    // =====================================================================
    // Module state
    // =====================================================================

    static std::mutex              gMutex;
    static std::vector<SoundInstance*> gInstances;
    static std::atomic<uint32_t>   gNextHandle{1};

    // Listener state (written by game thread, read by audio thread)
    static Vec3 gListenerPos     = {};
    static Vec3 gListenerFwd     = {1.0f, 0.0f, 0.0f};
    static Vec3 gListenerRight   = {0.0f, 1.0f, 0.0f};
    static bool gListenerPredicted = false;
    static Vec3 gPredictedListenerPos   = {};
    static Vec3 gPredictedListenerFwd   = {};
    static Vec3 gPredictedListenerRight = {};

    // Position update throttle
    static uint64_t gLastPositionUpdateMs = 0;

    // waveOut state
    static HWAVEOUT gWaveOut = nullptr;
    static bool     gInitialized = false;
    static bool     gShuttingDown = false;

    // Buffer ring
    struct AudioBuffer
    {
        WAVEHDR  header  = {};
        std::vector<int16_t> data;
    };
    static AudioBuffer gBuffers[kBufferCount];

    // =====================================================================
    // WAV file parser
    // =====================================================================

    static bool ParseWavFile(const std::string& path, ParsedWav& out, std::string* outErr)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
        {
            if (outErr) *outErr = "cannot open wav: " + path;
            return false;
        }
        const auto fileSize = static_cast<size_t>(f.tellg());
        f.seekg(0);

        if (fileSize < 12)
        {
            if (outErr) *outErr = "wav too small";
            return false;
        }

        char riff[4]; uint32_t riffSz; char wave[4];
        f.read(riff, 4);
        f.read(reinterpret_cast<char*>(&riffSz), 4);
        f.read(wave, 4);
        if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0)
        {
            if (outErr) *outErr = "not a RIFF/WAVE file";
            return false;
        }

        uint16_t numChannels = 0, bitsPerSample = 0;
        uint32_t sampleRate = 0;
        bool hasFmt = false;

        while (f.good() && static_cast<size_t>(f.tellg()) + 8 <= fileSize)
        {
            char id[4]; uint32_t chunkSz;
            f.read(id, 4);
            f.read(reinterpret_cast<char*>(&chunkSz), 4);
            if (!f) break;
            const auto chunkStart = f.tellg();

            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                uint16_t audioFmt;
                f.read(reinterpret_cast<char*>(&audioFmt), 2);
                if (audioFmt != 1)
                {
                    if (outErr) *outErr = "wav audioFormat != 1 (not PCM)";
                    return false;
                }
                f.read(reinterpret_cast<char*>(&numChannels), 2);
                f.read(reinterpret_cast<char*>(&sampleRate), 4);
                f.seekg(6, std::ios::cur); // byteRate + blockAlign
                f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
                if (bitsPerSample != 16)
                {
                    if (outErr) *outErr = "wav bitsPerSample != 16";
                    return false;
                }
                hasFmt = true;
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                if (!hasFmt)
                {
                    if (outErr) *outErr = "data chunk before fmt chunk";
                    return false;
                }
                const size_t sampleCount = chunkSz / 2; // 16-bit = 2 bytes per sample
                out.samples.resize(sampleCount);
                f.read(reinterpret_cast<char*>(out.samples.data()), chunkSz);
                if (!f)
                {
                    if (outErr) *outErr = "truncated data chunk";
                    return false;
                }
                out.numChannels = numChannels;
                out.sampleRate  = sampleRate;
                return true;
            }

            // Skip to next chunk (pad to even size)
            const auto next = static_cast<std::streampos>(
                static_cast<size_t>(chunkStart) + chunkSz + (chunkSz & 1));
            f.seekg(next);
        }

        if (outErr) *outErr = "no data chunk found in wav";
        return false;
    }

    // =====================================================================
    // Spatial math
    // =====================================================================

    static float Vec3Length(const Vec3& v)
    {
        return std::sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    }

    static Vec3 Vec3Sub(const Vec3& a, const Vec3& b)
    {
        return { a.X - b.X, a.Y - b.Y, a.Z - b.Z };
    }

    static float Vec3Dot(const Vec3& a, const Vec3& b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    // Compute spatial parameters from source and listener state.
    static SpatialParams ComputeSpatial(
        const Vec3& sourcePos,
        const Vec3& listenerPos,
        const Vec3& listenerFwd,
        const Vec3& listenerRight,
        float masterVolume)
    {
        SpatialParams sp;
        sp.is3D = true;

        Vec3 toSource = Vec3Sub(sourcePos, listenerPos);
        float dist = Vec3Length(toSource);
        sp.distance = dist;

        // --- Distance attenuation (linear) ---
        float atten = 1.0f;
        if (dist <= kInnerRadius)
        {
            atten = 1.0f;
        }
        else if (dist >= kOuterRadius)
        {
            atten = 0.0f;
        }
        else
        {
            atten = 1.0f - (dist - kInnerRadius) / (kOuterRadius - kInnerRadius);
        }

        float gain = atten * masterVolume;
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 4.0f) gain = 4.0f;

        // --- Stereo panning ---
        // Project direction onto listener right axis.
        // pan = -1 (full left), 0 (center), +1 (full right)
        float pan = 0.0f;
        if (dist > 1.0f)
        {
            float invDist = 1.0f / dist;
            Vec3 dir = { toSource.X * invDist, toSource.Y * invDist, toSource.Z * invDist };
            pan = Vec3Dot(dir, listenerRight);
            // Clamp to [-1, 1]
            if (pan < -1.0f) pan = -1.0f;
            if (pan > 1.0f)  pan = 1.0f;
        }

        // Constant-power panning (sin/cos law):
        //   L = cos(θ), R = sin(θ) where θ = (pan+1)/2 * π/2
        float theta = (pan + 1.0f) * 0.5f * 1.5707963f; // π/2
        sp.gainL = gain * std::cosf(theta);
        sp.gainR = gain * std::sinf(theta);

        // --- Low-pass filter ---
        if (dist <= kLpfStartRadius)
        {
            sp.lpfAlpha = kLpfAlphaMax; // passthrough
        }
        else if (dist >= kLpfEndRadius)
        {
            sp.lpfAlpha = kLpfAlphaMin;
        }
        else
        {
            float t = (dist - kLpfStartRadius) / (kLpfEndRadius - kLpfStartRadius);
            sp.lpfAlpha = kLpfAlphaMax + t * (kLpfAlphaMin - kLpfAlphaMax);
        }

        return sp;
    }

    // =====================================================================
    // Prediction helpers (placeholders for future use)
    // =====================================================================

    // PredictSourcePosition: returns the predicted position of a sound source
    // accounting for velocity and audio latency. Currently returns the raw position.
    static Vec3 PredictSourcePosition(const Vec3& currentPos, const Vec3& /*velocity*/, float /*latencySec*/)
    {
        // TODO: return currentPos + velocity * latencySec
        return currentPos;
    }

    // PredictListenerState: returns the predicted listener position/orientation
    // accounting for head tracking latency. Currently returns the raw state.
    static void PredictListenerState(
        const Vec3& pos, const Vec3& fwd, const Vec3& right,
        float /*latencySec*/,
        Vec3& outPos, Vec3& outFwd, Vec3& outRight)
    {
        // TODO: extrapolate based on angular velocity and translational velocity
        outPos = pos;
        outFwd = fwd;
        outRight = right;
    }

    // =====================================================================
    // DSP: mix active sounds into an output buffer
    // =====================================================================

    static void MixIntoBuffer(int16_t* outputStereo, int framesToMix)
    {
        // Zero the output buffer
        std::memset(outputStereo, 0, framesToMix * 2 * sizeof(int16_t));

        // Temporary float accumulator for mixing
        std::vector<float> accum(framesToMix * 2, 0.0f);

        std::lock_guard<std::mutex> lock(gMutex);

        for (auto* inst : gInstances)
        {
            if (inst->stopped.load(std::memory_order_relaxed))
                continue;

            const SpatialParams& sp = inst->spatial;
            const float gainL = sp.gainL;
            const float gainR = sp.gainR;
            const float alpha = sp.lpfAlpha;
            float lpfL = inst->lpfStateL;
            float lpfR = inst->lpfStateR;

            const int srcChannels = inst->wav.numChannels;
            const int srcSampleRate = inst->wav.sampleRate;
            const size_t totalSrcSamples = inst->wav.samples.size();
            const int16_t* srcData = inst->wav.samples.data();
            const size_t totalSrcFrames = totalSrcSamples / srcChannels;

            // Simple resampling ratio (nearest-neighbor for now)
            const double resampleRatio = static_cast<double>(srcSampleRate) / static_cast<double>(kOutputSampleRate);

            for (int i = 0; i < framesToMix; ++i)
            {
                // Calculate source frame position from output frame
                size_t srcFrame = inst->playbackOffset + static_cast<size_t>(i * resampleRatio);

                // Handle looping / end of data
                if (srcFrame >= totalSrcFrames)
                {
                    if (inst->loop)
                    {
                        // Wrap around for looping (adjust playbackOffset too)
                        inst->playbackOffset = 0;
                        srcFrame = static_cast<size_t>(i * resampleRatio);
                        if (srcFrame >= totalSrcFrames)
                        {
                            // Still past end (file shorter than one buffer)
                            srcFrame = srcFrame % totalSrcFrames;
                        }
                    }
                    else
                    {
                        inst->stopped.store(true, std::memory_order_relaxed);
                        break;
                    }
                }

                // Read source sample(s)
                float sampleL, sampleR;
                if (srcChannels == 1)
                {
                    float s = static_cast<float>(srcData[srcFrame]);
                    sampleL = s;
                    sampleR = s;
                }
                else
                {
                    // Stereo or multi-channel: take first two channels
                    size_t idx = srcFrame * srcChannels;
                    sampleL = static_cast<float>(srcData[idx]);
                    sampleR = static_cast<float>(srcData[idx + 1]);
                }

                // Apply gains (3D panning + attenuation)
                sampleL *= gainL;
                sampleR *= gainR;

                // Apply one-pole low-pass filter
                if (alpha < 0.999f)
                {
                    lpfL = alpha * sampleL + (1.0f - alpha) * lpfL;
                    lpfR = alpha * sampleR + (1.0f - alpha) * lpfR;
                    sampleL = lpfL;
                    sampleR = lpfR;
                }
                else
                {
                    lpfL = sampleL;
                    lpfR = sampleR;
                }

                accum[i * 2    ] += sampleL;
                accum[i * 2 + 1] += sampleR;
            }

            // Advance playback offset
            size_t framesConsumed = static_cast<size_t>(framesToMix * resampleRatio);
            inst->playbackOffset += framesConsumed;

            // Store LPF state back
            inst->lpfStateL = lpfL;
            inst->lpfStateR = lpfR;
        }

        // Convert float accum to int16 with output gain and clipping
        for (int i = 0; i < framesToMix * 2; ++i)
        {
            float s = accum[i] * kOutputGain;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            outputStereo[i] = static_cast<int16_t>(s);
        }
    }

    // =====================================================================
    // waveOut callback
    // =====================================================================

    static void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
                                          DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/)
    {
        if (uMsg != WOM_DONE)
            return;
        if (gShuttingDown)
            return;

        WAVEHDR* hdr = reinterpret_cast<WAVEHDR*>(dwParam1);
        if (!hdr)
            return;

        // Find which buffer this is
        AudioBuffer* buf = nullptr;
        for (int i = 0; i < kBufferCount; ++i)
        {
            if (&gBuffers[i].header == hdr)
            {
                buf = &gBuffers[i];
                break;
            }
        }
        if (!buf)
            return;

        // Mix new audio into this buffer
        int framesPerBuffer = (kOutputSampleRate * kBufferDurationMs) / 1000;
        MixIntoBuffer(buf->data.data(), framesPerBuffer);

        // Re-submit
        hdr->dwFlags &= ~WHDR_DONE;
        waveOutWrite(hwo, hdr, sizeof(WAVEHDR));
    }

    // =====================================================================
    // Public API: Lifecycle
    // =====================================================================

    bool Initialize(std::string* outError)
    {
        if (gInitialized)
            return true;

        gShuttingDown = false;

        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels        = kOutputChannels;
        wfx.nSamplesPerSec   = kOutputSampleRate;
        wfx.wBitsPerSample   = kOutputBitsPerSample;
        wfx.nBlockAlign      = (wfx.nChannels * wfx.wBitsPerSample) / 8;
        wfx.nAvgBytesPerSec  = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize           = 0;

        MMRESULT result = waveOutOpen(
            &gWaveOut,
            WAVE_MAPPER,
            &wfx,
            reinterpret_cast<DWORD_PTR>(WaveOutCallback),
            0,
            CALLBACK_FUNCTION);

        if (result != MMSYSERR_NOERROR)
        {
            char errBuf[256];
            waveOutGetErrorTextA(result, errBuf, sizeof(errBuf));
            std::string err = std::string("waveOutOpen failed: ") + errBuf;
            if (outError) *outError = err;
            LOG_ERROR("[SpatialAudio] " << err);
            return false;
        }

        // Prepare buffers
        int framesPerBuffer = (kOutputSampleRate * kBufferDurationMs) / 1000;
        int samplesPerBuffer = framesPerBuffer * kOutputChannels;

        for (int i = 0; i < kBufferCount; ++i)
        {
            auto& buf = gBuffers[i];
            buf.data.resize(samplesPerBuffer, 0);

            std::memset(&buf.header, 0, sizeof(WAVEHDR));
            buf.header.lpData         = reinterpret_cast<LPSTR>(buf.data.data());
            buf.header.dwBufferLength = samplesPerBuffer * sizeof(int16_t);

            waveOutPrepareHeader(gWaveOut, &buf.header, sizeof(WAVEHDR));

            // Mix silence into the initial buffer and submit
            MixIntoBuffer(buf.data.data(), framesPerBuffer);
            waveOutWrite(gWaveOut, &buf.header, sizeof(WAVEHDR));
        }

        gInitialized = true;
        LOG_INFO("[SpatialAudio] Initialized (sampleRate=" << kOutputSampleRate
            << ", buffers=" << kBufferCount
            << ", bufferMs=" << kBufferDurationMs << ")");
        return true;
    }

    void Shutdown()
    {
        if (!gInitialized)
            return;

        gShuttingDown = true;
        LOG_INFO("[SpatialAudio] Shutting down...");

        if (gWaveOut)
        {
            waveOutReset(gWaveOut);

            for (int i = 0; i < kBufferCount; ++i)
            {
                waveOutUnprepareHeader(gWaveOut, &gBuffers[i].header, sizeof(WAVEHDR));
                gBuffers[i].data.clear();
            }

            waveOutClose(gWaveOut);
            gWaveOut = nullptr;
        }

        // Free all instances
        {
            std::lock_guard<std::mutex> lock(gMutex);
            for (auto* inst : gInstances)
                delete inst;
            gInstances.clear();
        }

        gInitialized = false;
        LOG_INFO("[SpatialAudio] Shutdown complete");
    }

    void Tick()
    {
        if (!gInitialized)
            return;

        // Throttle position updates to ~kPositionUpdateHz
        uint64_t nowMs = static_cast<uint64_t>(GetTickCount64());
        uint64_t intervalMs = static_cast<uint64_t>(1000.0f / kPositionUpdateHz);
        if (nowMs - gLastPositionUpdateMs < intervalMs)
            return;
        gLastPositionUpdateMs = nowMs;

        // Determine effective listener state
        Vec3 effListenerPos, effListenerFwd, effListenerRight;
        if (gListenerPredicted)
        {
            effListenerPos   = gPredictedListenerPos;
            effListenerFwd   = gPredictedListenerFwd;
            effListenerRight = gPredictedListenerRight;
        }
        else
        {
            // Apply prediction placeholder (currently identity)
            PredictListenerState(
                gListenerPos, gListenerFwd, gListenerRight,
                static_cast<float>(kBufferDurationMs * kBufferCount) / 1000.0f,
                effListenerPos, effListenerFwd, effListenerRight);
        }

        // Update spatial params for each active instance
        {
            std::lock_guard<std::mutex> lock(gMutex);

            // Clean up finished instances
            gInstances.erase(
                std::remove_if(gInstances.begin(), gInstances.end(),
                    [](SoundInstance* inst) {
                        if (inst->stopped.load(std::memory_order_relaxed))
                        {
                            LOG_INFO("[SpatialAudio] Sound finished: " << inst->descriptor);
                            delete inst;
                            return true;
                        }
                        return false;
                    }),
                gInstances.end());

            for (auto* inst : gInstances)
            {
                if (inst->stopped.load(std::memory_order_relaxed))
                    continue;

                if (!inst->spatial.is3D)
                    continue; // 2D sounds don't need spatial updates

                // Determine effective source position
                Vec3 srcPos = inst->hasPredictedPos ? inst->predictedPos : inst->sourcePos;
                // Apply prediction placeholder
                srcPos = PredictSourcePosition(srcPos, {0, 0, 0}, 0.0f);

                inst->spatial = ComputeSpatial(
                    srcPos,
                    effListenerPos,
                    effListenerFwd,
                    effListenerRight,
                    inst->masterVolume);

                // Clear prediction override
                inst->hasPredictedPos = false;
            }
        }

        // Clear listener prediction override
        gListenerPredicted = false;
    }

    // =====================================================================
    // Public API: Playback
    // =====================================================================

    SoundHandle Play3D(
        const std::string& wavFilePath,
        const Vec3& sourcePos,
        const Vec3& listenerPos,
        const Vec3& listenerFwd,
        const Vec3& listenerRight,
        float volume,
        bool loop,
        std::string* outError)
    {
        if (!gInitialized)
        {
            // Auto-initialize
            std::string initErr;
            if (!Initialize(&initErr))
            {
                if (outError) *outError = "SpatialAudio not initialized: " + initErr;
                return kInvalidHandle;
            }
        }

        ParsedWav wav;
        std::string parseErr;
        if (!ParseWavFile(wavFilePath, wav, &parseErr))
        {
            if (outError) *outError = parseErr;
            LOG_ERROR("[SpatialAudio] Play3D parse failed: " << parseErr << " file='" << wavFilePath << "'");
            return kInvalidHandle;
        }

        auto* inst = new SoundInstance();
        inst->handle = gNextHandle.fetch_add(1, std::memory_order_relaxed);
        inst->wav = std::move(wav);
        inst->playbackOffset = 0;
        inst->masterVolume = (std::max)(0.0f, (std::min)(4.0f, volume));
        inst->loop = loop;
        inst->descriptor = wavFilePath;
        inst->tag = wavFilePath;
        inst->sourcePos = sourcePos;

        // Compute initial spatial params
        inst->spatial = ComputeSpatial(sourcePos, listenerPos, listenerFwd, listenerRight, inst->masterVolume);

        float durationSec = 0.0f;
        if (inst->wav.numChannels > 0 && inst->wav.sampleRate > 0)
        {
            durationSec = static_cast<float>(inst->wav.samples.size())
                        / static_cast<float>(inst->wav.numChannels * inst->wav.sampleRate);
        }

        {
            std::lock_guard<std::mutex> lock(gMutex);
            gInstances.push_back(inst);
        }

        LOG_INFO("[SpatialAudio] Play3D ok (handle=" << inst->handle
            << ", file='" << wavFilePath << "'"
            << ", ch=" << inst->wav.numChannels
            << ", sr=" << inst->wav.sampleRate
            << ", dur=" << durationSec << "s"
            << ", loop=" << (loop ? 1 : 0)
            << ", vol=" << volume
            << ", dist=" << inst->spatial.distance
            << ", gainL=" << inst->spatial.gainL
            << ", gainR=" << inst->spatial.gainR
            << ", lpfAlpha=" << inst->spatial.lpfAlpha << ")");

        return inst->handle;
    }

    SoundHandle Play2D(
        const std::string& wavFilePath,
        float volume,
        bool loop,
        std::string* outError)
    {
        if (!gInitialized)
        {
            std::string initErr;
            if (!Initialize(&initErr))
            {
                if (outError) *outError = "SpatialAudio not initialized: " + initErr;
                return kInvalidHandle;
            }
        }

        ParsedWav wav;
        std::string parseErr;
        if (!ParseWavFile(wavFilePath, wav, &parseErr))
        {
            if (outError) *outError = parseErr;
            LOG_ERROR("[SpatialAudio] Play2D parse failed: " << parseErr << " file='" << wavFilePath << "'");
            return kInvalidHandle;
        }

        auto* inst = new SoundInstance();
        inst->handle = gNextHandle.fetch_add(1, std::memory_order_relaxed);
        inst->wav = std::move(wav);
        inst->playbackOffset = 0;
        inst->masterVolume = (std::max)(0.0f, (std::min)(4.0f, volume));
        inst->loop = loop;
        inst->descriptor = wavFilePath;
        inst->tag = wavFilePath;

        // 2D: centered, full volume, no LPF
        inst->spatial.is3D    = false;
        inst->spatial.gainL   = inst->masterVolume * 0.707f; // equal power center
        inst->spatial.gainR   = inst->masterVolume * 0.707f;
        inst->spatial.lpfAlpha = 1.0f;
        inst->spatial.distance = 0.0f;

        {
            std::lock_guard<std::mutex> lock(gMutex);
            gInstances.push_back(inst);
        }

        LOG_INFO("[SpatialAudio] Play2D ok (handle=" << inst->handle
            << ", file='" << wavFilePath << "'"
            << ", ch=" << inst->wav.numChannels
            << ", sr=" << inst->wav.sampleRate
            << ", loop=" << (loop ? 1 : 0)
            << ", vol=" << volume << ")");

        return inst->handle;
    }

    // =====================================================================
    // Public API: Control
    // =====================================================================

    void SetSourcePosition(SoundHandle handle, const Vec3& pos)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto* inst : gInstances)
        {
            if (inst->handle == handle)
            {
                inst->sourcePos = pos;
                return;
            }
        }
    }

    void SetListenerState(const Vec3& pos, const Vec3& forward, const Vec3& right)
    {
        gListenerPos   = pos;
        gListenerFwd   = forward;
        gListenerRight = right;
    }

    void Stop(SoundHandle handle)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto* inst : gInstances)
        {
            if (inst->handle == handle)
            {
                inst->stopped.store(true, std::memory_order_relaxed);
                LOG_INFO("[SpatialAudio] Stop: handle=" << handle
                    << " desc='" << inst->descriptor << "'");
                return;
            }
        }
    }

    void StopAll()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        int count = 0;
        for (auto* inst : gInstances)
        {
            if (!inst->stopped.load(std::memory_order_relaxed))
            {
                inst->stopped.store(true, std::memory_order_relaxed);
                ++count;
            }
        }
        LOG_INFO("[SpatialAudio] StopAll: stopped " << count << " sounds");
    }

    bool IsPlaying(SoundHandle handle)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto* inst : gInstances)
        {
            if (inst->handle == handle)
                return !inst->stopped.load(std::memory_order_relaxed);
        }
        return false;
    }

    int StopByTag(const std::string& tag)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        int stopped = 0;
        for (auto* inst : gInstances)
        {
            if (inst->stopped.load(std::memory_order_relaxed))
                continue;
            // Match if tag is a prefix or substring of descriptor
            if (inst->tag.find(tag) != std::string::npos
                || inst->descriptor.find(tag) != std::string::npos)
            {
                inst->stopped.store(true, std::memory_order_relaxed);
                ++stopped;
            }
        }
        if (stopped > 0)
            LOG_INFO("[SpatialAudio] StopByTag('" << tag << "'): stopped " << stopped);
        return stopped;
    }

    // =====================================================================
    // Public API: Prediction placeholders
    // =====================================================================

    void SetPredictedSourcePosition(SoundHandle handle, const Vec3& predicted)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (auto* inst : gInstances)
        {
            if (inst->handle == handle)
            {
                inst->hasPredictedPos = true;
                inst->predictedPos = predicted;
                return;
            }
        }
    }

    void SetPredictedListenerState(
        const Vec3& pos,
        const Vec3& forward,
        const Vec3& right)
    {
        gListenerPredicted      = true;
        gPredictedListenerPos   = pos;
        gPredictedListenerFwd   = forward;
        gPredictedListenerRight = right;
    }

    // =====================================================================
    // Public API: Diagnostics
    // =====================================================================

    std::string DescribeActive(size_t maxEntries)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        std::ostringstream oss;
        oss << "SpatialAudio active: " << gInstances.size();

        size_t shown = 0;
        for (const auto* inst : gInstances)
        {
            if (shown >= maxEntries)
            {
                oss << "\n  ...";
                break;
            }
            ++shown;

            const bool playing = !inst->stopped.load(std::memory_order_relaxed);
            oss << "\n  [" << inst->handle << "] "
                << (inst->spatial.is3D ? "3D" : "2D")
                << " " << (playing ? "playing" : "stopped")
                << " dist=" << inst->spatial.distance
                << " gainL=" << inst->spatial.gainL
                << " gainR=" << inst->spatial.gainR
                << " lpf=" << inst->spatial.lpfAlpha
                << " loop=" << (inst->loop ? 1 : 0)
                << " :: " << inst->descriptor;
        }

        return oss.str();
    }

    int ActiveCount()
    {
        std::lock_guard<std::mutex> lock(gMutex);
        int count = 0;
        for (const auto* inst : gInstances)
        {
            if (!inst->stopped.load(std::memory_order_relaxed))
                ++count;
        }
        return count;
    }

} // namespace Mod::SpatialAudio
