

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

#include "Logging.hpp"
#include "ArenaSubsystem.hpp"

namespace Mod
{
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
        if (scale <= 0.0f) scale = 0.001f;
        if (scale > 1.0f) scale = 1.0f;
        
        bulletTimeScale_ = scale;
        LOG_INFO("[Cheats] Bullet Time scale set to " << scale);
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
        status << "  Bullet Time: " << (bulletTimeActive_ ? "ACTIVE" : "inactive") << " (Scale: " << bulletTimeScale_ << ")";
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
        
        LOG_INFO("[Cheats] All cheats deactivated");
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
            float hunger = playerStats->GetHunger();
            // Most games use 0-100 or 0-1 for hunger. ITR2 seems to use 0-100 based on health logic.
            if (hunger < 95.0f)
            {
                playerStats->ChangeHungerAndNotifyAll(100.0f - hunger);
            }
        }

        if (fatigueDisabled_.load())
        {
            float stamina = playerStats->GetStamina();
            float maxStamina = playerStats->GetMaxStaminaAffectedByHunger();
            if (stamina < (maxStamina - 5.0f))
            {
                playerStats->ChangeStaminaAndNotifyAll(maxStamina - stamina);
            }
        }
    }

}
