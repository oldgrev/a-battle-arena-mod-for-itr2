/*
AILEARNINGS:
- ABPA_Scope_C::SceneCaptureComponent2D at 0x0840 — the scope's capture camera
- USceneCaptureComponent2D::PostProcessSettings at 0x0340 (FPostProcessSettings)
- USceneCaptureComponent2D::PostProcessBlendWeight at 0x0A60 (float)
- USceneCaptureComponent::bCaptureEveryFrame at 0x0232 bit 0
- The game's scopes normally have bCaptureEveryFrame=false; they use manual CaptureScene() calls
- We do NOT need to change bCaptureEveryFrame — the scope ticks and calls CaptureScene() when
  the player is looking through it. Our PP modifications are persistent so they take effect
  on the next capture automatically.
- ABPA_Scope_C inherits from ABPA_RadiusStaticMeshItemBase_C which inherits from ARadiusItemBase
- Use GetAllPlayerItems to find all player items, then IsA() check for scope class
- PostProcessSettings override flags (bOverride_*) must be set to 1 for each property to take effect
*/

#include "ScopeNVGSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "HookManager.hpp"  // For ScopedProcessEventGuard

#include "..\CppSDK\SDK\BPA_Scope_classes.hpp"
#include "..\CppSDK\SDK\IntoTheRadius2_classes.hpp"

#include <sstream>
#include <algorithm>
#include <cmath>

namespace Mod
{
    ScopeNVGSubsystem& ScopeNVGSubsystem::Get()
    {
        static ScopeNVGSubsystem instance;
        return instance;
    }

    // =========================================================================
    // Scope Discovery
    // =========================================================================

    std::string ScopeNVGSubsystem::ScanScopes(SDK::UWorld* world)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        oss << "=== ScopeNVG: ScanScopes ===\n";

        if (!world)
        {
            oss << "ERROR: World is null\n";
            LOG_ERROR("[ScopeNVG] ScanScopes: world is null");
            return oss.str();
        }

        // First disable NVG on all currently tracked scopes (clean slate)
        for (auto& scope : scopes_)
        {
            if (scope.nvgEnabled && IsScopeValid(scope))
            {
                RestoreAllApproaches(scope);
                scope.nvgEnabled = false;
            }
        }

        // Get the player
        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        if (!playerPawn)
        {
            oss << "ERROR: Player pawn is null\n";
            LOG_ERROR("[ScopeNVG] ScanScopes: player pawn is null");
            scopes_.clear();
            return oss.str();
        }

        oss << "Player: " << playerPawn->GetName() << "\n";
        LOG_INFO("[ScopeNVG] ScanScopes: player=" << playerPawn->GetName());

