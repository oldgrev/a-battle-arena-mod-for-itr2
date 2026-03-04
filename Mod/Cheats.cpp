

/*
AILEARNINGS
Added explicit 'Set*' methods for cheat toggles (godmode, unlimited ammo, hunger, fatigue). These
were required by CommandHandler and arena startup code. The setters simply call the existing toggles
when the desired state differs. This change fixed multiple C2039 compile errors when invoking
SetUnlimitedAmmo/SetGodMode/etc.
- Anomaly disable cheat (2026-02-27): Uses GetAllActorsOfClass for AAnomalyBase and
  ABP_AnomalySpawner_C. Destroys anomaly actors and disables ticking on spawner actors.
  ApplyAnomalySuppression is called every tick to catch newly-spawned anomalies while active.
- Heal item spawn (2026-02-27): Uses UFLSpawn::SpawnItemByTypeTag with tag
  "Item.Consumable.Injector.QuickHeal" to spawn a healing injector near the player.
  Uses the same FGameplayTag creation pattern as LoadoutSubsystem (StringToName).
*/

#include "Cheats.hpp"
#include "HookManager.hpp"

#include "ModFeedback.hpp"

#include <sstream>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <array>

#include "Logging.hpp"
#include "ArenaSubsystem.hpp"
#include "ModTuning.hpp"
#include "GameContext.hpp"  // for world access when interacting with subsystems

// forward declare cheat subsystem type for convenience
using CheatSubsystem_t = SDK::URadiusCheatSubsystem;

namespace Mod
{
    namespace
    {
        // helper to retrieve the cheat subsystem from the current world (may return nullptr)
        CheatSubsystem_t* GetCheatSubsystem()
        {
            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
                return nullptr;
            SDK::UGameInstance* gi = world->OwningGameInstance;
            if (!gi)
                return nullptr;
            // Manual search for URadiusCheatSubsystem in subsystems
            // Note: GetGameInstanceSubsystem is a template function not available in SDK
            // We need to find the subsystem ourselves
            return nullptr; // Cheat subsystem access needs different approach
        }

        // Helper to access TArray members when the SDK headers are being difficult.
        template <typename ElementType>
        struct TArrayRaw { ElementType *Data; int32_t NumElements; int32_t MaxElements; };

        template <typename ElementType>
        static int32_t GetTArrayNum(const SDK::TArray<ElementType>& arr) {
            return reinterpret_cast<const TArrayRaw<ElementType>*>(&arr)->NumElements;
        }

        template <typename ElementType>
        static ElementType* GetTArrayData(const SDK::TArray<ElementType>& arr) {
            return reinterpret_cast<const TArrayRaw<ElementType>*>(&arr)->Data;
        }

