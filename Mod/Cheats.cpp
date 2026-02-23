

/*
AILEARNINGS
Added explicit 'Set*' methods for cheat toggles (godmode, unlimited ammo, hunger, fatigue). These
were required by CommandHandler and arena startup code. The setters simply call the existing toggles
when the desired state differs. This change fixed multiple C2039 compile errors when invoking
SetUnlimitedAmmo/SetGodMode/etc.
*/

#include "Cheats.hpp"
#include "HookManager.hpp"

#include "ModFeedback.hpp"

#include <sstream>
#include <cmath>

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
    }

    Cheats::Cheats()
    {
    }

    void Cheats::ToggleGodMode()
    {
        godModeActive_ = !godModeActive_;
        LOG_INFO("[Cheats] God Mode " << (godModeActive_ ? "enabled" : "disabled"));

        Mod::ModFeedback::ShowMessage(
            godModeActive_ ? L"[Mod] God Mode: ON" : L"[Mod] God Mode: OFF",
            3.0f,
            godModeActive_ ? SDK::FLinearColor{0.2f, 1.0f, 0.2f, 1.0f} : SDK::FLinearColor{1.0f, 0.4f, 0.4f, 1.0f});
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

        // Update hook manager
        HookManager::Get().SetUnlimitedAmmoEnabled(unlimitedAmmoActive_);

        LOG_INFO("[Cheats] Unlimited Ammo " << (unlimitedAmmoActive_ ? "enabled" : "disabled"));

        Mod::ModFeedback::ShowMessage(
            unlimitedAmmoActive_ ? L"[Mod] Unlimited Ammo: ON" : L"[Mod] Unlimited Ammo: OFF",
            3.0f,
            unlimitedAmmoActive_ ? SDK::FLinearColor{0.2f, 1.0f, 1.0f, 1.0f} : SDK::FLinearColor{1.0f, 0.6f, 0.2f, 1.0f});

        // also toggle durability bypass when enabling unlimited ammo, since they are often used together and durability bypass is simpler (doesn't require ProcessEvent hook logic)
        if (unlimitedAmmoActive_ && !durabilityBypassActive_)
        {
            ToggleDurabilityBypass();
        }

    }

    void Cheats::ToggleDurabilityBypass()
    {
        durabilityBypassActive_ = !durabilityBypassActive_;

        HookManager::Get().SetDurabilityBypassEnabled(durabilityBypassActive_);

        LOG_INFO("[Cheats] Durability Bypass " << (durabilityBypassActive_ ? "enabled" : "disabled"));

        Mod::ModFeedback::ShowMessage(
            durabilityBypassActive_ ? L"[Mod] Durability Bypass: ON" : L"[Mod] Durability Bypass: OFF",
            3.0f,
            durabilityBypassActive_ ? SDK::FLinearColor{0.2f, 1.0f, 0.6f, 1.0f} : SDK::FLinearColor{1.0f, 0.6f, 0.2f, 1.0f});
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

        LOG_INFO("[Cheats] Hunger Disabled " << (hungerDisabled_ ? "enabled" : "disabled"));

        Mod::ModFeedback::ShowMessage(
            hungerDisabled_ ? L"[Mod] Hunger: DISABLED" : L"[Mod] Hunger: ENABLED",
            3.0f,
            hungerDisabled_ ? SDK::FLinearColor{0.2f, 1.0f, 0.4f, 1.0f} : SDK::FLinearColor{1.0f, 0.6f, 0.2f, 1.0f});
    }

    bool Cheats::IsHungerDisabledActive() const
    {
        return hungerDisabled_;
    }

    void Cheats::ToggleFatigueDisabled()
    {
        fatigueDisabled_ = !fatigueDisabled_;
        HookManager::Get().SetFatigueDisabled(fatigueDisabled_);

        LOG_INFO("[Cheats] Fatigue Disabled " << (fatigueDisabled_ ? "enabled" : "disabled"));

        Mod::ModFeedback::ShowMessage(
            fatigueDisabled_ ? L"[Mod] Fatigue: DISABLED" : L"[Mod] Fatigue: ENABLED",
            3.0f,
            fatigueDisabled_ ? SDK::FLinearColor{0.2f, 1.0f, 0.4f, 1.0f} : SDK::FLinearColor{1.0f, 0.6f, 0.2f, 1.0f});
    }

    bool Cheats::IsFatigueDisabledActive() const
    {
        return fatigueDisabled_;
    }

    void Cheats::ToggleBulletTime()
    {
        bulletTimeActive_ = !bulletTimeActive_;

        LOG_INFO("[Cheats] Bullet Time " << (bulletTimeActive_ ? "enabled" : "disabled") << " (Scale: " << bulletTimeScale_ << ")");

        Mod::ModFeedback::ShowMessage(
            bulletTimeActive_ ? L"[Mod] Bullet Time: ON" : L"[Mod] Bullet Time: OFF",
            3.0f,
            bulletTimeActive_ ? SDK::FLinearColor{0.6f, 0.2f, 1.0f, 1.0f} : SDK::FLinearColor{1.0f, 0.6f, 0.2f, 1.0f});

        // Forced update of global dilation when toggling off
        if (!bulletTimeActive_)
        {
            // We'll reset in the next Update() call, but logging it here
        }
    }

    void Cheats::SetBulletTimeScale(float scale)
    {
        if (scale <= 0.0f) scale = Mod::Tuning::kBulletTimeMinScale;
        if (scale > Mod::Tuning::kBulletTimeMaxScale) scale = Mod::Tuning::kBulletTimeMaxScale;
        
        bulletTimeScale_ = scale;
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
        Mod::ModFeedback::ShowMessage(noClipActive_ ? L"[Mod] NoClip: ON" : L"[Mod] NoClip: OFF", 2.0f);
        LOG_INFO("[Cheats] NoClip " << (noClipActive_ ? "enabled" : "disabled"));
    }

    bool Cheats::IsNoClipActive() const
    {
        return noClipActive_;
    }

    void Cheats::ToggleJumpAllowed()
    {
        CheatSubsystem_t* cheatsub = GetCheatSubsystem();
        if (!cheatsub)
        {
            LOG_WARN("[Cheats] ToggleJumpAllowed: cheat subsystem not available");
            return;
        }
        bool oldVal = cheatsub->IsJumpAllowed();
        cheatsub->SetJumpAllowed(!oldVal);
        jumpAllowedActive_ = !oldVal;
        Mod::ModFeedback::ShowMessage(jumpAllowedActive_ ? L"[Mod] Jump: ALLOWED" : L"[Mod] Jump: DISABLED", 2.0f);
        LOG_INFO("[Cheats] JumpAllowed " << (jumpAllowedActive_ ? "enabled" : "disabled"));
    }

    bool Cheats::IsJumpAllowedActive() const
    {
        return jumpAllowedActive_;
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
                if (actors.Num() > 0)
                    replicator = static_cast<SDK::ARadiusGameDataReplicator*>(actors[0]);
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
                if (actors.Num() > 0)
                    replicator = static_cast<SDK::ARadiusGameDataReplicator*>(actors[0]);
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
        LOG_INFO("[Cheats] Debug mode " << (debugModeActive_ ? "enabled" : "disabled"));
        Mod::ModFeedback::ShowMessage(debugModeActive_ ? L"[Mod] Debug: ON" : L"[Mod] Debug: OFF", 2.0f);
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

        portableLightIntensityScale_.store(scale, std::memory_order_relaxed);
        portableLightScaleDirty_.store(true, std::memory_order_release);

        std::wstringstream msg;
        msg << L"[Mod] Light brightness scale: " << scale;
        Mod::ModFeedback::ShowMessage(msg.str().c_str(), 3.0f, SDK::FLinearColor{0.9f, 0.9f, 0.2f, 1.0f});

        LOG_INFO("[Cheats] Portable light intensity scale set to " << scale);
    }

    float Cheats::GetPortableLightIntensityScale() const
    {
        return portableLightIntensityScale_.load(std::memory_order_relaxed);
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
        status << "  Jump Allowed: " << (jumpAllowedActive_ ? "ON" : "off") << "\n";
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
        if (jumpAllowedActive_) ToggleJumpAllowed();
        if (debugModeActive_) ToggleDebugMode();

        // Clear any cached light pointers/intensities on level change.
        portableLightOriginalIntensity_.clear();
        portableLightScaleDirty_.store(true, std::memory_order_release);
        nextPortableLightScan_ = std::chrono::steady_clock::time_point{};
        
        LOG_INFO("[Cheats] All cheats deactivated");
    }

    void Cheats::ApplyPortableLightBrightness(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player)
    {
        (void)player;

        if (!world)
            return;

        const float scale = portableLightIntensityScale_.load(std::memory_order_relaxed);
        if (!std::isfinite(scale) || scale <= 0.0f)
            return;

        // Default scale: do nothing unless we've previously scaled lights (or need to resync after a level change).
        const bool isDefault = std::fabs(scale - 1.0f) < 0.0001f;
        const bool dirty = portableLightScaleDirty_.load(std::memory_order_acquire);
        if (isDefault && !dirty)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (!dirty && nextPortableLightScan_ != std::chrono::steady_clock::time_point{} && now < nextPortableLightScan_)
            return;

        // Even when scaled, scan infrequently to catch newly spawned/attached lights.
        nextPortableLightScan_ = now + std::chrono::seconds(2);
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
            auto it = portableLightOriginalIntensity_.find(base);
            if (it == portableLightOriginalIntensity_.end())
            {
                it = portableLightOriginalIntensity_.emplace(base, base->Intensity).first;
            }

            const float target = it->second * scale;
            if (std::fabs(base->Intensity - target) > 0.01f)
            {
                light->SetIntensity(target);
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

    void Cheats::Update(SDK::UWorld *world)
    {
        if (!world)
            return;

        auto *player = FindPlayer(world);

        if (player && godModeActive_.load())
        {
            ApplyGodMode(player);
        }

        if (player && (hungerDisabled_.load() || fatigueDisabled_.load()))
        {
            ApplyStatsCheats(player);
        }

        // Apply Bullet Time
        float targetGlobalDilation = bulletTimeActive_.load() ? bulletTimeScale_.load() : 1.0f;
        SDK::UGameplayStatics::SetGlobalTimeDilation(world, targetGlobalDilation);

        if (player)
        {
            // Player CustomTimeDilation should be 1/GlobalDilation to maintain normal speed
            float targetPlayerDilation = 1.0f / targetGlobalDilation;
            
            if (player->CustomTimeDilation != targetPlayerDilation)
            {
                player->CustomTimeDilation = targetPlayerDilation;
            }

            // Apply to held items and potentially arms if they are separate actors/components
            UpdateHeldItemsDilation(world, targetPlayerDilation, player);

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

        // Flashlight/headlamp brightness scaling (runs infrequently; safe even if player is null)
        ApplyPortableLightBrightness(world, player);
    }

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
            player->PlayerStats = static_cast<SDK::UPlayerStatsComponent *>(statsComponent);
        }

        auto *playerStats = player->PlayerStats;

        // Get current health
        float currentHealth = playerStats->GetHealth();

        // Max health - use a reasonable value (100 is typical for this game)
        float maxHealth = 100.0f;

        //LOG_INFO("[Cheats] Current Health: " << currentHealth << " / " << maxHealth);

        // If health is below max, restore it by calling Server_ChangeHealth with positive delta
        if (currentHealth < maxHealth)
        {
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
        }

        if (fatigueDisabled_.load())
        {
            // simple approach: jam the current stamina value up to the max so the
            // player never runs out. (previous delta based approach sometimes
            // allowed tiny losses over time.)
            float maxStamina = playerStats->GetMaxStaminaAffectedByHunger();
            playerStats->CurrentStamina = maxStamina;
        }
    }

}