        // Get the container subsystem to enumerate player items
        SDK::URadiusContainerSubsystem* containerSys = nullptr;
        try
        {
            SDK::UObject* csObj = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
                world,
                SDK::URadiusContainerSubsystem::StaticClass()
            );
            if (csObj && csObj->IsA(SDK::URadiusContainerSubsystem::StaticClass()))
                containerSys = static_cast<SDK::URadiusContainerSubsystem*>(csObj);
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ScanScopes: exception getting container subsystem");
        }

        if (!containerSys)
        {
            oss << "ERROR: Container subsystem not found\n";
            LOG_ERROR("[ScopeNVG] ScanScopes: container subsystem is null");
            scopes_.clear();
            return oss.str();
        }

        // Get all player items
        SDK::TArray<SDK::ARadiusItemBase*> playerItems;
        bool gotItems = false;
        try
        {
            Mod::ScopedProcessEventGuard guard;
            gotItems = containerSys->GetAllPlayerItems(playerPawn, &playerItems);
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ScanScopes: exception in GetAllPlayerItems");
        }

        int totalItems = gotItems ? playerItems.Num() : 0;
        oss << "Player items: " << totalItems << "\n";
        LOG_INFO("[ScopeNVG] ScanScopes: GetAllPlayerItems found " << totalItems << " items");

        // Scan for scopes
        std::vector<ScopeInfo> newScopes;
        SDK::UClass* scopeClass = SDK::ABPA_Scope_C::StaticClass();
        
        if (!scopeClass)
        {
            oss << "ERROR: ABPA_Scope_C::StaticClass() returned null\n";
            LOG_ERROR("[ScopeNVG] ScanScopes: scope static class is null");
            scopes_.clear();
            return oss.str();
        }

        for (int i = 0; i < totalItems; ++i)
        {
            SDK::ARadiusItemBase* item = playerItems[i];
            if (!item) continue;

            try
            {
                if (!SDK::UKismetSystemLibrary::IsValid(item)) continue;
            }
            catch (...) { continue; }

            bool isScope = false;
            try
            {
                isScope = item->IsA(scopeClass);
            }
            catch (...) { continue; }

            if (!isScope) continue;

            // Found a scope!
            SDK::ABPA_Scope_C* scopeActor = static_cast<SDK::ABPA_Scope_C*>(item);
            
            std::string actorName;
            std::string className;
            try
            {
                actorName = scopeActor->GetName();
                className = scopeActor->Class ? scopeActor->Class->GetName() : "Unknown";
            }
            catch (...)
            {
                actorName = "Unknown";
                className = "Unknown";
            }

            // Check if it has a capture component
            SDK::USceneCaptureComponent2D* captureComp = GetCaptureComponent(scopeActor);
            
            // Build short display name
            std::string shortName = className;
            // Strip common prefixes: "BP_Scope_", "BPA_Scope_", "BP_", etc.
            if (shortName.size() > 9 && shortName.substr(0, 9) == "BP_Scope_")
                shortName = shortName.substr(9);
            else if (shortName.size() > 10 && shortName.substr(0, 10) == "BPA_Scope_")
                shortName = shortName.substr(10);
            else if (shortName.size() > 3 && shortName.substr(0, 3) == "BP_")
                shortName = shortName.substr(3);
            // Strip _C suffix
            if (shortName.size() > 2 && shortName.substr(shortName.size() - 2) == "_C")
                shortName = shortName.substr(0, shortName.size() - 2);

            ScopeInfo info;
            info.actor = scopeActor;
            info.name = shortName;
            info.className = className;
            info.nvgEnabled = false;
            info.captureCompAddr = captureComp ? reinterpret_cast<uintptr_t>(captureComp) : 0;

            oss << "  Scope[" << newScopes.size() << "]: " << shortName
                << " (" << className << ")"
                << " actor=" << actorName
                << " capture=" << (captureComp ? "YES" : "NO")
                << "\n";

            LOG_INFO("[ScopeNVG] Found scope[" << newScopes.size() << "]: "
                << className << " name=" << actorName
                << " captureComp=" << (void*)captureComp);

            // Log capture component details if present
            if (captureComp)
            {
                try
                {
                    float fov = captureComp->FOVAngle;
                    auto* rt = captureComp->TextureTarget;
                    float ppWeight = captureComp->PostProcessBlendWeight;
                    
                    // Check bCaptureEveryFrame
                    uint8_t captureBits = *reinterpret_cast<uint8_t*>(
                        reinterpret_cast<uintptr_t>(captureComp) + 0x0232);
                    bool captureEveryFrame = (captureBits & 0x01) != 0;

                    oss << "    FOV=" << fov
                        << " RT=" << (rt ? rt->GetName() : "NULL")
                        << " PPWeight=" << ppWeight
                        << " CaptureEveryFrame=" << (captureEveryFrame ? "Y" : "N")
                        << "\n";

                    LOG_INFO("[ScopeNVG]   FOV=" << fov
                        << " RT=" << (rt ? rt->GetName() : "NULL")
                        << " PPWeight=" << ppWeight
                        << " bCaptureEveryFrame=" << captureEveryFrame);
                }
                catch (...)
                {
                    oss << "    (failed to read capture details)\n";
                    LOG_WARN("[ScopeNVG]   Exception reading capture component details");
                }

                // Log existing LensMatInst
                try
                {
                    auto* matInst = scopeActor->LensMatInst;
                    auto* srcMat = scopeActor->LensSourceMat;
                    oss << "    LensMatInst=" << (matInst ? matInst->GetName() : "NULL")
                        << " LensSourceMat=" << (srcMat ? srcMat->GetName() : "NULL")
                        << "\n";
                    LOG_INFO("[ScopeNVG]   LensMatInst=" << (matInst ? matInst->GetName() : "NULL")
                        << " LensSourceMat=" << (srcMat ? srcMat->GetName() : "NULL"));
                }
                catch (...)
                {
                    LOG_WARN("[ScopeNVG]   Exception reading scope material info");
                }

                // Log FirearmParent
                try
                {
                    auto* firearm = scopeActor->FirearmParent;
                    if (firearm)
                    {
                        std::string firearmName = firearm->GetName();
                        std::string firearmClass = firearm->Class ? firearm->Class->GetName() : "?";
                        oss << "    Weapon: " << firearmName << " (" << firearmClass << ")\n";
                        LOG_INFO("[ScopeNVG]   FirearmParent=" << firearmName << " class=" << firearmClass);
                    }
                    else
                    {
                        oss << "    Weapon: (not attached to weapon)\n";
                        LOG_INFO("[ScopeNVG]   FirearmParent=null (loose scope)");
                    }
                }
                catch (...)
                {
                    LOG_WARN("[ScopeNVG]   Exception reading FirearmParent");
                }
            }

            newScopes.push_back(std::move(info));
        }

        // Also scan GObjects as a fallback — some scopes may not be in the container system
        // (e.g., if they're loose in the world but the player is holding them)
        LOG_INFO("[ScopeNVG] ScanScopes: also scanning GObjects for additional scopes...");
        if (SDK::UObject::GObjects)
        {
            int gObjScopes = 0;
            for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj) continue;

                try
                {
                    if (!obj->IsA(scopeClass)) continue;
                }
                catch (...) { continue; }

                auto* scopeActor = static_cast<SDK::ABPA_Scope_C*>(obj);
                
                // Check validity
                try
                {
                    if (!SDK::UKismetSystemLibrary::IsValid(scopeActor)) continue;
                }
                catch (...) { continue; }

                // Check if it belongs to local player
                bool belongsToPlayer = false;
                try
                {
                    Mod::ScopedProcessEventGuard guard;
                    belongsToPlayer = scopeActor->DoesBelongToLocalPlayer();
                }
                catch (...)
                {
                    // If DoesBelongToLocalPlayer throws, skip
                    continue;
                }

                if (!belongsToPlayer) continue;

                // Check if already discovered
                bool alreadyFound = false;
                for (auto& s : newScopes)
                {
                    if (s.actor == scopeActor)
                    {
                        alreadyFound = true;
                        break;
                    }
                }
                if (alreadyFound) continue;

                // New scope found via GObjects scan
                std::string actorName, className;
                try
                {
                    actorName = scopeActor->GetName();
                    className = scopeActor->Class ? scopeActor->Class->GetName() : "Unknown";
                }
                catch (...)
                {
                    actorName = "GObj_Unknown";
                    className = "Unknown";
                }

                SDK::USceneCaptureComponent2D* captureComp = GetCaptureComponent(scopeActor);

                std::string shortName = className;
                if (shortName.size() > 9 && shortName.substr(0, 9) == "BP_Scope_")
                    shortName = shortName.substr(9);
                else if (shortName.size() > 10 && shortName.substr(0, 10) == "BPA_Scope_")
                    shortName = shortName.substr(10);
                else if (shortName.size() > 3 && shortName.substr(0, 3) == "BP_")
                    shortName = shortName.substr(3);
                if (shortName.size() > 2 && shortName.substr(shortName.size() - 2) == "_C")
                    shortName = shortName.substr(0, shortName.size() - 2);

                ScopeInfo info;
                info.actor = scopeActor;
                info.name = shortName;
                info.className = className;
                info.nvgEnabled = false;
                info.captureCompAddr = captureComp ? reinterpret_cast<uintptr_t>(captureComp) : 0;

                oss << "  Scope[" << newScopes.size() << "] (GObj): " << shortName
                    << " (" << className << ")"
                    << " actor=" << actorName
                    << " capture=" << (captureComp ? "YES" : "NO")
                    << "\n";
                LOG_INFO("[ScopeNVG] Found scope via GObj[" << newScopes.size() << "]: "
                    << className << " name=" << actorName);

                newScopes.push_back(std::move(info));
                gObjScopes++;
            }
            if (gObjScopes > 0)
            {
                oss << "  (Found " << gObjScopes << " additional scopes via GObjects)\n";
                LOG_INFO("[ScopeNVG] GObj scan found " << gObjScopes << " additional scopes");
            }
        }

        scopes_ = std::move(newScopes);
        oss << "\nTotal scopes found: " << scopes_.size() << "\n";
        LOG_INFO("[ScopeNVG] ScanScopes complete: " << scopes_.size() << " scopes");

        return oss.str();
    }

    // =========================================================================
    // Enable / Disable / Toggle
    // =========================================================================

    std::string ScopeNVGSubsystem::EnableScopeNVG(int scopeIndex)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;

        if (scopeIndex < 0 || scopeIndex >= static_cast<int>(scopes_.size()))
        {
            oss << "ERROR: Invalid scope index " << scopeIndex
                << " (have " << scopes_.size() << " scopes)\n";
            LOG_WARN("[ScopeNVG] EnableScopeNVG: invalid index " << scopeIndex);
            return oss.str();
        }

        auto& scope = scopes_[scopeIndex];

        if (!IsScopeValid(scope))
        {
            oss << "ERROR: Scope " << scope.name << " is no longer valid (destroyed?)\n";
            LOG_WARN("[ScopeNVG] EnableScopeNVG: scope " << scope.name << " invalid");
            return oss.str();
        }

        if (scope.nvgEnabled)
        {
            oss << "Scope " << scope.name << " NVG already enabled\n";
            return oss.str();
        }

        auto* cc = GetCaptureComponent(scope.actor);
        LOG_INFO("[ScopeNVG] === ENABLING NVG on scope " << scopeIndex << " (" << scope.name << ") ===");
        
        // Log state BEFORE
        if (cc) LogCaptureState("BEFORE-ENABLE", cc, scope);

        // Backup original state
        BackupScopeState(scope);

        // Apply all three approaches
        ApplyAllApproaches(scope);
        scope.nvgEnabled = true;

        // Log state AFTER
        if (cc) LogCaptureState("AFTER-ENABLE", cc, scope);

        oss << "Scope " << scope.name << " NVG ENABLED\n";
        LOG_INFO("[ScopeNVG] === ENABLE COMPLETE for " << scope.name << " ===");

        return oss.str();
    }

    std::string ScopeNVGSubsystem::DisableScopeNVG(int scopeIndex)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;

        if (scopeIndex < 0 || scopeIndex >= static_cast<int>(scopes_.size()))
        {
            oss << "ERROR: Invalid scope index " << scopeIndex << "\n";
            LOG_WARN("[ScopeNVG] DisableScopeNVG: invalid index " << scopeIndex);
            return oss.str();
        }

        auto& scope = scopes_[scopeIndex];

        if (!scope.nvgEnabled)
        {
            oss << "Scope " << scope.name << " NVG already disabled\n";
            return oss.str();
        }

        auto* cc = GetCaptureComponent(scope.actor);
        LOG_INFO("[ScopeNVG] === DISABLING NVG on scope " << scopeIndex << " (" << scope.name << ") ===");
        
        if (cc) LogCaptureState("BEFORE-DISABLE", cc, scope);

        if (IsScopeValid(scope))
        {
            RestoreAllApproaches(scope);
        }
        scope.nvgEnabled = false;

        if (cc) LogCaptureState("AFTER-DISABLE", cc, scope);

        oss << "Scope " << scope.name << " NVG DISABLED\n";
        LOG_INFO("[ScopeNVG] === DISABLE COMPLETE for " << scope.name << " ===");

        return oss.str();
    }

    std::string ScopeNVGSubsystem::ToggleScopeNVG(int scopeIndex)
    {
        // Note: lock is acquired inside Enable/Disable
        if (scopeIndex < 0 || scopeIndex >= static_cast<int>(scopes_.size()))
        {
            return "ERROR: Invalid scope index " + std::to_string(scopeIndex) + "\n";
        }

        if (scopes_[scopeIndex].nvgEnabled)
            return DisableScopeNVG(scopeIndex);
        else
            return EnableScopeNVG(scopeIndex);
    }

    std::string ScopeNVGSubsystem::EnableAll()
    {
        std::ostringstream oss;
        for (int i = 0; i < static_cast<int>(scopes_.size()); ++i)
        {
            if (!scopes_[i].nvgEnabled)
                oss << EnableScopeNVG(i);
        }
        if (scopes_.empty())
            oss << "No scopes discovered. Run scan first.\n";
        return oss.str();
    }

    std::string ScopeNVGSubsystem::DisableAll()
    {
        std::ostringstream oss;
        for (int i = 0; i < static_cast<int>(scopes_.size()); ++i)
        {
            if (scopes_[i].nvgEnabled)
                oss << DisableScopeNVG(i);
        }
        return oss.str();
    }

    bool ScopeNVGSubsystem::AnyNVGEnabled() const
    {
        for (auto& s : scopes_)
            if (s.nvgEnabled) return true;
        return false;
    }

    // =========================================================================
    // Update (tick)
    // =========================================================================

    void ScopeNVGSubsystem::Update(SDK::UWorld* world)
    {
        if (!world) return;

        std::lock_guard<std::mutex> lock(mutex_);

        tickCounter_++;

        // Handle scan requests (from menu page open) 
        bool doScan = scanRequested_.exchange(false, std::memory_order_relaxed);
        
        // Also periodic auto-scan if enabled
        if (!doScan && autoScan_.load(std::memory_order_relaxed) && (tickCounter_ % kScanInterval == 0))
        {
            doScan = true;
        }

        if (doScan)
        {
            // Save NVG-enabled state per scope key so we can re-apply after re-scan
            std::map<std::string, bool> prevState;
            for (auto& s : scopes_)
                prevState[s.className + "_" + s.name] = s.nvgEnabled;

            // Disable NVG on active scopes before scan
            for (auto& scope : scopes_)
            {
                if (scope.nvgEnabled && IsScopeValid(scope))
                {
                    RestoreAllApproaches(scope);
                    scope.nvgEnabled = false;
                }
            }

            // Re-scan (quiet — not full ScanScopes with logging flood)
            SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
            if (playerPawn)
            {
                SDK::URadiusContainerSubsystem* containerSys = nullptr;
                try
                {
                    SDK::UObject* csObj = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
                        world, SDK::URadiusContainerSubsystem::StaticClass());
                    if (csObj && csObj->IsA(SDK::URadiusContainerSubsystem::StaticClass()))
                        containerSys = static_cast<SDK::URadiusContainerSubsystem*>(csObj);
                }
                catch (...) {}

                if (containerSys)
                {
                    SDK::TArray<SDK::ARadiusItemBase*> playerItems;
                    bool gotItems = false;
                    try
                    {
                        Mod::ScopedProcessEventGuard guard;
                        gotItems = containerSys->GetAllPlayerItems(playerPawn, &playerItems);
                    }
                    catch (...) {}

                    if (gotItems)
                    {
                        SDK::UClass* scopeClass = SDK::ABPA_Scope_C::StaticClass();
                        std::vector<ScopeInfo> newScopes;

                        for (int i = 0; i < playerItems.Num(); ++i)
                        {
                            SDK::ARadiusItemBase* item = playerItems[i];
                            if (!item) continue;
                            try { if (!SDK::UKismetSystemLibrary::IsValid(item)) continue; } catch (...) { continue; }
                            try { if (!item->IsA(scopeClass)) continue; } catch (...) { continue; }

                            auto* scopeActor = static_cast<SDK::ABPA_Scope_C*>(item);
                            auto* cc = GetCaptureComponent(scopeActor);

                            std::string className, actorName;
                            try
                            {
                                className = scopeActor->Class ? scopeActor->Class->GetName() : "Unknown";
                                actorName = scopeActor->GetName();
                            }
                            catch (...) { className = "Unknown"; actorName = "Unknown"; }

                            std::string shortName = className;
                            if (shortName.size() > 9 && shortName.substr(0, 9) == "BP_Scope_")
                                shortName = shortName.substr(9);
                            else if (shortName.size() > 10 && shortName.substr(0, 10) == "BPA_Scope_")
                                shortName = shortName.substr(10);
                            else if (shortName.size() > 3 && shortName.substr(0, 3) == "BP_")
                                shortName = shortName.substr(3);
                            if (shortName.size() > 2 && shortName.substr(shortName.size() - 2) == "_C")
                                shortName = shortName.substr(0, shortName.size() - 2);

                            ScopeInfo info;
                            info.actor = scopeActor;
                            info.name = shortName;
                            info.className = className;
                            info.nvgEnabled = false;
                            info.captureCompAddr = cc ? reinterpret_cast<uintptr_t>(cc) : 0;

                            // Restore previous NVG state
                            std::string key = className + "_" + shortName;
                            auto it = prevState.find(key);
                            if (it != prevState.end() && it->second)
                            {
                                BackupScopeState(info);
                                ApplyAllApproaches(info);
                                info.nvgEnabled = true;
                            }

                            newScopes.push_back(std::move(info));
                        }

                        scopes_ = std::move(newScopes);
                    }
                }
            }

            return;  // Skip per-scope PP re-apply on scan ticks
        }

        // // Per-tick: re-apply NVG effects on enabled scopes (in case the game resets things)
        // for (auto& scope : scopes_)
        // {
        //     if (!scope.nvgEnabled) continue;

        //     if (!IsScopeValid(scope))
        //     {
        //         LOG_WARN("[ScopeNVG] NVG-enabled scope " << scope.name << " no longer valid, disabling");
        //         scope.nvgEnabled = false;
        //         continue;
        //     }

        //     // Re-apply all approaches every tick to counteract game resets
        //     ApplyAllApproaches(scope);
        // }
    }

    // =========================================================================
    // ===== APPROACH A: PostProcess Settings + ShowFlags =====
    // =========================================================================

    void ScopeNVGSubsystem::ApplyApproachA_PostProcess(ScopeInfo& scope)
    {
        auto* captureComp = GetCaptureComponent(scope.actor);
        if (!captureComp)
        {
            LOG_WARN("[ScopeNVG] ApproachA: no capture component on " << scope.name);
            return;
        }

        LOG_INFO("[ScopeNVG] ApproachA: Applying PP to " << scope.name);

        // Step 1: Check ShowFlagSettings directly on the member. If PostProcessing
        // is listed as disabled, flip it to enabled in-place.
        try
        {
            auto& flags = captureComp->ShowFlagSettings;
            LOG_INFO("[ScopeNVG] ApproachA: ShowFlagSettings has " << flags.Num() << " entries");
            
            bool foundPP = false;
            for (int i = 0; i < flags.Num(); ++i)
            {
                auto& f = flags[i];
                const wchar_t* rawName = f.ShowFlagName.CStr();
                if (!rawName) continue;
                
                std::wstring wname(rawName);
                std::string name(wname.begin(), wname.end());
                LOG_INFO("[ScopeNVG] ApproachA: ShowFlag[" << i << "] = " << name << " Enabled=" << (f.Enabled ? "true" : "false"));

                if (name == "PostProcessing" && !f.Enabled)
                {
                    LOG_INFO("[ScopeNVG] ApproachA: FLIPPING PostProcessing from false -> true");
                    f.Enabled = true;
                    scope.flippedPPShowFlag = true;
                    foundPP = true;
                }
            }

            if (!foundPP)
            {
                LOG_INFO("[ScopeNVG] ApproachA: PostProcessing not found in ShowFlagSettings (defaults to enabled — good)");
            }
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachA: Exception reading/modifying ShowFlagSettings");
        }

        // Step 2: Write NVG post-process values
        auto& pp = captureComp->PostProcessSettings;

        // Enable override flags
        pp.bOverride_ColorSaturation = 1;
        pp.bOverride_ColorGain = 1;
        pp.bOverride_ColorGamma = 1;
        pp.bOverride_ColorOffset = 1;
        pp.bOverride_SceneColorTint = 1;
        pp.bOverride_ColorGammaShadows = 1;
        pp.bOverride_ColorGainShadows = 1;
        pp.bOverride_FilmSlope = 1;
        pp.bOverride_FilmToe = 1;
        pp.bOverride_FilmShoulder = 1;
        pp.bOverride_BloomIntensity = 1;
        pp.bOverride_BloomThreshold = 1;
        pp.bOverride_FilmGrainIntensity = 1;
        pp.bOverride_FilmGrainIntensityShadows = 1;
        pp.bOverride_FilmGrainIntensityMidtones = 1;
        pp.bOverride_FilmGrainIntensityHighlights = 1;
        pp.bOverride_FilmGrainTexelSize = 1;
        pp.bOverride_AutoExposureBias = 1;
        pp.bOverride_AutoExposureMethod = 1;
        pp.bOverride_AutoExposureMinBrightness = 1;
        pp.bOverride_AutoExposureMaxBrightness = 1;
        pp.bOverride_AmbientOcclusionIntensity = 1;
        pp.bOverride_MotionBlurAmount = 1;
        pp.bOverride_VignetteIntensity = 1;

        // Green NVG color grading
        pp.ColorSaturation = SDK::FVector4{0.2, 0.5, 0.2, 1.0};
        pp.ColorGain = SDK::FVector4{0.3, greenGain_, 0.25, 1.0};
        pp.ColorGamma = SDK::FVector4{0.95, 1.05, 0.95, 1.0};
        pp.ColorOffset = SDK::FVector4{0.0, 0.008, 0.0, 0.0};
        pp.SceneColorTint = SDK::FLinearColor{0.8f, 1.0f, 0.8f, 1.0f};
        pp.ColorGammaShadows = SDK::FVector4{1.0, 1.1, 1.0, 1.0};
        pp.ColorGainShadows = SDK::FVector4{0.4, 0.8, 0.35, 1.0};

        // Tone mapping
        pp.FilmToe = 0.8f;
        pp.FilmSlope = 0.88f;
        pp.FilmShoulder = 0.26f;

        // Auto-exposure (bright for NVG)
        pp.AutoExposureMethod = SDK::EAutoExposureMethod::AEM_Manual;
        pp.AutoExposureBias = exposureBias_;
        pp.AutoExposureMinBrightness = 0.03f;
        pp.AutoExposureMaxBrightness = 20.0f;

        // Bloom
        pp.BloomIntensity = bloomIntensity_;
        pp.BloomThreshold = 0.5f;

        // Film grain
        pp.FilmGrainIntensity = grainIntensity_ * 0.5f;
        pp.FilmGrainIntensityShadows = grainIntensity_ * 0.8f;
        pp.FilmGrainIntensityMidtones = grainIntensity_ * 0.4f;
        pp.FilmGrainIntensityHighlights = grainIntensity_ * 0.1f;
        pp.FilmGrainTexelSize = 1.5f;

        // No vignette
        pp.VignetteIntensity = 0.0f;
        pp.AmbientOcclusionIntensity = 0.0f;
        pp.MotionBlurAmount = 0.0f;

        // Set PP blend weight to full
        captureComp->PostProcessBlendWeight = 1.0f;

        LOG_INFO("[ScopeNVG] ApproachA: PP values written. PPWeight=1.0 ExposureBias=" << exposureBias_
            << " GreenGain=" << greenGain_ << " Grain=" << grainIntensity_);
    }

    void ScopeNVGSubsystem::RestoreApproachA_PostProcess(ScopeInfo& scope)
    {
        auto* captureComp = GetCaptureComponent(scope.actor);
        if (!captureComp) return;

        LOG_INFO("[ScopeNVG] ApproachA: Restoring PP on " << scope.name);

        // Reset override flags
        auto& pp = captureComp->PostProcessSettings;
        pp.bOverride_ColorSaturation = 0;
        pp.bOverride_ColorGain = 0;
        pp.bOverride_ColorGamma = 0;
        pp.bOverride_ColorOffset = 0;
        pp.bOverride_SceneColorTint = 0;
        pp.bOverride_ColorGammaShadows = 0;
        pp.bOverride_ColorGainShadows = 0;
        pp.bOverride_FilmSlope = 0;
        pp.bOverride_FilmToe = 0;
        pp.bOverride_FilmShoulder = 0;
        pp.bOverride_BloomIntensity = 0;
        pp.bOverride_BloomThreshold = 0;
        pp.bOverride_FilmGrainIntensity = 0;
        pp.bOverride_FilmGrainIntensityShadows = 0;
        pp.bOverride_FilmGrainIntensityMidtones = 0;
        pp.bOverride_FilmGrainIntensityHighlights = 0;
        pp.bOverride_FilmGrainTexelSize = 0;
        pp.bOverride_AutoExposureBias = 0;
        pp.bOverride_AutoExposureMethod = 0;
        pp.bOverride_AutoExposureMinBrightness = 0;
        pp.bOverride_AutoExposureMaxBrightness = 0;
        pp.bOverride_AmbientOcclusionIntensity = 0;
        pp.bOverride_MotionBlurAmount = 0;
        pp.bOverride_VignetteIntensity = 0;

        // Restore original blend weight
        if (scope.hadBackup)
        {
            captureComp->PostProcessBlendWeight = scope.origPostProcessBlendWeight;
        }

        // Restore original ShowFlagSettings (if we flipped PostProcessing)
        if (scope.flippedPPShowFlag)
        {
            try
            {
                auto& flags = captureComp->ShowFlagSettings;
                for (int i = 0; i < flags.Num(); ++i)
                {
                    const wchar_t* rawName = flags[i].ShowFlagName.CStr();
                    if (!rawName) continue;
                    std::wstring wname(rawName);
                    std::string name(wname.begin(), wname.end());
                    if (name == "PostProcessing")
                    {
                        LOG_INFO("[ScopeNVG] ApproachA: RESTORING PostProcessing from true -> false");
                        flags[i].Enabled = false;
                        break;
                    }
                }
                scope.flippedPPShowFlag = false;
            }
            catch (...)
            {
                LOG_ERROR("[ScopeNVG] ApproachA: Exception restoring ShowFlagSettings");
            }
        }

        LOG_INFO("[ScopeNVG] ApproachA: PP restored for " << scope.name);
    }

    // =========================================================================
    // ===== APPROACH B: LensMatInst MID Parameter Tweaks =====
    // =========================================================================

    void ScopeNVGSubsystem::ApplyApproachB_MaterialParams(ScopeInfo& scope)
    {
        if (!scope.actor) return;

        SDK::UMaterialInstanceDynamic* matInst = nullptr;
        try
        {
            matInst = scope.actor->LensMatInst;
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachB: Exception reading LensMatInst on " << scope.name);
            return;
        }

        if (!matInst)
        {
            LOG_WARN("[ScopeNVG] ApproachB: LensMatInst is null on " << scope.name);
            return;
        }

        LOG_INFO("[ScopeNVG] ApproachB: Setting material params on " << scope.name
            << " MID=" << matInst->GetName());

        try
        {
            Mod::ScopedProcessEventGuard guard;

            // Read current BrightnessMult for logging
            SDK::FName bmName = SDK::BasicFilesImpleUtils::StringToName(L"BrightnessMult");
            SDK::FName gcName = SDK::BasicFilesImpleUtils::StringToName(L"GridColor");

            float currentBrightness = matInst->K2_GetScalarParameterValue(bmName);
            LOG_INFO("[ScopeNVG] ApproachB: Current BrightnessMult=" << currentBrightness);

            // Set BrightnessMult higher for NVG brightness boost
            matInst->SetScalarParameterValue(bmName, 5.0f);
            LOG_INFO("[ScopeNVG] ApproachB: Set BrightnessMult=5.0");

            // Set GridColor to green as a visual indicator (even though grid might be off)
            SDK::FLinearColor greenColor{0.0f, 1.0f, 0.0f, 1.0f};
            LOG_INFO("[ScopeNVG] ApproachB: Original GridColor is: (" 
                << matInst->K2_GetVectorParameterValue(gcName).R << ","
                << matInst->K2_GetVectorParameterValue(gcName).G << ","
                << matInst->K2_GetVectorParameterValue(gcName).B << ","
                << matInst->K2_GetVectorParameterValue(gcName).A << ")");
            matInst->SetVectorParameterValue(gcName, greenColor);
            LOG_INFO("[ScopeNVG] ApproachB: Set GridColor=(0,1,0,1) green");

            // Verify the value was written
            float newBrightness = matInst->K2_GetScalarParameterValue(bmName);
            LOG_INFO("[ScopeNVG] ApproachB: Readback BrightnessMult=" << newBrightness);
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachB: Exception setting material params on " << scope.name);
        }
    }

    void ScopeNVGSubsystem::RestoreApproachB_MaterialParams(ScopeInfo& scope)
    {
        if (!scope.actor) return;

        SDK::UMaterialInstanceDynamic* matInst = nullptr;
        try { matInst = scope.actor->LensMatInst; } catch (...) { return; }
        if (!matInst) return;

        LOG_INFO("[ScopeNVG] ApproachB: Restoring material params on " << scope.name);

        try
        {
            Mod::ScopedProcessEventGuard guard;
            SDK::FName bmName = SDK::BasicFilesImpleUtils::StringToName(L"BrightnessMult");
            matInst->SetScalarParameterValue(bmName, scope.origBrightnessMult);
            SDK::FName gcName = SDK::BasicFilesImpleUtils::StringToName(L"GridColor");
            SDK::FLinearColor defaultGridColor{0.0f, 0.0f, 0.0f, 1.0f};
            matInst->SetVectorParameterValue(gcName, defaultGridColor);
            // Verify the value was written
            SDK::FLinearColor newGridColor = matInst->K2_GetVectorParameterValue(gcName);
            LOG_INFO("[ScopeNVG] ApproachB: Readback GridColor=("
                << newGridColor.R << "," << newGridColor.G << "," << newGridColor.B << "," << newGridColor.A << ")");

            LOG_INFO("[ScopeNVG] ApproachB: Restored BrightnessMult=" << scope.origBrightnessMult);
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachB: Exception restoring material params on " << scope.name);
        }
    }

    // =========================================================================
    // ===== APPROACH C: bCaptureEveryFrame + Force CaptureScene =====
    // =========================================================================

    void ScopeNVGSubsystem::ApplyApproachC_ForcedCapture(ScopeInfo& scope)
    {
        auto* captureComp = GetCaptureComponent(scope.actor);
        if (!captureComp)
        {
            LOG_WARN("[ScopeNVG] ApproachC: no capture component on " << scope.name);
            return;
        }

        // Read current bCaptureEveryFrame
        uint8_t* captureBitsPtr = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(captureComp) + 0x0232);
        bool wasCaptureEveryFrame = (*captureBitsPtr & 0x01) != 0;
        
        LOG_INFO("[ScopeNVG] ApproachC: " << scope.name
            << " bCaptureEveryFrame was " << (wasCaptureEveryFrame ? "true" : "false")
            << ", setting to true");

        // Enable bCaptureEveryFrame so the scope renders with our PP every frame
        *captureBitsPtr |= 0x01;

        // Also force an immediate CaptureScene() call
        try
        {
            Mod::ScopedProcessEventGuard guard;
            captureComp->CaptureScene();
            LOG_INFO("[ScopeNVG] ApproachC: CaptureScene() called successfully");
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachC: Exception calling CaptureScene()");
        }
    }

    void ScopeNVGSubsystem::RestoreApproachC_ForcedCapture(ScopeInfo& scope)
    {
        auto* captureComp = GetCaptureComponent(scope.actor);
        if (!captureComp) return;

        LOG_INFO("[ScopeNVG] ApproachC: Restoring bCaptureEveryFrame on " << scope.name
            << " to " << (scope.origCaptureEveryFrame ? "true" : "false"));

        // Restore original bCaptureEveryFrame state
        uint8_t* captureBitsPtr = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(captureComp) + 0x0232);
        
        if (scope.origCaptureEveryFrame)
            *captureBitsPtr |= 0x01;
        else
            *captureBitsPtr &= ~0x01;

        // Force one more capture with restored settings
        try
        {
            Mod::ScopedProcessEventGuard guard;
            captureComp->CaptureScene();
            LOG_INFO("[ScopeNVG] ApproachC: Restoration CaptureScene() called");
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] ApproachC: Exception calling CaptureScene() on restore");
        }
    }

    // =========================================================================
    // Combined apply/restore
    // =========================================================================

    void ScopeNVGSubsystem::ApplyAllApproaches(ScopeInfo& scope)
    {
        //ApplyApproachA_PostProcess(scope);
        ApplyApproachB_MaterialParams(scope);
        //ApplyApproachC_ForcedCapture(scope);
    }

    void ScopeNVGSubsystem::RestoreAllApproaches(ScopeInfo& scope)
    {
        //RestoreApproachC_ForcedCapture(scope);
        RestoreApproachB_MaterialParams(scope);
        //RestoreApproachA_PostProcess(scope);
    }

    // =========================================================================
    // Backup original state
    // =========================================================================

    void ScopeNVGSubsystem::BackupScopeState(ScopeInfo& scope)
    {
        if (scope.hadBackup) return;  // Already backed up

        auto* captureComp = GetCaptureComponent(scope.actor);
        if (!captureComp)
        {
            LOG_WARN("[ScopeNVG] BackupScopeState: no capture component on " << scope.name);
            return;
        }

        // Backup PP state
        scope.origPostProcessBlendWeight = captureComp->PostProcessBlendWeight;
        scope.origExposureBias = captureComp->PostProcessSettings.AutoExposureBias;
        
        // Backup bCaptureEveryFrame
        uint8_t captureBits = *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(captureComp) + 0x0232);
        scope.origCaptureEveryFrame = (captureBits & 0x01) != 0;

        // Log current ShowFlagSettings for diagnostics
        try
        {
            auto& flags = captureComp->ShowFlagSettings;
            LOG_INFO("[ScopeNVG] BackupScopeState: ShowFlagSettings has " << flags.Num() << " entries");
            
            for (int i = 0; i < flags.Num(); ++i)
            {
                auto& f = flags[i];
                const wchar_t* rawName = f.ShowFlagName.CStr();
                if (!rawName) continue;
                std::wstring wname(rawName);
                std::string name(wname.begin(), wname.end());
                LOG_INFO("[ScopeNVG] BackupScopeState:   ShowFlag[" << i << "]: " << name << "=" << (f.Enabled ? "true" : "false"));
            }
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] BackupScopeState: Exception reading ShowFlagSettings");
        }

        // Backup material BrightnessMult
        try
        {
            auto* matInst = scope.actor->LensMatInst;
            if (matInst)
            {
                Mod::ScopedProcessEventGuard guard;
                SDK::FName bmName = SDK::BasicFilesImpleUtils::StringToName(L"BrightnessMult");
                scope.origBrightnessMult = matInst->K2_GetScalarParameterValue(bmName);
                LOG_INFO("[ScopeNVG] BackupScopeState: BrightnessMult=" << scope.origBrightnessMult);
            }
            else
            {
                LOG_WARN("[ScopeNVG] BackupScopeState: LensMatInst is null, default BrightnessMult=1.0");
            }
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] BackupScopeState: Exception reading BrightnessMult");
        }

        scope.hadBackup = true;

        LOG_INFO("[ScopeNVG] BackupScopeState COMPLETE for " << scope.name
            << ": PPWeight=" << scope.origPostProcessBlendWeight
            << " ExposureBias=" << scope.origExposureBias
            << " CaptureEveryFrame=" << (scope.origCaptureEveryFrame ? "Y" : "N")
            << " BrightnessMult=" << scope.origBrightnessMult);
    }

    // =========================================================================
    // Diagnostic Logging
    // =========================================================================

    void ScopeNVGSubsystem::LogCaptureState(const std::string& prefix, SDK::USceneCaptureComponent2D* cc, const ScopeInfo& scope)
    {
        if (!cc) return;

        try
        {
            auto& pp = cc->PostProcessSettings;
            float ppWeight = cc->PostProcessBlendWeight;
            
            uint8_t captureBits = *reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(cc) + 0x0232);
            bool captureEveryFrame = (captureBits & 0x01) != 0;

            LOG_INFO("[ScopeNVG] " << prefix << " [" << scope.name << "]:"
                << " PPWeight=" << ppWeight
                << " bCaptureEveryFrame=" << (captureEveryFrame ? "Y" : "N")
                << " bOverride_ColorGain=" << (int)pp.bOverride_ColorGain
                << " bOverride_AutoExposureBias=" << (int)pp.bOverride_AutoExposureBias
                << " bOverride_SceneColorTint=" << (int)pp.bOverride_SceneColorTint);

            LOG_INFO("[ScopeNVG] " << prefix << " [" << scope.name << "] PP values:"
                << " ColorGain.Y(green)=" << pp.ColorGain.Y
                << " ExposureBias=" << pp.AutoExposureBias
                << " BloomIntensity=" << pp.BloomIntensity
                << " VignetteIntensity=" << pp.VignetteIntensity
                << " FilmGrainIntensity=" << pp.FilmGrainIntensity);

            LOG_INFO("[ScopeNVG] " << prefix << " [" << scope.name << "] Color:"
                << " SceneColorTint=("
                << pp.SceneColorTint.R << "," << pp.SceneColorTint.G << ","
                << pp.SceneColorTint.B << "," << pp.SceneColorTint.A << ")"
                << " ColorSat.Y=" << pp.ColorSaturation.Y
                << " ColorGamma.Y=" << pp.ColorGamma.Y);

            // Log ShowFlagSettings directly from member
            auto& flags = cc->ShowFlagSettings;
            std::ostringstream flagStr;
            for (int i = 0; i < flags.Num(); ++i)
            {
                const wchar_t* rawName = flags[i].ShowFlagName.CStr();
                std::string name = "?";
                if (rawName)
                {
                    std::wstring wname(rawName);
                    name = std::string(wname.begin(), wname.end());
                }
                flagStr << name << "=" << (flags[i].Enabled ? "1" : "0");
                if (i < flags.Num() - 1) flagStr << ", ";
            }
            LOG_INFO("[ScopeNVG] " << prefix << " [" << scope.name << "] ShowFlags: [" << flagStr.str() << "]");

            // Log material BrightnessMult
            auto* matInst = scope.actor->LensMatInst;
            if (matInst)
            {
                Mod::ScopedProcessEventGuard guard;
                SDK::FName bmName = SDK::BasicFilesImpleUtils::StringToName(L"BrightnessMult");
                float bm = matInst->K2_GetScalarParameterValue(bmName);
                LOG_INFO("[ScopeNVG] " << prefix << " [" << scope.name << "] Material: BrightnessMult=" << bm);
            }
        }
        catch (...)
        {
            LOG_ERROR("[ScopeNVG] " << prefix << ": Exception logging capture state for " << scope.name);
        }
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    bool ScopeNVGSubsystem::IsScopeValid(const ScopeInfo& scope) const
    {
        if (!scope.actor) return false;
        try { return SDK::UKismetSystemLibrary::IsValid(scope.actor); }
        catch (...) { return false; }
    }

    SDK::USceneCaptureComponent2D* ScopeNVGSubsystem::GetCaptureComponent(SDK::ABPA_Scope_C* scope) const
    {
        if (!scope) return nullptr;
        return scope->SceneCaptureComponent2D;
    }

    std::string ScopeNVGSubsystem::GetStatusReport() const
    {
        std::ostringstream oss;
        oss << "=== ScopeNVG Status ===\n";
        oss << "Scopes tracked: " << scopes_.size() << "\n";
        oss << "Auto-scan: " << (autoScan_.load() ? "ON" : "OFF") << "\n";
        oss << "PP Params: Green=" << greenGain_ << " Exposure=" << exposureBias_
            << " Grain=" << grainIntensity_ << " Bloom=" << bloomIntensity_ << "\n";

        for (int i = 0; i < static_cast<int>(scopes_.size()); ++i)
        {
            auto& s = scopes_[i];
            oss << "  [" << i << "] " << s.name << " (" << s.className << ")"
                << " NVG=" << (s.nvgEnabled ? "ON" : "OFF")
                << " Valid=" << (IsScopeValid(s) ? "Y" : "N")
                << "\n";
        }

        return oss.str();
    }

} // namespace Mod
