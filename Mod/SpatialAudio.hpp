#pragma once
/*
    SpatialAudio – Windows waveOut-based 3D positional audio engine.

    Bypasses Unreal Engine audio entirely. Parses .wav files, applies real-time
    3D processing (stereo pan, distance attenuation, low-pass filter), and
    outputs through the Windows waveOut API.

    Positions are sampled at ~10 fps for efficiency. Placeholder hooks for
    movement-prediction are included for future latency compensation.
*/

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

namespace Mod::SpatialAudio
{
    // A 3D position vector (matches UE convention: cm, left-handed Z-up).
    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    // Opaque handle to a playing sound instance.
    using SoundHandle = uint32_t;
    inline constexpr SoundHandle kInvalidHandle = 0;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    // Initialise the waveOut device. Call once at mod start.
    // Returns false if the audio device could not be opened.
    bool Initialize(std::string* outError = nullptr);

    // Shut down and release the waveOut device. Stops all playing sounds.
    void Shutdown();

    // Must be called periodically (~every game tick) to update spatial
    // positions and clean up finished sounds.  Internally throttled to
    // kPositionUpdateHz.
    void Tick();

    // ------------------------------------------------------------------
    // Playback
    // ------------------------------------------------------------------

    // Play a .wav file with 3D positioning relative to a listener.
    //
    //  sourcePos   – initial world position of the sound source.
    //  listenerPos – initial world position of the listener (player head).
    //  listenerFwd – initial listener forward direction (unit vector).
    //  listenerRight – initial listener right direction (unit vector).
    //  volume      – master volume multiplier [0..4].
    //  loop        – if true, restarts when finished.
    //
    // Returns a handle that can be used to stop or query the sound.
    SoundHandle Play3D(
        const std::string& wavFilePath,
        const Vec3& sourcePos,
        const Vec3& listenerPos,
        const Vec3& listenerFwd,
        const Vec3& listenerRight,
        float volume = 1.0f,
        bool loop = false,
        std::string* outError = nullptr);

    // Play a .wav file with no 3D processing (centered stereo, full volume).
    SoundHandle Play2D(
        const std::string& wavFilePath,
        float volume = 1.0f,
        bool loop = false,
        std::string* outError = nullptr);

    // ------------------------------------------------------------------
    // Control
    // ------------------------------------------------------------------

    // Update the source position for a playing 3D sound.
    void SetSourcePosition(SoundHandle handle, const Vec3& pos);

    // Update the listener (player) state.  Affects ALL playing 3D sounds.
    void SetListenerState(
        const Vec3& pos,
        const Vec3& forward,
        const Vec3& right);

    // Stop a specific sound.
    void Stop(SoundHandle handle);

    // Stop all currently playing sounds.
    void StopAll();

    // Returns true if the given handle is still actively playing.
    bool IsPlaying(SoundHandle handle);

    // Stop and remove all sounds whose source matches the given tag.
    // The tag is the wavFilePath prefix or actor-descriptor passed at Play3D time.
    int StopByTag(const std::string& tag);

    // ------------------------------------------------------------------
    // Prediction placeholders (for future latency compensation)
    // ------------------------------------------------------------------

    // Override the predicted source position for a sound.  If set, this
    // position is used instead of the last SetSourcePosition value during
    // the next DSP pass.  Cleared after each Tick.
    void SetPredictedSourcePosition(SoundHandle handle, const Vec3& predicted);

    // Override the predicted listener state.  Cleared after each Tick.
    void SetPredictedListenerState(
        const Vec3& pos,
        const Vec3& forward,
        const Vec3& right);

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------

    // Return a compact multi-line summary of active sounds.
    std::string DescribeActive(size_t maxEntries = 16);

    // Return the number of currently active sound instances.
    int ActiveCount();

    // ------------------------------------------------------------------
    // Tuning – all tunables live in ModTuning.hpp.  These are
    // convenience aliases so SpatialAudio.cpp can reference them
    // without a Mod::Tuning:: prefix on every line.
    // ------------------------------------------------------------------

} // namespace Mod::SpatialAudio
