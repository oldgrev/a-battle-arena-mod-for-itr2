#pragma once

#include <string>
#include <cstdint>

#include "..\CppSDK\SDK.hpp"

namespace Mod::ModFeedback
{
    // Minimal VR-friendly feedback using UKismetSystemLibrary::PrintString.
    void ShowMessage(const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 1.0f, 1.0f});

    // Diagnostics: start/stop a staggered subtitle-format test sequence.
    // Triggered via TCP to explore length/linebreak/markup limits in one VR run.
    void StartSubtitleTestSequence(uint32_t intervalMs = 900);
    void StopSubtitleTestSequence();

    // Replays any queued messages that were suppressed during map load / main menu.
    // Called from ModMain::OnTick when the world is ready.
    void DrainPending();

    // World-space helper (optional; useful for debugging spawn points, arena zones, etc.).
    void ShowWorldText(const SDK::FVector& location, const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 0.0f, 1.0f});

    // Play a UI/2D sound for the local player.
    void PlaySound2D(SDK::USoundBase* sound, float volume = 1.0f, float pitch = 1.0f, bool isUiSound = true);

    // Play a 3D sound at a world location.
    void PlaySoundAtLocation(SDK::USoundBase* sound, const SDK::FVector& location, float volume = 1.0f, float pitch = 1.0f);

    // Load a sound asset by Unreal soft object path and play it in 2D.
    bool PlaySoundAsset2D(const std::string& softObjectPath, float volume = 1.0f, float pitch = 1.0f, bool isUiSound = true, std::string* outError = nullptr);

    // Load a sound asset by Unreal soft object path and play it at the local player location (3D).
    bool PlaySoundAsset3DAtPlayer(const std::string& softObjectPath, float volume = 1.0f, float pitch = 1.0f, std::string* outError = nullptr);

    // Play a local media file (e.g. mp3) through Unreal's media framework audio path.
    bool PlayMediaFile2D(const std::string& filePath, bool loop, float volume, std::string* outError = nullptr);

    // Play a streaming media URL through Unreal's media framework audio path.
    bool PlayMediaUrl2D(const std::string& url, bool loop, float volume, std::string* outError = nullptr);

    // Play a local media file attached to a moving actor (true 3D follow audio).
    bool PlayMediaFileAttached3D(const std::string& actorSelector, const std::string& filePath, bool loop, float volume, std::string* outResolvedActor = nullptr, std::string* outError = nullptr);

    // Play a streaming media URL attached to a moving actor (true 3D follow audio).
    bool PlayMediaUrlAttached3D(const std::string& actorSelector, const std::string& url, bool loop, float volume, std::string* outResolvedActor = nullptr, std::string* outError = nullptr);

    // Stop and clear all media players started by this mod.
    int StopAllMedia();

    // Return compact state for active media instances.
    std::string DescribeActiveMedia(std::size_t maxEntries = 16);

    // Scan a folder for sound files and build playback groups from filename prefixes.
    // Also parses .txt files in the folder: each line is a file path or URL, inheriting
    // the group name from the .txt filename. Groups are cumulative across files and .txts.
    bool ScanSoundGroupsFromFolder(const std::string& folderPath, std::string* outError = nullptr);

    // Return a compact summary of loaded sound groups.
    std::string DescribeSoundGroups(std::size_t maxGroups = 20, std::size_t maxEntriesPerGroup = 5);

    // Pick a random sound from a group and play it through the media pipeline (2D).
    // Discerns file vs URL automatically. Retries up to kSoundGroupRetryCount times on failure.
    bool PlayRandomSoundGroup2D(const std::string& groupName, bool loop, float volume, std::string* outChosenFile = nullptr, std::string* outError = nullptr);

    // Returns true if any tracked media instance is currently playing for the given actor.
    bool IsMediaPlayingForActor(SDK::AActor* actor);

    // Stops and destroys any tracked media instances for the given actor. Returns count stopped.
    int StopMediaForActor(SDK::AActor* actor);

    // Play a local media file attached to a specific actor (takes actor pointer directly, no string selector).
    bool PlayMediaFileAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError = nullptr);

    // Play a streaming URL attached to a specific actor (takes actor pointer directly).
    bool PlayMediaUrlAttachedToActor(SDK::AActor* actor, const std::string& url, bool loop, float volume, std::string* outError = nullptr);

    // Play a 16-bit PCM .wav file as true 3D spatial audio attached to a moving actor.
    // Uses SpawnSoundAttached (not UMediaPlayer) so attenuation and 3D positioning work correctly.
    bool PlayWavAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError = nullptr);

    // Play an existing game sound asset (e.g. MetaSoundSource) attached to a moving actor.
    // Used to verify SpawnSoundAttached 3D spatialization works with proper sound assets.
    bool PlaySoundAssetAttachedToActor(SDK::AActor* actor, const std::string& softObjectPath, float volume = 1.0f, std::string* outError = nullptr);

    // Diagnostic A/B variant: create a runtime WAV soundwave using a known-good template USoundWave's
    // playback configuration (loading behavior/group/volume/pitch), then attach and play at local player.
    bool PlayWavAttachedToPlayerWithTemplate(const std::string& templateSoftObjectPath, const std::string& filePath, bool loop, float volume, std::string* outError = nullptr);

    // Discover currently loaded USoundWave assets from GObjects and return a compact summary.
    std::string DescribeLoadedSoundWaves(std::size_t maxEntries = 20, const std::string& containsFilter = std::string(), bool includeDefaultObjects = false);

    // No-soft-path diagnostic variant: auto-select a loaded USoundWave template from GObjects,
    // apply its playback config to a runtime WAV soundwave, and play attached to local player.
    bool PlayWavAttachedToPlayerAutoTemplate(const std::string& filePath, bool loop, float volume, const std::string& containsFilter = std::string(), std::string* outTemplateName = nullptr, std::string* outError = nullptr);

    // Pick a random sound from a group and play it as 3D audio attached to the given actor.
    // Discerns file vs URL. Retries on failure. Outputs the chosen entry path/url.
    bool PlayRandomSoundGroupAttachedToActor(SDK::AActor* actor, const std::string& groupName, bool loop, float volume, std::string* outChosenEntry = nullptr, std::string* outError = nullptr);

    // Probe and initialise the default sounds folder (sounds/ relative to CWD).
    // Logs whether it exists and scans it. Safe to call at mod startup.
    void InitSoundSystem();
}