        static std::string ToLowerCopy(const std::string& value)
        {
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        static bool ContainsAnyKeyword(const std::string& haystackLower, const std::array<const char*, 12>& keywords)
        {
            for (const char* keyword : keywords)
            {
                if (keyword && haystackLower.find(keyword) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        static constexpr std::array<const char*, 12> kPlantsBushesKeywords = {
            "plant", "bush", "shrub", "foliage", "fern", "grass",
            "weed", "ivy", "thicket", "reed", "branch", "leaf"
        };

        static constexpr std::array<const char*, 12> kTreesKeywords = {
            "tree", "trunk", "pine", "oak", "birch", "spruce",
            "willow", "stump", "conifer", "cedar", "deadtree", "treeline"
        };
    }

    Cheats::Cheats()
    {
        plantsBushesRule_.category = EnvironmentPruneCategory::PlantsBushes;
        plantsBushesRule_.source = EnvironmentPruneSource::All;

        treesRule_.category = EnvironmentPruneCategory::Trees;
        treesRule_.source = EnvironmentPruneSource::All;

        foliageRule_.category = EnvironmentPruneCategory::All;
        foliageRule_.source = EnvironmentPruneSource::Foliage;

        grassRule_.category = EnvironmentPruneCategory::All;
        grassRule_.source = EnvironmentPruneSource::Grass;

        instancedStaticMeshRule_.category = EnvironmentPruneCategory::All;
        instancedStaticMeshRule_.source = EnvironmentPruneSource::InstancedStaticMesh;
    }

    void Cheats::ToggleGodMode()
    {
        godModeActive_ = !godModeActive_;
        // ShowMessage removed - mod menu shows status
    }

    // ------------------------------------------------------------------------
    // Explicit setters
    // ------------------------------------------------------------------------
    void Cheats::SetGodMode(bool enabled)
    {
        if (godModeActive_.load() != enabled)
        {
            ToggleGodMode();
        }
    }

    void Cheats::SetUnlimitedAmmo(bool enabled)
    {
        if (unlimitedAmmoActive_.load() != enabled)
        {
            ToggleUnlimitedAmmo();
        }
    }

    void Cheats::SetHungerDisabled(bool enabled)
    {
        if (hungerDisabled_.load() != enabled)
        {
            ToggleHungerDisabled();
        }
    }

    void Cheats::SetFatigueDisabled(bool enabled)
    {
        if (fatigueDisabled_.load() != enabled)
        {
            ToggleFatigueDisabled();
        }
    }

    bool Cheats::IsGodModeActive() const
    {
        return godModeActive_;
    }

    void Cheats::ToggleUnlimitedAmmo()
    {
        unlimitedAmmoActive_ = !unlimitedAmmoActive_;
        HookManager::Get().SetUnlimitedAmmoEnabled(unlimitedAmmoActive_);

        // Also toggle durability bypass when enabling unlimited ammo
        if (unlimitedAmmoActive_ && !durabilityBypassActive_)
        {
            ToggleDurabilityBypass();
        }
        // ShowMessage removed - mod menu shows status
    }

    void Cheats::ToggleDurabilityBypass()
    {
        durabilityBypassActive_ = !durabilityBypassActive_;
        HookManager::Get().SetDurabilityBypassEnabled(durabilityBypassActive_);
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsUnlimitedAmmoActive() const
    {
        return unlimitedAmmoActive_;
    }

    bool Cheats::IsDurabilityBypassActive() const
    {
        return durabilityBypassActive_;
    }

    void Cheats::ToggleHungerDisabled()
    {
        hungerDisabled_ = !hungerDisabled_;
        HookManager::Get().SetHungerDisabled(hungerDisabled_);
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsHungerDisabledActive() const
    {
        return hungerDisabled_;
    }

    void Cheats::ToggleFatigueDisabled()
    {
        fatigueDisabled_ = !fatigueDisabled_;
        HookManager::Get().SetFatigueDisabled(fatigueDisabled_);
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsFatigueDisabledActive() const
    {
        return fatigueDisabled_;
    }

    void Cheats::ToggleBulletTime()
    {
        bulletTimeActive_ = !bulletTimeActive_;
        bulletTimeDirty_.store(true, std::memory_order_release);
        // ShowMessage removed - mod menu shows status
    }

    void Cheats::SetBulletTimeScale(float scale)
    {
        if (scale <= 0.0f) scale = Mod::Tuning::kBulletTimeMinScale;
        if (scale > Mod::Tuning::kBulletTimeMaxScale) scale = Mod::Tuning::kBulletTimeMaxScale;

        const float current = bulletTimeScale_.load(std::memory_order_relaxed);
        if (std::fabs(current - scale) > 0.0001f)
        {
            bulletTimeScale_.store(scale, std::memory_order_relaxed);
            bulletTimeDirty_.store(true, std::memory_order_release);
        }

        LOG_INFO("[Cheats] Bullet Time scale set to " << scale);
    }

    // ------------------------------------------------------------------------
    // Additional cheats implemented based on Lua script
    // ------------------------------------------------------------------------

    void Cheats::ToggleNoClip()
    {
        CheatSubsystem_t* cheatsub = GetCheatSubsystem();
        if (!cheatsub)
        {
            LOG_WARN("[Cheats] ToggleNoClip: cheat subsystem not available");
            return;
        }
        bool oldVal = cheatsub->GetNoClip();
        cheatsub->SetNoClip(!oldVal);
        noClipActive_ = !oldVal;
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsNoClipActive() const
    {
        return noClipActive_;
    }


    void Cheats::AddMoney(int amount)
    {
        CheatSubsystem_t* cheatsub = GetCheatSubsystem();
        if (cheatsub)
        {
            cheatsub->AddMoney(amount);
            LOG_INFO("[Cheats] Added money: " << amount);
            Mod::ModFeedback::ShowMessage(L"[Mod] Money added", 2.0f);
        }
        else
        {
            // fallback: find replicator actor in world similar to Lua script
            SDK::ARadiusGameDataReplicator* replicator = nullptr;
            if (SDK::UWorld* world = GameContext::GetWorld())
            {
                SDK::TArray<SDK::AActor*> actors;
                SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusGameDataReplicator::StaticClass(), &actors);
                const int32_t count = GetTArrayNum(actors);
                SDK::AActor** data = GetTArrayData(actors);
                if (count > 0 && data && data[0] && data[0]->IsA(SDK::ARadiusGameDataReplicator::StaticClass()))
                    replicator = static_cast<SDK::ARadiusGameDataReplicator*>(data[0]);
            }
            if (replicator)
            {
                replicator->Money += amount;
                LOG_INFO("[Cheats] (replicator) Added money: " << amount);
                Mod::ModFeedback::ShowMessage(L"[Mod] Money added", 2.0f);
            }
            else
            {
                LOG_WARN("[Cheats] AddMoney: cheat subsystem unavailable and no replicator found");
            }
        }
    }

    void Cheats::SetAccessLevel(int level)
    {
        CheatSubsystem_t* cheatsub = GetCheatSubsystem();
        if (cheatsub)
        {
            cheatsub->SetAccessLevel(level);
            LOG_INFO("[Cheats] Set access level to " << level);
            Mod::ModFeedback::ShowMessage(L"[Mod] Access level changed", 2.0f);
        }
        else
        {
            SDK::ARadiusGameDataReplicator* replicator = nullptr;
            if (SDK::UWorld* world = GameContext::GetWorld())
            {
                SDK::TArray<SDK::AActor*> actors;
                SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusGameDataReplicator::StaticClass(), &actors);
                const int32_t count = GetTArrayNum(actors);
                SDK::AActor** data = GetTArrayData(actors);
                if (count > 0 && data && data[0] && data[0]->IsA(SDK::ARadiusGameDataReplicator::StaticClass()))
                    replicator = static_cast<SDK::ARadiusGameDataReplicator*>(data[0]);
            }
            if (replicator)
            {
                replicator->SetAccessLevel(level);
                LOG_INFO("[Cheats] (replicator) Set access level to " << level);
                Mod::ModFeedback::ShowMessage(L"[Mod] Access level changed", 2.0f);
            }
            else
            {
                LOG_WARN("[Cheats] SetAccessLevel: cheat subsystem unavailable and no replicator found");
            }
        }
    }

    void Cheats::ToggleDebugMode()
    {
        debugModeActive_ = !debugModeActive_;
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsDebugModeActive() const
    {
        return debugModeActive_;
    }

    void Cheats::SetPortableLightIntensityScale(float scale)
    {
        if (!std::isfinite(scale))
            scale = 1.0f;

        // Keep sane bounds; values outside this range tend to blow out HDR or go fully dark.
        if (scale < 0.05f) scale = 0.05f;
        if (scale > 20.0f) scale = 20.0f;

        const float current = portableLightIntensityScale_.load(std::memory_order_relaxed);
        if (std::fabs(current - scale) > 0.0001f)
        {
            portableLightIntensityScale_.store(scale, std::memory_order_relaxed);
            portableLightScaleDirty_.store(true, std::memory_order_release);
        }

        std::wstringstream msg;
        msg << L"[Mod] Light brightness scale: " << scale;
        Mod::ModFeedback::ShowMessage(msg.str().c_str(), 3.0f, SDK::FLinearColor{0.9f, 0.9f, 0.2f, 1.0f});

        LOG_INFO("[Cheats] Portable light intensity scale set to " << scale);
    }

    float Cheats::GetPortableLightIntensityScale() const
    {
        return portableLightIntensityScale_.load(std::memory_order_relaxed);
    }

    void Cheats::SetPortableLightFadeDistanceScale(float scale)
    {
        if (!std::isfinite(scale))
            scale = 1.0f;

        if (scale < 0.05f) scale = 0.05f;
        if (scale > 20.0f) scale = 20.0f;

        const float current = portableLightFadeDistanceScale_.load(std::memory_order_relaxed);
        if (std::fabs(current - scale) > 0.0001f)
        {
            portableLightFadeDistanceScale_.store(scale, std::memory_order_relaxed);
            portableLightScaleDirty_.store(true, std::memory_order_release);
        }

        std::wstringstream msg;
        msg << L"[Mod] Light fade distance scale: " << scale;
        Mod::ModFeedback::ShowMessage(msg.str().c_str(), 3.0f, SDK::FLinearColor{0.9f, 0.9f, 0.2f, 1.0f});

        LOG_INFO("[Cheats] Portable light fade distance scale set to " << scale);
    }

    float Cheats::GetPortableLightFadeDistanceScale() const
    {
        return portableLightFadeDistanceScale_.load(std::memory_order_relaxed);
    }

    std::string Cheats::GetStatus() const
    {
        std::ostringstream status;
        status << "Cheats Status:\n";
        status << "  God Mode: " << (godModeActive_ ? "ACTIVE" : "inactive") << "\n";
        status << "  Unlimited Ammo: " << (unlimitedAmmoActive_ ? "ACTIVE" : "inactive") << "\n";
        status << "  Durability Bypass: " << (durabilityBypassActive_ ? "ACTIVE" : "inactive") << "\n";
        status << "  Hunger Disabled: " << (hungerDisabled_ ? "ACTIVE" : "inactive") << "\n";
        status << "  Fatigue Disabled: " << (fatigueDisabled_ ? "ACTIVE" : "inactive") << "\n";
        status << "  Bullet Time: " << (bulletTimeActive_ ? "ACTIVE" : "inactive") << " (Scale: " << bulletTimeScale_ << ")\n";
        status << "  NoClip: " << (noClipActive_ ? "ON" : "off") << "\n";
        status << "  Anomalies Disabled: " << (anomaliesDisabled_ ? "ACTIVE" : "inactive") << "\n";
        status << "  AutoMag: " << (autoMagActive_ ? "ACTIVE" : "inactive") << "\n";
         status << "  Remove Plants/Bushes (persistent): " << (plantsBushesRule_.enabled ? "ON" : "off")
             << " [radius=" << plantsBushesRule_.radius << ", interval=" << plantsBushesRule_.intervalSeconds << "s]\n";
         status << "  Remove Trees (persistent): " << (treesRule_.enabled ? "ON" : "off")
             << " [radius=" << treesRule_.radius << ", interval=" << treesRule_.intervalSeconds << "s]\n";
         status << "  Remove Foliage (persistent): " << (foliageRule_.enabled ? "ON" : "off")
             << " [radius=" << foliageRule_.radius << ", interval=" << foliageRule_.intervalSeconds << "s]\n";
         status << "  Remove Grass (persistent): " << (grassRule_.enabled ? "ON" : "off")
             << " [radius=" << grassRule_.radius << ", interval=" << grassRule_.intervalSeconds << "s]\n";
         status << "  Remove ISM (persistent): " << (instancedStaticMeshRule_.enabled ? "ON" : "off")
             << " [radius=" << instancedStaticMeshRule_.radius << ", interval=" << instancedStaticMeshRule_.intervalSeconds << "s]\n";
        status << "  Debug Mode: " << (debugModeActive_ ? "ON" : "off");
        return status.str();
    }


    void Cheats::DeactivateAll()
    {
        if (godModeActive_) ToggleGodMode();
        if (unlimitedAmmoActive_) ToggleUnlimitedAmmo();
        if (durabilityBypassActive_) ToggleDurabilityBypass();
        if (hungerDisabled_) ToggleHungerDisabled();
        if (fatigueDisabled_) ToggleFatigueDisabled();
        if (bulletTimeActive_) ToggleBulletTime();
        if (noClipActive_) ToggleNoClip();
        if (debugModeActive_) ToggleDebugMode();
        if (autoMagActive_) ToggleAutoMag();
        plantsBushesRule_.enabled = false;
        treesRule_.enabled = false;
        foliageRule_.enabled = false;
        grassRule_.enabled = false;
        instancedStaticMeshRule_.enabled = false;
        pendingEnvironmentPruneBatches_.clear();
        // Note: anomaliesDisabled_ is NOT toggled on level change -- it persists intentionally.

        // Clear any cached light pointers/intensities on level change.
        //portableLightOriginalIntensity_.clear();
        //portableLightOriginalFadeDistance_.clear();
        //portableLightScaleDirty_.store(true, std::memory_order_release);
        //lastPortableLightWorld_ = nullptr;
        lastBulletTimeWorld_ = nullptr;
        lastBulletTimePlayer_ = nullptr;
        bulletTimeDirty_.store(true, std::memory_order_release);
        
        LOG_INFO("[Cheats] All cheats deactivated");
    }

    void Cheats::ApplyPortableLightBrightness(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player)
    {
        (void)player;

        if (!world)
            return;

        const float scale = portableLightIntensityScale_.load(std::memory_order_relaxed);
        const float fadeScale = portableLightFadeDistanceScale_.load(std::memory_order_relaxed);
        if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(fadeScale) || fadeScale <= 0.0f)
            return;

        // Default scale: do nothing unless we've previously scaled lights (or need to resync after a level change).
        const bool isDefault = std::fabs(scale - 1.0f) < 0.0001f;
        const bool fadeIsDefault = std::fabs(fadeScale - 1.0f) < 0.0001f;
        const bool dirty = portableLightScaleDirty_.load(std::memory_order_acquire);
        if (isDefault && fadeIsDefault && !dirty)
            return;

        portableLightScaleDirty_.store(false, std::memory_order_release);

        if (!SDK::UObject::GObjects)
            return;

        const int32_t total = SDK::UObject::GObjects->Num();
        if (total <= 0)
            return;

        int touched = 0;

        auto applyTo = [&](SDK::ULocalLightComponent* light)
        {
            if (!light)
                return;

            auto* base = static_cast<SDK::ULightComponentBase*>(light);
            auto* lightComp = static_cast<SDK::ULightComponent*>(light);
            auto it = portableLightOriginalIntensity_.find(base);
            if (it == portableLightOriginalIntensity_.end())
            {
                it = portableLightOriginalIntensity_.emplace(base, base->Intensity).first;
            }

            auto fadeIt = portableLightOriginalFadeDistance_.find(base);
            if (fadeIt == portableLightOriginalFadeDistance_.end())
            {
                fadeIt = portableLightOriginalFadeDistance_.emplace(base, lightComp->LightFunctionFadeDistance).first;
            }

            const float target = it->second * scale;
            const float targetFadeDistance = fadeIt->second * fadeScale;
            bool changed = false;
            if (std::fabs(base->Intensity - target) > 0.01f)
            {
                light->SetIntensity(target);
                changed = true;
            }

            if (std::fabs(lightComp->LightFunctionFadeDistance - targetFadeDistance) > 0.01f)
            {
                lightComp->SetLightFunctionFadeDistance(targetFadeDistance);
                changed = true;
            }

            if (changed)
            {
                touched++;
            }
        };

        for (int32_t i = 0; i < total; ++i)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
            if (!obj)
                continue;

            if (!obj->IsA(SDK::UBPC_LightComp_C::StaticClass()))
                continue;

            auto* lightComp = static_cast<SDK::UBPC_LightComp_C*>(obj);
            if (!lightComp || lightComp->IsDefaultObject())
                continue;

            // Avoid messing with laser components; users typically only want flashlight/headlamp brightness.
            if (lightComp->IsLaser)
                continue;

            applyTo(lightComp->SpotLight);
            applyTo(lightComp->PointLight);
        }

        if (debugModeActive_.load() && touched > 0)
        {
            // Rate-limit in case the scan finds many objects.
            if (portableLightLogCounter_++ < 10)
            {
                LOG_INFO("[Cheats] Applied portable light brightness scale=" << scale << " (touched=" << touched << ")");
            }
        }
    }

    // do we really need to do all of this every single frame?
    // sloppy work....
    void Cheats::Update(SDK::UWorld *world)
    {
        if (!world)
            return;

        auto *player = FindPlayer(world);

        if (world != lastBulletTimeWorld_ || player != lastBulletTimePlayer_)
        {
            lastBulletTimeWorld_ = world;
            lastBulletTimePlayer_ = player;
            bulletTimeDirty_.store(true, std::memory_order_release);
        }

        if (world != lastPortableLightWorld_)
        {
            lastPortableLightWorld_ = world;
            portableLightOriginalIntensity_.clear();
            portableLightOriginalFadeDistance_.clear();
            portableLightScaleDirty_.store(true, std::memory_order_release);
        }

        if (player && godModeActive_.load())
        {
            ApplyGodMode(player);
        }

        if (player && (hungerDisabled_.load() || fatigueDisabled_.load()))
        {
            ApplyStatsCheats(player);
        }

        const bool bulletDirty = bulletTimeDirty_.exchange(false, std::memory_order_acq_rel);
        const float targetGlobalDilation = bulletTimeActive_.load() ? bulletTimeScale_.load() : 1.0f;
        if (bulletDirty)
        {
            SDK::UGameplayStatics::SetGlobalTimeDilation(world, targetGlobalDilation);
        }

        if (player)
        {
            // Player CustomTimeDilation should be 1/GlobalDilation to maintain normal speed
            float targetPlayerDilation = 1.0f / targetGlobalDilation;

            if (bulletDirty && player->CustomTimeDilation != targetPlayerDilation)
            {
                player->CustomTimeDilation = targetPlayerDilation;
            }

            // Apply to held items and potentially arms only when bullet-time settings/context changes.
            if (bulletDirty)
            {
                UpdateHeldItemsDilation(world, targetPlayerDilation, player);
            }

            // Periodic durability check
            static uint32_t lastCheck = 0;
            durabilityCheckCounter_++;
            if (durabilityCheckCounter_ >= 600)
            {
                durabilityCheckCounter_ = 0;
                if (durabilityBypassActive_.load())
                {
                    ApplyDurabilityFix(world, player);
                }
            }
        }

        // Flashlight/headlamp brightness scaling only when setting/context changes.
        if (portableLightScaleDirty_.load(std::memory_order_acquire))
        {
            ApplyPortableLightBrightness(world, player);
        }

        // Anomaly suppression: first run is immediate when enabled, then throttled to every 30 seconds.
        ApplyAnomalySuppression(world);

        // Environment pruning: delayed deletion queue and optional persistent collectors.
        UpdateEnvironmentPrune(world, player);
    }

    // body dilation doesn't even work, arms and the player weapon move so slow which is crap in vr. to do
    void Cheats::UpdateHeldItemsDilation(SDK::UWorld *world, float targetDilation, SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player)
    {
        if (!world || !player) return;

        // 1. Dilate held items (which might not be technically "attached" in the engine's hierarchy)
        SDK::TArray<SDK::AActor *> itemActors;
        SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusItemBase::StaticClass(), &itemActors);

        const int32_t itemActorsCount = GetTArrayNum(itemActors);
        SDK::AActor** itemActorsData = GetTArrayData(itemActors);
        for (int32_t i = 0; i < itemActorsCount; ++i)
        {
            SDK::AActor *actor = itemActorsData[i];
            if (!actor) continue;

            auto *item = static_cast<SDK::ARadiusItemBase *>(actor);

            bool isHeld = false;
            if (item->GripControllerPrimary && item->GripControllerPrimary->GetIsHeld(actor))
                isHeld = true;
            else if (item->GripControllerSecondary && item->GripControllerSecondary->GetIsHeld(actor))
                isHeld = true;

            if (isHeld)
            {
                if (actor->CustomTimeDilation != targetDilation)
                {
                    actor->CustomTimeDilation = targetDilation;
                }
            }
            else if (!bulletTimeActive_.load() && actor->CustomTimeDilation != 1.0f)
            {
                actor->CustomTimeDilation = 1.0f;
            }
        }

        // 2. Dilate anything attached to the player (like separate hand actors or body components)
        SDK::TArray<SDK::AActor *> attachedActors;
        player->GetAttachedActors(&attachedActors, true, true);
        const int32_t attachedActorsCount = GetTArrayNum(attachedActors);
        SDK::AActor** attachedActorsData = GetTArrayData(attachedActors);
        for (int32_t i = 0; i < attachedActorsCount; ++i)
        {
            SDK::AActor *actor = attachedActorsData[i];
            if (actor && actor->CustomTimeDilation != targetDilation)
            {
                actor->CustomTimeDilation = targetDilation;
            }
        }
    }

    void Cheats::ApplyDurabilityFix(SDK::UWorld *world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player)
    {
        // this currently only applies to the primary object of the weapon.
        // i.e.  dust covers etc don't have their durability fixed by this
        // later should modify it to do an initial recurse when an item is grabbed
        // but hook the actual durability change function to prevent durability loss, for speed.


        if (!world) return;

        SDK::TArray<SDK::AActor *> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusItemBase::StaticClass(), &actors);

        const int32_t actorsCount = GetTArrayNum(actors);
        SDK::AActor** actorsData = GetTArrayData(actors);
        for (int32_t i = 0; i < actorsCount; ++i)
        {
            SDK::AActor *actor = actorsData[i];
            if (!actor)
                continue;

            auto *item = static_cast<SDK::ARadiusItemBase *>(actor);

            bool isHeld = false;
            // Native check for held items
            if (item->GripControllerPrimary && item->GripControllerPrimary->GetIsHeld(actor))
                isHeld = true;
            else if (item->GripControllerSecondary && item->GripControllerSecondary->GetIsHeld(actor))
                isHeld = true;

            if (isHeld)
            {
                // Access data component and dynamic data to find the actual max durability
                if (item->DataComponent)
                {
                    // ItemDynamicDataPtr is a TWeakObjectPtr. Use Get() to check validity.
                    if (item->DataComponent->ItemDynamicDataPtr.Get() != nullptr)
                    {
                        auto* dynamicData = item->DataComponent->ItemDynamicDataPtr.Get();
                        SDK::FRadiusItemStaticData staticData;
                        
                        // Get static data which contains the max durability
                        if (dynamicData->GetItemBasicStaticData(&staticData))
                        {
                            float maxDurability = staticData.ItemDurability;
                            float currentDurability = dynamicData->Durability;

                            // If it's a delta, we add enough to reach max. 
                            // If it's a setter (less likely via Change), we'd just pass max.
                            // In ITR2 delta is typically used.

                            // the above were the ai comments, I don't know about that. 
                            // i'd just set the max durability every time instead of doing the extra maths.
                            // cpu cycles don't grow on trees. I blame the kids these days.
                            if (currentDurability < maxDurability)
                            {
                                float delta = maxDurability - currentDurability;
                                item->Server_ChangeItemDurability(delta);
                                LOG_INFO("[Cheats] Restored " << item->GetName() << " durability by " << delta << " to max " << maxDurability);
                            }
                        }
                    }
                }
            }
        }
    }

    // this function makes me wonder how many other classes and functions need to find the player
    // it'd be better off having the main mod class expose some kind of GetPlayer() function which
    // caches and manages some sort of refresh logic. This is not fine.
    SDK::ABP_RadiusPlayerCharacter_Gameplay_C *Cheats::FindPlayer(SDK::UWorld *world)
    {
        // Use the SDK helper to get the player pawn
        SDK::APawn *pawn = SDK::UGameplayStatics::GetPlayerPawn(world, 0);
        if (!pawn)
        {
            cachedPlayer_ = nullptr;
            return nullptr;
        }

        // Safer check: verify the class is correct before casting
        if (pawn->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
        {
            cachedPlayer_ = static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C *>(pawn);
        }
        else
        {
            cachedPlayer_ = nullptr;
        }

        return cachedPlayer_;
    }

    void Cheats::ApplyGodMode(SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player)
    {
        // it's probably more efficient to find the ways I call this and then do a lot of this there
        // because all the other stuff probably needs the same checks
        if (!player)
            return;

        // ITR2 character class has a direct member for PlayerStats
        if (!player->PlayerStats)
        {
            // Fallback to GetComponentByClass if direct member is null for some reason
            auto *statsComponent = player->GetComponentByClass(SDK::UPlayerStatsComponent::StaticClass());
            if (!statsComponent)
            {
                LOG_ERROR("[Cheats] PlayerStatsComponent not found on player!");
                return;
            }

            if (!statsComponent->IsA(SDK::UPlayerStatsComponent::StaticClass()))
            {
                LOG_ERROR("[Cheats] PlayerStatsComponent lookup returned unexpected type");
                return;
            }

            player->PlayerStats = static_cast<SDK::UPlayerStatsComponent *>(statsComponent);
        }

        auto *playerStats = player->PlayerStats;

        // Get current health
        float currentHealth = playerStats->GetHealth();

        // Max health - use a reasonable value (100 is typical for this game)
        float maxHealth = 100.0f;

        // If health is below max, restore it by calling Server_ChangeHealth with positive delta
        // unless it's less than 0, which means you've taken big damage and may be stuck in a damage zone which
        // would result in a constant cycle of load game popups (ask me how I know)
        if (currentHealth < maxHealth && currentHealth > 0.0f)
        {
            // i could hook it so that any call to change health with a negative delta is ignored
            // but this way we see the red screen pulse indicating damaged followed by the green screen pulse indicating healing
            // and sufficiently damaging things (like going in red water), will still end the player.
            float healthDelta = maxHealth - currentHealth;
            playerStats->Server_ChangeHealth(healthDelta, nullptr, nullptr);
            LOG_INFO("[Cheats] Restored " << healthDelta << " health");

            // Record virtual damage taken for Arena stats
            auto* arena = Arena::ArenaSubsystem::Get();
            if (arena && arena->IsActive())
            {
                arena->RecordPlayerDamageTaken(healthDelta);
            }
        }
    }

    void Cheats::ApplyStatsCheats(SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player)
    {
        if (!player || !player->PlayerStats)
            return;

        auto *playerStats = player->PlayerStats;

        if (hungerDisabled_.load())
        {
            // the game's hunger value appears to be a resource that drains over time;
            // simply resetting it to a very high value each frame prevents any
            // accumulation. matching the Lua script we use 10k as a safe upper bound.
            playerStats->CurrentHunger = 65535.0f;
            // this doesn't work like i'd expect. I might just hook the hunger functions
            // to just pass through and do nothing, which would technically be an optimization!
            // alexa, put it on the to do list.
        }

        if (fatigueDisabled_.load())
        {
            // simple approach: jam the current stamina value up to the max so the
            // player never runs out. (previous delta based approach sometimes
            // allowed tiny losses over time.)
            float maxStamina = playerStats->GetMaxStaminaAffectedByHunger();
            playerStats->CurrentStamina = maxStamina;
            // another one for hooking
        }
    }

    // -----------------------------------------------------------------------
    // Anomaly disable/enable cheat
    // -----------------------------------------------------------------------

    void Cheats::ToggleAnomaliesDisabled()
    {
        anomaliesDisabled_ = !anomaliesDisabled_;
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsAnomaliesDisabledActive() const
    {
        return anomaliesDisabled_;
    }

    // -----------------------------------------------------------------------
    // Automag: magazines placed in mag pouches auto-refill
    // -----------------------------------------------------------------------

    void Cheats::ToggleAutoMag()
    {
        autoMagActive_ = !autoMagActive_;
        HookManager::Get().SetAutoMagEnabled(autoMagActive_);
        // ShowMessage removed - mod menu shows status
    }

    bool Cheats::IsAutoMagActive() const
    {
        return autoMagActive_;
    }

    void Cheats::SetAutoMag(bool enabled)
    {
        if (autoMagActive_ != enabled) ToggleAutoMag();
    }

    const char* Cheats::CategoryToLabel(EnvironmentPruneCategory category) const
    {
        switch (category)
        {
        case EnvironmentPruneCategory::All:
            return "all";
        case EnvironmentPruneCategory::PlantsBushes:
            return "plants_bushes";
        case EnvironmentPruneCategory::Trees:
            return "trees";
        default:
            return "unknown";
        }
    }

    const char* Cheats::SourceToLabel(EnvironmentPruneSource source) const
    {
        switch (source)
        {
        case EnvironmentPruneSource::All:
            return "all";
        case EnvironmentPruneSource::Foliage:
            return "foliage";
        case EnvironmentPruneSource::Grass:
            return "grass";
        case EnvironmentPruneSource::InstancedStaticMesh:
            return "staticmeshcomponent";
        default:
            return "unknown";
        }
    }

    bool Cheats::MatchesEnvironmentCategory(EnvironmentPruneCategory category, const std::string& name) const
    {
        if (category == EnvironmentPruneCategory::All)
            return true;

        if (name.empty())
            return false;

        const std::string lower = ToLowerCopy(name);
        if (category == EnvironmentPruneCategory::PlantsBushes)
        {
            return ContainsAnyKeyword(lower, kPlantsBushesKeywords);
        }
        return ContainsAnyKeyword(lower, kTreesKeywords);
    }

    bool Cheats::MatchesEnvironmentSource(EnvironmentPruneSource source, const SDK::UInstancedStaticMeshComponent* component) const
    {
        if (!component)
            return false;

        switch (source)
        {
        case EnvironmentPruneSource::All:
            return true;
        case EnvironmentPruneSource::Foliage:
            return component->IsA(SDK::UFoliageInstancedStaticMeshComponent::StaticClass());
        case EnvironmentPruneSource::Grass:
            return component->IsA(SDK::UGrassInstancedStaticMeshComponent::StaticClass());
        case EnvironmentPruneSource::InstancedStaticMesh:
            return component->IsA(SDK::UInstancedStaticMeshComponent::StaticClass());
        default:
            return false;
        }
    }

    bool Cheats::IsWithinRadius(const SDK::FVector& a, const SDK::FVector& b, float radius) const
    {
        if (radius <= 0.0f)
            return true;

        const float dx = a.X - b.X;
        const float dy = a.Y - b.Y;
        const float dz = a.Z - b.Z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        const float radiusSq = radius * radius;
        return distSq <= radiusSq;
    }

    std::string Cheats::ToggleEnvironmentPersistent(EnvironmentPruneRule& rule, float radius, float intervalSeconds)
    {
        if (!std::isfinite(radius) || radius < 0.0f)
            radius = kDefaultEnvironmentPruneRadius;
        if (!std::isfinite(intervalSeconds) || intervalSeconds < 0.25f)
            intervalSeconds = kDefaultEnvironmentPruneIntervalSeconds;

        rule.enabled = !rule.enabled;
        rule.radius = radius;
        rule.intervalSeconds = intervalSeconds;
        rule.nextCollectAt = std::chrono::steady_clock::time_point{};

        std::ostringstream out;
        out << CategoryToLabel(rule.category)
            << " source=" << SourceToLabel(rule.source)
            << " persistent=" << (rule.enabled ? "ON" : "OFF")
            << " radius=" << rule.radius
            << " intervalSec=" << rule.intervalSeconds;

        LOG_INFO("[Cheats] Environment prune " << out.str());
        return out.str();
    }

    std::string Cheats::QueueEnvironmentPruneNow(
        SDK::UWorld* world,
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player,
        EnvironmentPruneCategory category,
        EnvironmentPruneSource source,
        float radius,
        bool persistent)
    {
        if (!world)
            return "world not ready";

        if (!std::isfinite(radius) || radius < 0.0f)
            radius = kDefaultEnvironmentPruneRadius;

        if (radius > 0.0f && !player)
        {
            std::ostringstream oss;
            oss << "cannot collect " << CategoryToLabel(category) << " with radius " << radius << " (player not found)";
            LOG_WARN("[Cheats] " << oss.str());
            return oss.str();
        }

        PendingPruneBatch batch;
        batch.category = category;
        batch.source = source;
        batch.persistent = persistent;
        batch.radius = radius;
        batch.executeAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        CollectEnvironmentPruneTargets(world, player, category, source, radius, batch);

        std::ostringstream msg;
        msg << CategoryToLabel(category)
            << " source=" << SourceToLabel(source)
            << " queued targets=" << batch.instanceTargets.size()
            << " instanceComponents=" << batch.instanceTargets.size()
            << " radius=" << radius
            << " executeInSec=1";

        LOG_INFO("[Cheats] " << msg.str());
        pendingEnvironmentPruneBatches_.push_back(std::move(batch));
        return msg.str();
    }

    void Cheats::CollectEnvironmentPruneTargets(
        SDK::UWorld* world,
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player,
        EnvironmentPruneCategory category,
        EnvironmentPruneSource source,
        float radius,
        PendingPruneBatch& batch)
    {
        if (!world)
            return;

        Mod::ScopedProcessEventGuard guard;

        const bool useRadius = radius > 0.0f;
        SDK::FVector center{};
        if (useRadius && player)
        {
            center = player->K2_GetActorLocation();
        }

        if (!SDK::UObject::GObjects)
            return;

        const int32_t totalObjects = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < totalObjects; ++i)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
            if (!obj || !obj->IsA(SDK::UInstancedStaticMeshComponent::StaticClass()))
                continue;

            auto* component = static_cast<SDK::UInstancedStaticMeshComponent*>(obj);
            if (!component || component->IsDefaultObject() || !SDK::UKismetSystemLibrary::IsValid(component))
                continue;

            if (!MatchesEnvironmentSource(source, component))
                continue;

            std::string meshName;
            if (component->StaticMesh)
            {
                meshName = component->StaticMesh->GetName();
            }

            std::string ownerName;
            if (SDK::AActor* owner = component->GetOwner())
            {
                ownerName = owner->GetName();
            }

            if (!MatchesEnvironmentCategory(category, meshName) &&
                !MatchesEnvironmentCategory(category, ownerName) &&
                !MatchesEnvironmentCategory(category, component->GetName()))
            {
                continue;
            }

            std::vector<int32_t> indices;
            if (useRadius)
            {
                SDK::TArray<int32_t> overlapping = component->GetInstancesOverlappingSphere(center, radius, true);
                const int32_t overlapCount = GetTArrayNum(overlapping);
                int32_t* overlapData = GetTArrayData(overlapping);
                for (int32_t idx = 0; idx < overlapCount; ++idx)
                {
                    if (component->IsValidInstance(overlapData[idx]))
                    {
                        indices.push_back(overlapData[idx]);
                    }
                }
            }
            else
            {
                const int32_t count = component->GetInstanceCount();
                indices.reserve(static_cast<size_t>(count));
                for (int32_t idx = 0; idx < count; ++idx)
                {
                    if (component->IsValidInstance(idx))
                    {
                        indices.push_back(idx);
                    }
                }
            }

            if (!indices.empty())
            {
                PruneInstancesTarget target;
                target.component = component;
                target.instanceIndices = std::move(indices);
                target.meshName = meshName;
                target.ownerName = ownerName;
                batch.instanceTargets.push_back(std::move(target));
            }
        }
    }

    void Cheats::ExecuteDueEnvironmentPruneBatches()
    {
        if (pendingEnvironmentPruneBatches_.empty())
            return;

        const auto now = std::chrono::steady_clock::now();
        auto it = pendingEnvironmentPruneBatches_.begin();
        while (it != pendingEnvironmentPruneBatches_.end())
        {
            if (now < it->executeAt)
            {
                ++it;
                continue;
            }

            int removedActors = 0;
            int removedInstances = 0;

            for (PruneInstancesTarget& instanceTarget : it->instanceTargets)
            {
                if (!instanceTarget.component || !SDK::UKismetSystemLibrary::IsValid(instanceTarget.component))
                    continue;

                SDK::TArray<int32_t> validIndices;
                for (int32_t idx : instanceTarget.instanceIndices)
                {
                    if (instanceTarget.component->IsValidInstance(idx))
                    {
                        validIndices.Add(idx);
                    }
                }

                const int32_t validCount = GetTArrayNum(validIndices);
                if (validCount > 0)
                {
                    if (instanceTarget.component->RemoveInstances(validIndices))
                    {
                        removedInstances += validCount;
                    }
                    else
                    {
                        LOG_WARN("[Cheats] Environment prune RemoveInstances failed for mesh=" << instanceTarget.meshName);
                    }
                }
            }

            LOG_INFO("[Cheats] Environment prune executed category=" << CategoryToLabel(it->category)
                << " source=" << SourceToLabel(it->source)
                << " persistent=" << (it->persistent ? "true" : "false")
                << " removedActors=" << removedActors
                << " removedInstances=" << removedInstances
                << " queuedInstanceComponents=" << it->instanceTargets.size());

            it = pendingEnvironmentPruneBatches_.erase(it);
        }
    }

    void Cheats::UpdateEnvironmentPrune(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player)
    {
        if (!world)
            return;

        if (world != lastEnvironmentPruneWorld_)
        {
            lastEnvironmentPruneWorld_ = world;
            pendingEnvironmentPruneBatches_.clear();
            plantsBushesRule_.nextCollectAt = std::chrono::steady_clock::time_point{};
            treesRule_.nextCollectAt = std::chrono::steady_clock::time_point{};
            foliageRule_.nextCollectAt = std::chrono::steady_clock::time_point{};
            grassRule_.nextCollectAt = std::chrono::steady_clock::time_point{};
            instancedStaticMeshRule_.nextCollectAt = std::chrono::steady_clock::time_point{};
            LOG_INFO("[Cheats] Environment prune world changed - pending batches cleared");
        }

        const auto now = std::chrono::steady_clock::now();

        auto updateRule = [&](EnvironmentPruneCategory category, EnvironmentPruneRule& rule)
        {
            if (!rule.enabled)
                return;

            if (now < rule.nextCollectAt)
                return;

            std::string queueMessage = QueueEnvironmentPruneNow(world, player, category, rule.source, rule.radius, true);
            LOG_INFO("[Cheats] Environment prune persistent collect " << queueMessage);
            rule.nextCollectAt = now + std::chrono::milliseconds(static_cast<int64_t>(rule.intervalSeconds * 1000.0f));
        };

        updateRule(EnvironmentPruneCategory::PlantsBushes, plantsBushesRule_);
        updateRule(EnvironmentPruneCategory::Trees, treesRule_);
        updateRule(EnvironmentPruneCategory::All, foliageRule_);
        updateRule(EnvironmentPruneCategory::All, grassRule_);
        updateRule(EnvironmentPruneCategory::All, instancedStaticMeshRule_);

        ExecuteDueEnvironmentPruneBatches();
    }

    std::string Cheats::RemovePlantsBushesOnce(SDK::UWorld* world, float radius)
    {
        return QueueEnvironmentPruneNow(world, FindPlayer(world), EnvironmentPruneCategory::PlantsBushes, EnvironmentPruneSource::All, radius, false);
    }

    std::string Cheats::RemoveTreesOnce(SDK::UWorld* world, float radius)
    {
        return QueueEnvironmentPruneNow(world, FindPlayer(world), EnvironmentPruneCategory::Trees, EnvironmentPruneSource::All, radius, false);
    }

    std::string Cheats::TogglePlantsBushesPersistent(float radius, float intervalSeconds)
    {
        return ToggleEnvironmentPersistent(plantsBushesRule_, radius, intervalSeconds);
    }

    std::string Cheats::ToggleTreesPersistent(float radius, float intervalSeconds)
    {
        return ToggleEnvironmentPersistent(treesRule_, radius, intervalSeconds);
    }

    std::string Cheats::RemoveFoliageOnce(SDK::UWorld* world, float radius)
    {
        return QueueEnvironmentPruneNow(world, FindPlayer(world), EnvironmentPruneCategory::All, EnvironmentPruneSource::Foliage, radius, false);
    }

    std::string Cheats::ToggleFoliagePersistent(float radius, float intervalSeconds)
    {
        return ToggleEnvironmentPersistent(foliageRule_, radius, intervalSeconds);
    }

    std::string Cheats::RemoveGrassOnce(SDK::UWorld* world, float radius)
    {
        return QueueEnvironmentPruneNow(world, FindPlayer(world), EnvironmentPruneCategory::All, EnvironmentPruneSource::Grass, radius, false);
    }

    std::string Cheats::ToggleGrassPersistent(float radius, float intervalSeconds)
    {
        return ToggleEnvironmentPersistent(grassRule_, radius, intervalSeconds);
    }

    std::string Cheats::RemoveInstancedStaticMeshOnce(SDK::UWorld* world, float radius)
    {
        return QueueEnvironmentPruneNow(world, FindPlayer(world), EnvironmentPruneCategory::All, EnvironmentPruneSource::InstancedStaticMesh, radius, false);
    }

    std::string Cheats::ToggleInstancedStaticMeshPersistent(float radius, float intervalSeconds)
    {
        return ToggleEnvironmentPersistent(instancedStaticMeshRule_, radius, intervalSeconds);
    }

    void Cheats::ApplyAnomalySuppression(SDK::UWorld* world)
    {
        if (!world) return;

        if (!anomaliesDisabled_)
        {
            nextAnomalySuppressionAt_ = std::chrono::steady_clock::time_point{};
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextAnomalySuppressionAt_)
        {
            return;
        }
        nextAnomalySuppressionAt_ = now + std::chrono::seconds(30);

        Mod::ScopedProcessEventGuard guard;

        // Destroy all existing anomaly actors.
        {
            SDK::TArray<SDK::AActor*> anomalies;
            SDK::UGameplayStatics::GetAllActorsOfClass(
                world, SDK::AAnomalyBase::StaticClass(), &anomalies);

            const int32_t count = GetTArrayNum(anomalies);
            SDK::AActor** data  = GetTArrayData(anomalies);

            for (int32_t i = 0; i < count; ++i)
            {
                if (data[i] && SDK::UKismetSystemLibrary::IsValid(data[i]))
                {
                    // only destroy class BP_Anomaly_Portal_C, BP_Anomaly_Reflector_C, BP_Anomaly_ForestHedgehog_C and BP_AnomalyBallLightning_ForSpawners_C
                    if (data[i]->GetName().find("BP_Anomaly_Portal_C") == std::string::npos &&
                        data[i]->GetName().find("BP_Anomaly_Reflector_C") == std::string::npos &&
                        data[i]->GetName().find("BP_Anomaly_ForestHedgehog_C") == std::string::npos &&
                        data[i]->GetName().find("BP_AnomalyBallLightning_ForSpawners_C") == std::string::npos)
                    {
                        continue;
                    }

                    LOG_INFO("[Cheats] ApplyAnomalySuppression: destroying anomaly " << data[i]->GetName());
                    data[i]->K2_DestroyActor();
                }
            }

            if (count > 0)
            {
                LOG_INFO("[Cheats] ApplyAnomalySuppression: destroyed " << count << " anomalies");
            }
        }

        // Disable all anomaly spawner actors (stop ticking so they can't spawn new ones).
        {
            SDK::TArray<SDK::AActor*> spawners;
            SDK::UGameplayStatics::GetAllActorsOfClass(
                world, SDK::AAnomalySpawnPoint::StaticClass(), &spawners);

            const int32_t count = GetTArrayNum(spawners);
            SDK::AActor** data  = GetTArrayData(spawners);

            for (int32_t i = 0; i < count; ++i)
            {
                if (data[i] && SDK::UKismetSystemLibrary::IsValid(data[i]))
                {
                    data[i]->SetActorTickEnabled(false);
                    data[i]->SetActorHiddenInGame(true);
                    data[i]->SetActorEnableCollision(false);
                }
            }
        }

    }

    // -----------------------------------------------------------------------
    // Spawn a QuickHeal injector near the player
    // -----------------------------------------------------------------------

    std::string Cheats::SpawnHealItem(SDK::UWorld* world)
    {
        if (!world) return "world is null";

        Mod::ScopedProcessEventGuard guard;

        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return "player not found";

        // Get position slightly in front of the player.
        SDK::FVector playerLoc = player->K2_GetActorLocation();
        SDK::FRotator playerRot = player->K2_GetActorRotation();
        SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(playerRot);

        SDK::FVector spawnLoc;
        spawnLoc.X = playerLoc.X + forward.X * 80.0f;
        spawnLoc.Y = playerLoc.Y + forward.Y * 80.0f;
        spawnLoc.Z = playerLoc.Z + 50.0f; // roughly hand height

        SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(
            spawnLoc, playerRot, {1.0f, 1.0f, 1.0f});

        // Build the gameplay tag for the heal item.
        static const wchar_t* kHealItemTag = L"Item.Consumable.Injector.QuickHeal";
        SDK::FGameplayTag typeTag;
        typeTag.TagName = SDK::BasicFilesImpleUtils::StringToName(kHealItemTag);

        SDK::FItemConfiguration cfg{};
        cfg.bShopItem = false;
        cfg.StartDurabilityRatio = 1.0f;
        cfg.StackAmount = 1;

        try
        {
            SDK::AActor* actor = SDK::UFLSpawn::SpawnItemByTypeTag(
                world, typeTag, spawnTransform, cfg, true);

            if (actor)
            {
                std::string name = actor->GetName();
                LOG_INFO("[Cheats] Spawned heal item: " << name);
                Mod::ModFeedback::ShowMessage(
                    L"[Mod] QuickHeal spawned!", 2.0f,
                    SDK::FLinearColor{0.2f, 1.0f, 0.5f, 1.0f});
                return "Spawned: " + name;
            }
            else
            {
                LOG_ERROR("[Cheats] SpawnItemByTypeTag returned null for QuickHeal");
                return "failed: SpawnItemByTypeTag returned null";
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Cheats] Exception spawning heal item: " << e.what());
            return std::string("failed: ") + e.what();
        }
        catch (...)
        {
            LOG_ERROR("[Cheats] Unknown exception spawning heal item");
            return "failed: unknown exception";
        }
    }

}
