/*
AILEARNINGS
- GetPlayersInventory returns player items as TArray<URadiusItemDynamicData*>
- ItemType.TagName gives the FGameplayTag's FName which can be converted to string
- Attachments are stored directly in URadiusItemDynamicData::Attachments array
- Transform data is in DynamicTransform.DynamicLocation/DynamicRotation (not LocalTransform)
- Use UFLSpawn::SpawnItemByTypeTag with FItemConfiguration to spawn items
- FName cannot be constructed from const char* - must use SDK::BasicFilesImpleUtils::StringToName(wchar_t*)
- URadiusContainerSubsystem is a WORLD subsystem - use USubsystemBlueprintLibrary::GetWorldSubsystem()
- GetPlayersInventory() returns FLAT list of ALL player items, not hierarchical
- Parent-child relationships are tracked via ParentContainerUid field
- IsPlayerContainerID() checks if a container UID belongs to the player
- GetTopParentContainerID() finds the root container for any item
- GetAllPlayerItems() takes AActor* Player and fills TArray<ARadiusItemBase*>
- Container hierarchy: Player -> ChestRig/Backpack -> Pouches -> Magazines/Boxes -> Ammo
- Some inventory “container objects” returned by URadiusContainerSubsystem::GetContainerObject are UActorComponent holster components (no world transform); use GetOwner() to derive an anchor pose.
- Some inventory “container objects” returned by URadiusContainerSubsystem::GetContainerObject are UActorComponent holster components (no world transform); use GetOwner() to derive an anchor pose.
- UClass::GetFunction(ClassName, FuncName) only finds functions for an exact declaring class name; for unknown Blueprint-generated container classes (e.g. BPC_AS_Base_Molle_Vertical_S10) scan the class chain’s Children fields by function name instead.
- For holster/holder containers, prefer calling StartHolstering(ItemActor, RelativeTransform) with the captured local transform; URadiusContainerSubsystem::InstantHolsterActor can snap items to an unexpected transform (observed as warping modules across the map).
- For holster/holder containers, prefer calling StartHolstering/PutItemToContainer with the captured local transform; URadiusContainerSubsystem::InstantHolsterActor can snap items to an unexpected transform (observed as warping modules across the map).
- Weapon attachments often parent to `AttachSlot.*` containers; those container objects can be non-scene components where using owner-actor pose as a spawn anchor misplaces optics/lasers. For `AttachSlot.*` items, spawn near the player and rely on attach logic.
- Magazine/stackable ammo is represented as `stacked=` counts in loadouts; set `FItemConfiguration.StackAmount` when spawning or magazines will spawn empty.
*/

#include "LoadoutSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"

// These headers provide the parameter structs needed to call Blueprint functions via ProcessEvent
// without linking the corresponding *_functions.cpp translation units.
#include "..\\CppSDK\\SDK\\BPC_ItemHolster_parameters.hpp"
#include "..\\CppSDK\\SDK\\BP_HolderVolume_parameters.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <set>
#include <functional>
#include <cmath>

namespace fs = std::filesystem;

namespace Mod::Loadout
{
    // Helper to convert narrow string to wide string (for FName/FString creation)
    static std::wstring ToWideString(const std::string& str)
    {
        if (str.empty()) return L"";
        std::wstring wide(str.begin(), str.end());
        return wide;
    }
    
    static LoadoutSubsystem g_Instance;
    
    LoadoutSubsystem* LoadoutSubsystem::Get()
    {
        return &g_Instance;
    }
    
    void LoadoutSubsystem::Initialize()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        // Ensure loadouts directory exists
        std::string dir = GetLoadoutsDirectory();
        try
        {
            if (!fs::exists(dir))
            {
                fs::create_directories(dir);
                LOG_INFO("[Loadout] Created loadouts directory: " << dir);
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Failed to create loadouts directory: " << e.what());
        }
        
        initialized_ = true;
        LOG_INFO("[Loadout] LoadoutSubsystem initialized");
    }
    
    void LoadoutSubsystem::Shutdown()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        initialized_ = false;
        LOG_INFO("[Loadout] LoadoutSubsystem shutdown");
    }
    
    std::string LoadoutSubsystem::GetLoadoutsDirectory() const
    {
        return "loadouts";
    }
    
    std::string LoadoutSubsystem::GetLoadoutFilePath(const std::string& name) const
    {
        return GetLoadoutsDirectory() + "/" + name + ".loadout";
    }
    
    SDK::URadiusContainerSubsystem* LoadoutSubsystem::GetContainerSubsystem(SDK::UWorld* world) const
    {
        LOG_INFO("[Loadout] GetContainerSubsystem called, world=" << (void*)world);
        
        if (!world)
        {
            LOG_ERROR("[Loadout] GetContainerSubsystem: World is null");
            return nullptr;
        }
        
        // Log world info
        try
        {
            LOG_INFO("[Loadout] World class: " << world->Class->GetName());
        }
        catch (...)
        {
            LOG_WARN("[Loadout] Failed to get world class name");
        }
        
        // Use the proper subsystem retrieval method (same pattern as ArenaSubsystem, AISubsystem)
        SDK::URadiusContainerSubsystem* subsystem = nullptr;
        
        try
        {
            subsystem = static_cast<SDK::URadiusContainerSubsystem*>(
                SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
                    world, 
                    SDK::URadiusContainerSubsystem::StaticClass()
                )
            );
            
            LOG_INFO("[Loadout] GetWorldSubsystem returned: " << (void*)subsystem);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception in GetWorldSubsystem: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] Unknown exception in GetWorldSubsystem");
        }
        
        // Fallback: scan GObjects if GetWorldSubsystem fails
        if (!subsystem)
        {
            LOG_WARN("[Loadout] GetWorldSubsystem failed, falling back to GObjects scan");
            
            int scannedCount = 0;
            int containerSubsystemCount = 0;
            
            for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj) continue;
                scannedCount++;
                
                SDK::UClass* cls = obj->Class;
                if (!cls) continue;
                
                std::string className = cls->GetName();
                if (className == "RadiusContainerSubsystem")
                {
                    containerSubsystemCount++;
                    
                    // Try to validate this is the right one for our world
                    auto* candidate = static_cast<SDK::URadiusContainerSubsystem*>(obj);
                    
                    // Log the candidate
                    LOG_INFO("[Loadout] Found RadiusContainerSubsystem candidate #" << containerSubsystemCount 
                             << " at " << (void*)candidate);
                    
                    // Check if it's valid and has containers
                    try
                    {
                        auto containers = candidate->GetContainers();
                        LOG_INFO("[Loadout] Candidate has " << containers.Num() << " containers");
                        
                        // Accept first one with containers, or just first one
                        if (!subsystem || containers.Num() > 0)
                        {
                            subsystem = candidate;
                            if (containers.Num() > 0)
                            {
                                LOG_INFO("[Loadout] Selected this candidate (has containers)");
                                break;
                            }
                        }
                    }
                    catch (...)
                    {
                        LOG_WARN("[Loadout] Exception checking candidate, skipping");
                    }
                }
            }
            
            LOG_INFO("[Loadout] GObjects scan: scanned " << scannedCount 
                     << " objects, found " << containerSubsystemCount << " container subsystems");
        }
        
        if (!subsystem)
        {
            LOG_ERROR("[Loadout] Could not find RadiusContainerSubsystem by any method");
        }
        
        return subsystem;
    }
    
    std::string LoadoutSubsystem::CaptureLoadout(SDK::UWorld* world, const std::string& loadoutName)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        LOG_INFO("[Loadout] ========== CAPTURE LOADOUT START ==========");
        LOG_INFO("[Loadout] Loadout name: " << loadoutName);
        LOG_INFO("[Loadout] World: " << (void*)world);
        
        if (loadoutName.empty())
        {
            LOG_ERROR("[Loadout] Empty loadout name");
            return "Error: Loadout name cannot be empty";
        }
        
        if (!world)
        {
            LOG_ERROR("[Loadout] World is null");
            return "Error: World not ready";
        }
        
        // Get the player character for reference
        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        LOG_INFO("[Loadout] Player pawn: " << (void*)playerPawn);
        
        if (playerPawn)
        {
            try
            {
                LOG_INFO("[Loadout] Player class: " << playerPawn->Class->GetName());
                SDK::FVector loc = playerPawn->K2_GetActorLocation();
                LOG_INFO("[Loadout] Player location: (" << loc.X << ", " << loc.Y << ", " << loc.Z << ")");
            }
            catch (...) { LOG_WARN("[Loadout] Failed to log player details"); }
        }
        
        // Get the container subsystem
        SDK::URadiusContainerSubsystem* containerSubsystem = GetContainerSubsystem(world);
        if (!containerSubsystem)
        {
            LOG_ERROR("[Loadout] Container subsystem is null after GetContainerSubsystem");
            return "Error: Could not find container subsystem";
        }
        
        LOG_INFO("[Loadout] Container subsystem: " << (void*)containerSubsystem);
        
        // === METHOD 1: GetPlayersInventory() ===
        LOG_INFO("[Loadout] === METHOD 1: Trying GetPlayersInventory() ===");
        SDK::TArray<SDK::URadiusItemDynamicData*> playerItems;
        int method1Count = 0;
        
        try
        {
            playerItems = containerSubsystem->GetPlayersInventory();
            method1Count = playerItems.Num();
            LOG_INFO("[Loadout] GetPlayersInventory returned " << method1Count << " items");
            
            // Log first few items for debugging
            for (int i = 0; i < playerItems.Num() && i < 10; i++)
            {
                SDK::URadiusItemDynamicData* item = playerItems[i];
                if (item)
                {
                    try
                    {
                        std::string typeTag = item->ItemType.TagName.ToString();
                        std::string uid = item->InstanceUid.ToString();
                        std::string parentUid = item->ParentContainerUid.ToString();
                        LOG_INFO("[Loadout]   Item[" << i << "]: type=" << typeTag 
                                 << " uid=" << uid 
                                 << " parent=" << parentUid);
                    }
                    catch (...) { LOG_WARN("[Loadout]   Item[" << i << "]: failed to read"); }
                }
                else
                {
                    LOG_WARN("[Loadout]   Item[" << i << "]: null");
                }
            }
            if (method1Count > 10)
            {
                LOG_INFO("[Loadout]   ... and " << (method1Count - 10) << " more items");
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] GetPlayersInventory exception: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] GetPlayersInventory unknown exception");
        }
        
        // === METHOD 2: GetAllPlayerItems() (returns ARadiusItemBase actors) ===
        LOG_INFO("[Loadout] === METHOD 2: Trying GetAllPlayerItems() ===");
        int method2Count = 0;
        
        if (playerPawn)
        {
            try
            {
                SDK::TArray<SDK::ARadiusItemBase*> itemActors;
                bool success = containerSubsystem->GetAllPlayerItems(playerPawn, &itemActors);
                method2Count = itemActors.Num();
                LOG_INFO("[Loadout] GetAllPlayerItems returned " << success 
                         << ", count=" << method2Count);
                
                // Log first few for debugging
                for (int i = 0; i < itemActors.Num() && i < 5; i++)
                {
                    SDK::ARadiusItemBase* actor = itemActors[i];
                    if (actor)
                    {
                        try
                        {
                            LOG_INFO("[Loadout]   ItemActor[" << i << "]: " << actor->Class->GetName()
                                     << " at (" << actor->K2_GetActorLocation().X << "...)");
                        }
                        catch (...) { LOG_WARN("[Loadout]   ItemActor[" << i << "]: failed to read"); }
                    }
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("[Loadout] GetAllPlayerItems exception: " << e.what());
            }
            catch (...)
            {
                LOG_ERROR("[Loadout] GetAllPlayerItems unknown exception");
            }
        }
        else
        {
            LOG_WARN("[Loadout] Cannot call GetAllPlayerItems - no player pawn");
        }
        
        // === METHOD 3: Check container hierarchy ===
        LOG_INFO("[Loadout] === METHOD 3: Checking container hierarchy ===");
        
        try
        {
            auto containers = containerSubsystem->GetContainers();
            LOG_INFO("[Loadout] Total containers in subsystem: " << containers.Num());
            
            // Note: TMap iteration is tricky in this SDK, just log the count for now
            // The main diagnostic info comes from METHOD 1 and METHOD 2
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Container hierarchy check exception: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] Container hierarchy check unknown exception");
        }
        
        // === BUILD LOADOUT FROM COLLECTED ITEMS ===
        LOG_INFO("[Loadout] === BUILDING LOADOUT ===");
        
        if (method1Count == 0)
        {
            LOG_ERROR("[Loadout] No items found via GetPlayersInventory - capture failed");
            return "Error: No items found in player inventory. Check logs for details.";
        }
        
        // Create loadout data
        LoadoutData loadout;
        loadout.name = loadoutName;
        loadout.version = 1;
        
        // Generate timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;
        localtime_s(&tm_now, &time_t_now);
        std::ostringstream timestampStream;
        timestampStream << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
        loadout.timestamp = timestampStream.str();
        
        // Build a set of all item UIDs for parent-child relationship detection
        std::set<std::string> allItemUids;
        for (int i = 0; i < playerItems.Num(); i++)
        {
            SDK::URadiusItemDynamicData* itemData = playerItems[i];
            if (itemData)
            {
                try
                {
                    allItemUids.insert(itemData->InstanceUid.ToString());
                }
                catch (...) {}
            }
        }
        
        LOG_INFO("[Loadout] Built UID set with " << allItemUids.size() << " unique items");
        
        // === Build Map of UID -> Actor using the item list and subsystem lookup ===
        std::map<std::string, SDK::AActor*> itemActorMap;
        if (containerSubsystem)
        {
            for (int i = 0; i < playerItems.Num(); i++)
            {
                SDK::URadiusItemDynamicData* itemData = playerItems[i];
                if (itemData)
                {
                    try
                    {
                        std::string uid = itemData->InstanceUid.ToString();
                        // This lookup is efficient enough for capture
                        SDK::ARadiusItemBase* actor = containerSubsystem->GetItemByContainerID(itemData->InstanceUid);
                        if (actor)
                        {
                            itemActorMap[uid] = actor;
                        }
                    }
                    catch (...) {}
                }
            }
            LOG_INFO("[Loadout] Mapped " << itemActorMap.size() << " item actors from inventory list");
        }

        // Capture top-level items (items whose parent is NOT another inventory item)
        int topLevelCount = 0;
        int childCount = 0;
        
        for (int i = 0; i < playerItems.Num(); i++)
        {
            SDK::URadiusItemDynamicData* itemData = playerItems[i];
            if (!itemData)
            {
                continue;
            }
            
            try
            {
                std::string parentUid = itemData->ParentContainerUid.ToString();
                
                // An item is top-level if its parent UID is NOT in our item set
                // (meaning its parent is a player body slot, not another item)
                bool isTopLevel = (allItemUids.find(parentUid) == allItemUids.end());
                
                if (isTopLevel)
                {
                    topLevelCount++;
                    LoadoutItem item = CaptureItemData(itemData, containerSubsystem, itemActorMap);
                    loadout.items.push_back(item);
                    
                    int attachmentCount = item.attachments.size();
                    LOG_INFO("[Loadout] Captured Top-level item: " << item.itemTypeTag 
                             << " (UID: " << item.instanceUid.substr(0, 8) << "..."
                             << ", attachments: " << attachmentCount << ")");
                }
                else
                {
                    childCount++;
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("[Loadout] Exception capturing item: " << e.what());
            }
            catch (...)
            {
                LOG_ERROR("[Loadout] Unknown exception capturing item");
            }
        }
        
        LOG_INFO("[Loadout] Categorized: " << topLevelCount << " top-level, " << childCount << " child items");
        LOG_INFO("[Loadout] Captured " << loadout.items.size() << " items into loadout");
        
        // Save to file
        if (!SaveLoadoutToFile(loadout))
        {
            LOG_ERROR("[Loadout] Failed to save loadout file");
            return "Error: Failed to save loadout file";
        }
        
        std::ostringstream result;
        result << "Captured loadout '" << loadoutName << "' with " << loadout.items.size() << " items";
        
        LOG_INFO("[Loadout] ========== CAPTURE LOADOUT END ==========");
        LOG_INFO("[Loadout] Result: " << result.str());
        
        // Show in-game feedback
        ModFeedback::ShowMessage(
            (L"Loadout captured: " + std::wstring(loadoutName.begin(), loadoutName.end())).c_str(),
            3.0f,
            SDK::FLinearColor{0.0f, 1.0f, 0.5f, 1.0f});
        
        return result.str();
    }
    
    LoadoutItem LoadoutSubsystem::CaptureItemData(SDK::URadiusItemDynamicData* itemData, 
                                                SDK::URadiusContainerSubsystem* cs,
                                                const std::map<std::string, SDK::AActor*>& itemActors)
    {
        LoadoutItem item;
        
        if (!itemData)
        {
            return item;
        }
        
        // Item type tag
        SDK::FName tagName = itemData->ItemType.TagName;
        item.itemTypeTag = tagName.ToString();
        
        // Instance UID
        item.instanceUid = itemData->InstanceUid.ToString();
        
        // Parent container UID
        item.parentContainerUid = itemData->ParentContainerUid.ToString();
        
        // Durability
        item.durability = itemData->Durability;
        
        // Transform
        // Default to DynamicTransform (likely World Space)
        SDK::FVector loc;
        SDK::FRotator rot;
        SDK::FVector scale{1.0f, 1.0f, 1.0f};
        
        if (itemData->DynamicTransform.bInitializedWithValues)
        {
            loc.X = itemData->DynamicTransform.DynamicLocation.X;
            loc.Y = itemData->DynamicTransform.DynamicLocation.Y;
            loc.Z = itemData->DynamicTransform.DynamicLocation.Z;
            
            rot.Pitch = static_cast<double>(itemData->DynamicTransform.DynamicRotation.X);
            rot.Yaw = static_cast<double>(itemData->DynamicTransform.DynamicRotation.Y);
            rot.Roll = static_cast<double>(itemData->DynamicTransform.DynamicRotation.Z);
        }

        // Try to get actual relative transform from Actor if available
        try
        {
            auto it = itemActors.find(item.instanceUid);
            if (it != itemActors.end() && it->second)
            {
                SDK::AActor* actor = it->second;
                SDK::USceneComponent* root = actor->RootComponent;
                if (root)
                {
                    // Check if attached
                    SDK::USceneComponent* attachParent = root->AttachParent;
                    if (attachParent)
                    {
                        // It is attached, so RelativeLocation/Rotation are valid relative to parent
                        loc = root->RelativeLocation;
                        rot = root->RelativeRotation;
                        scale = root->RelativeScale3D;
                        LOG_INFO("[Loadout] Captured RELATIVE transform for " << item.itemTypeTag 
                                 << ": " << loc.X << "," << loc.Y << "," << loc.Z);
                    }
                    else
                    {
                        // Not attached - it is in world space.
                        // However, loadouts are practically useless if storing absolute world coordinates
                        // unless we are creating a level editor. For player equipment, we usually want relative.
                        // If we are falling through here for equipment, something is wrong with attachment detection
                        // or the item is floating.
                        
                        // We will log a warning but still save it.
                        // Maybe we should try to calculate relative to player if possible?
                        // No, let's stick to what the engine reports.
                        LOG_WARN("[Loadout] Item " << item.itemTypeTag << " (ID: " << item.instanceUid.substr(0, 8) 
                                 << ") has actor but NO attach parent. Using World/Dynamic transform: "
                                 << loc.X << "," << loc.Y << "," << loc.Z);
                    }
                }
            }
        }
        catch (...)
        {
            LOG_WARN("[Loadout] Failed to access actor transform for " << item.instanceUid);
        }
        
        item.transform.posX = loc.X;
        item.transform.posY = loc.Y;
        item.transform.posZ = loc.Z;
        item.transform.rotPitch = rot.Pitch;
        item.transform.rotYaw = rot.Yaw;
        item.transform.rotRoll = rot.Roll;
        item.transform.scaleX = scale.X;
        item.transform.scaleY = scale.Y;
        item.transform.scaleZ = scale.Z;
        
        // Stacked items (for magazines, ammo containers)
        try
        {
            SDK::TArray<SDK::FStackedItem> stacked = itemData->GetStackedItems();
            for (int i = 0; i < stacked.Num(); i++)
            {
                const SDK::FStackedItem& stackedItem = stacked[i];
                std::string tag = stackedItem.ItemTag.TagName.ToString();
                // Each FStackedItem is one item (count is implicit from array size)
                // But we group by tag type
                bool found = false;
                for (auto& existing : item.stackedItems)
                {
                    if (existing.first == tag)
                    {
                        existing.second++;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    item.stackedItems.push_back({tag, 1});
                }
            }
        }
        catch (...)
        {
            // Stacked items may not be available for all items
        }
        
        // Additional data
        for (int i = 0; i < itemData->AdditionalData.Num(); i++)
        {
            const SDK::FString& entry = itemData->AdditionalData[i];
            std::string entryStr = entry.ToString();
            // Parse key=value format
            size_t eqPos = entryStr.find('=');
            if (eqPos != std::string::npos)
            {
                std::string key = entryStr.substr(0, eqPos);
                std::string value = entryStr.substr(eqPos + 1);
                item.additionalData[key] = value;
            }
        }
        
        // Attachments (recursive)
        for (int i = 0; i < itemData->Attachments.Num(); i++)
        {
            SDK::URadiusItemDynamicData* attachment = itemData->Attachments[i];
            if (attachment)
            {
                // Recursive pass of the same map
                LoadoutItem attachmentItem = CaptureItemData(attachment, cs, itemActors);
                item.attachments.push_back(attachmentItem);
            }
        }
        
        return item;
    }
    
    std::string LoadoutSubsystem::SerializeLoadout(const LoadoutData& loadout) const
    {
        std::ostringstream ss;
        
        // Header
        ss << "[LOADOUT]\n";
        ss << "name=" << loadout.name << "\n";
        ss << "timestamp=" << loadout.timestamp << "\n";
        ss << "version=" << loadout.version << "\n";
        ss << "item_count=" << loadout.items.size() << "\n";
        ss << "\n";
        
        // Items
        for (const auto& item : loadout.items)
        {
            ss << SerializeItem(item, 0);
        }
        
        return ss.str();
    }
    
    std::string LoadoutSubsystem::SerializeItem(const LoadoutItem& item, int depth) const
    {
        std::ostringstream ss;
        std::string indent(depth * 2, ' ');
        
        ss << indent << "[ITEM]\n";
        ss << indent << "type=" << item.itemTypeTag << "\n";
        ss << indent << "uid=" << item.instanceUid << "\n";
        ss << indent << "parent=" << item.parentContainerUid << "\n";
        ss << indent << "durability=" << item.durability << "\n";
        ss << indent << "transform=" 
           << item.transform.posX << "," << item.transform.posY << "," << item.transform.posZ << "|"
           << item.transform.rotPitch << "," << item.transform.rotYaw << "," << item.transform.rotRoll << "|"
           << item.transform.scaleX << "," << item.transform.scaleY << "," << item.transform.scaleZ << "\n";
        
        // Stacked items
        if (!item.stackedItems.empty())
        {
            ss << indent << "stacked_count=" << item.stackedItems.size() << "\n";
            for (const auto& stacked : item.stackedItems)
            {
                ss << indent << "stacked=" << stacked.first << ":" << stacked.second << "\n";
            }
        }
        
        // Additional data
        for (const auto& kvp : item.additionalData)
        {
            ss << indent << "data_" << kvp.first << "=" << kvp.second << "\n";
        }
        
        // Attachments
        if (!item.attachments.empty())
        {
            ss << indent << "attachment_count=" << item.attachments.size() << "\n";
            for (const auto& attachment : item.attachments)
            {
                ss << SerializeItem(attachment, depth + 1);
            }
        }
        
        ss << indent << "[/ITEM]\n";
        ss << "\n";
        
        return ss.str();
    }
    
    bool LoadoutSubsystem::SaveLoadoutToFile(const LoadoutData& loadout)
    {
        std::string filepath = GetLoadoutFilePath(loadout.name);
        
        try
        {
            std::ofstream file(filepath, std::ios::out | std::ios::trunc);
            if (!file.is_open())
            {
                LOG_ERROR("[Loadout] Failed to open file for writing: " << filepath);
                return false;
            }
            
            std::string content = SerializeLoadout(loadout);
            file << content;
            file.close();
            
            LOG_INFO("[Loadout] Saved loadout to: " << filepath);
            return true;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception saving loadout: " << e.what());
            return false;
        }
    }
    
    bool LoadoutSubsystem::LoadLoadoutFromFile(const std::string& name, LoadoutData& outLoadout)
    {
        std::string filepath = GetLoadoutFilePath(name);
        
        try
        {
            std::ifstream file(filepath);
            if (!file.is_open())
            {
                LOG_ERROR("[Loadout] Failed to open file for reading: " << filepath);
                return false;
            }
            
            std::ostringstream contentStream;
            contentStream << file.rdbuf();
            file.close();
            
            std::string content = contentStream.str();
            return DeserializeLoadout(content, outLoadout);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception loading loadout: " << e.what());
            return false;
        }
    }
    
    bool LoadoutSubsystem::DeserializeLoadout(const std::string& content, LoadoutData& outLoadout)
    {
        std::istringstream stream(content);
        std::string line;
        
        // Parse state
        bool inLoadout = false;
        std::vector<LoadoutItem*> itemStack; // For handling nested [ITEM] blocks
        
        while (std::getline(stream, line))
        {
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos)
            {
                continue; // Empty line
            }
            line = line.substr(start);
            
            // Check for section markers
            if (line == "[LOADOUT]")
            {
                inLoadout = true;
                continue;
            }
            
            if (line == "[ITEM]")
            {
                // Start a new item
                if (itemStack.empty())
                {
                    // Top-level item
                    outLoadout.items.push_back(LoadoutItem{});
                    itemStack.push_back(&outLoadout.items.back());
                }
                else
                {
                    // Nested item (attachment)
                    LoadoutItem* parent = itemStack.back();
                    parent->attachments.push_back(LoadoutItem{});
                    itemStack.push_back(&parent->attachments.back());
                }
                continue;
            }
            
            if (line == "[/ITEM]")
            {
                if (!itemStack.empty())
                {
                    itemStack.pop_back();
                }
                continue;
            }
            
            // Parse key=value
            size_t eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }
            
            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);
            
            if (inLoadout && itemStack.empty())
            {
                // Loadout header values
                if (key == "name") outLoadout.name = value;
                else if (key == "timestamp") outLoadout.timestamp = value;
                else if (key == "version") outLoadout.version = std::stoi(value);
            }
            else if (!itemStack.empty())
            {
                // Item values
                LoadoutItem* currentItem = itemStack.back();
                
                if (key == "type") currentItem->itemTypeTag = value;
                else if (key == "uid") currentItem->instanceUid = value;
                else if (key == "parent") currentItem->parentContainerUid = value;
                else if (key == "durability") currentItem->durability = std::stof(value);
                else if (key == "transform")
                {
                    // Parse transform: posX,posY,posZ|rotPitch,rotYaw,rotRoll|scaleX,scaleY,scaleZ
                    std::istringstream transformStream(value);
                    std::string posStr, rotStr, scaleStr;
                    std::getline(transformStream, posStr, '|');
                    std::getline(transformStream, rotStr, '|');
                    std::getline(transformStream, scaleStr, '|');
                    
                    sscanf_s(posStr.c_str(), "%f,%f,%f", 
                             &currentItem->transform.posX, 
                             &currentItem->transform.posY, 
                             &currentItem->transform.posZ);
                    sscanf_s(rotStr.c_str(), "%f,%f,%f", 
                             &currentItem->transform.rotPitch, 
                             &currentItem->transform.rotYaw, 
                             &currentItem->transform.rotRoll);
                    sscanf_s(scaleStr.c_str(), "%f,%f,%f", 
                             &currentItem->transform.scaleX, 
                             &currentItem->transform.scaleY, 
                             &currentItem->transform.scaleZ);
                }
                else if (key == "stacked")
                {
                    // Parse stacked: tag:count
                    size_t colonPos = value.find(':');
                    if (colonPos != std::string::npos)
                    {
                        std::string tag = value.substr(0, colonPos);
                        int count = std::stoi(value.substr(colonPos + 1));
                        currentItem->stackedItems.push_back({tag, count});
                    }
                }
                else if (key.substr(0, 5) == "data_")
                {
                    // Additional data
                    std::string dataKey = key.substr(5);
                    currentItem->additionalData[dataKey] = value;
                }
            }
        }
        
        return true;
    }
    
    std::string LoadoutSubsystem::ListLoadouts() const
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        std::string dir = GetLoadoutsDirectory();
        std::ostringstream result;
        result << "Available loadouts:\n";
        
        int count = 0;
        try
        {
            if (fs::exists(dir) && fs::is_directory(dir))
            {
                for (const auto& entry : fs::directory_iterator(dir))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".loadout")
                    {
                        std::string name = entry.path().stem().string();
                        result << "  - " << name;
                        
                        // Mark selected loadout
                        if (name == selectedLoadout_)
                        {
                            result << " [SELECTED]";
                        }
                        
                        result << "\n";
                        count++;
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            result << "Error reading directory: " << e.what() << "\n";
        }
        
        if (count == 0)
        {
            result << "  (none)\n";
        }
        
        result << "Total: " << count << " loadout(s)";
        
        return result.str();
    }
    
    std::string LoadoutSubsystem::GetSelectedLoadout() const
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return selectedLoadout_;
    }
    
    void LoadoutSubsystem::SetSelectedLoadout(const std::string& name)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        selectedLoadout_ = name;
        LOG_INFO("[Loadout] Selected loadout: " << name);
    }
    
    bool LoadoutSubsystem::LoadoutExists(const std::string& name) const
    {
        std::string filepath = GetLoadoutFilePath(name);
        return fs::exists(filepath);
    }
    
    bool LoadoutSubsystem::BackupCurrentLoadout(SDK::UWorld* world)
    {
        LOG_INFO("[Loadout] Backing up current loadout...");
        
        // Use a special name for the backup
        std::string result = CaptureLoadout(world, "_backup");
        
        bool success = result.find("Error") == std::string::npos;
        if (success)
        {
            LOG_INFO("[Loadout] Backup successful");
        }
        else
        {
            LOG_ERROR("[Loadout] Backup failed: " << result);
        }
        
        return success;
    }
    
    int LoadoutSubsystem::ClearPlayerLoadout(SDK::UWorld* world)
    {
        LOG_INFO("[Loadout] Clearing player loadout...");
        
        if (!world)
        {
            LOG_ERROR("[Loadout] ClearPlayerLoadout: World is null");
            return 0;
        }
        
        SDK::URadiusContainerSubsystem* containerSubsystem = GetContainerSubsystem(world);
        if (!containerSubsystem)
        {
            LOG_ERROR("[Loadout] ClearPlayerLoadout: Container subsystem not found");
            return 0;
        }
        
        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        if (!playerPawn)
        {
            LOG_ERROR("[Loadout] ClearPlayerLoadout: Player pawn not found");
            return 0;
        }
        
        int droppedCount = 0;
        
        // Get all player items
        SDK::TArray<SDK::ARadiusItemBase*> playerItems;
        if (containerSubsystem->GetAllPlayerItems(playerPawn, &playerItems))
        {
            LOG_INFO("[Loadout] Found " << playerItems.Num() << " items to clear");
            
            // Destroy each item
            for (int i = 0; i < playerItems.Num(); i++)
            {
                SDK::ARadiusItemBase* item = playerItems[i];
                if (item)
                {
                    try
                    {
                        // Destroy the actor
                        item->K2_DestroyActor();
                        droppedCount++;
                    }
                    catch (...)
                    {
                        LOG_WARN("[Loadout] Failed to destroy item at index " << i);
                    }
                }
            }
        }
        
        LOG_INFO("[Loadout] Cleared " << droppedCount << " items");
        return droppedCount;
    }
    
    // ---------------------------------------------------------------------------
    // ParseParentUid
    //
    // Splits a parent container UID into (baseItemUid, slotSuffix).
    //
    // Examples:
    //   "Item.Equipment.Vests.Modular.Classic.Sand-4-AttachSlot.ModularVest.Slot18"
    //     -> {"Item.Equipment.Vests.Modular.Classic.Sand-4", "AttachSlot.ModularVest.Slot18"}
    //
    //   "Item.Magazine.AK-74.Red-5922-AttachSlot.Magazine"
    //     -> {"Item.Magazine.AK-74.Red-5922", "AttachSlot.Magazine"}
    //
    //   "Player-Holster.Player.Vest"
    //     -> {"", "Player-Holster.Player.Vest"}   (player slot, no base item)
    // ---------------------------------------------------------------------------
    static std::pair<std::string, std::string> ParseParentUid(const std::string& parentUid)
    {
        // Player slot UIDs start with "Player-"
        if (parentUid.size() >= 7 && parentUid.substr(0, 7) == "Player-")
        {
            return {"", parentUid};
        }

        // Scan for the RIGHTMOST "-digits-" pattern.
        // The split point is the hyphen that follows the digit run.
        // e.g. "Foo-4-Bar" -> split after "-4", at the second hyphen.
        size_t splitPos = std::string::npos;

        for (size_t i = 0; i < parentUid.size(); i++)
        {
            if (parentUid[i] == '-')
            {
                // Check how many digits follow
                size_t j = i + 1;
                while (j < parentUid.size() && std::isdigit((unsigned char)parentUid[j]))
                {
                    j++;
                }
                // We need at least one digit, followed by another '-'
                if (j > i + 1 && j < parentUid.size() && parentUid[j] == '-')
                {
                    splitPos = j; // The second hyphen is the split boundary
                }
            }
        }

        if (splitPos != std::string::npos)
        {
            return {
                parentUid.substr(0, splitPos),      // e.g. "Item.Foo-4"
                parentUid.substr(splitPos + 1)       // e.g. "AttachSlot.Bar"
            };
        }

        // Couldn't find the pattern - treat the whole thing as a base UID
        return {parentUid, ""};
    }

    // ---------------------------------------------------------------------------
    // SnapshotInventory  -  returns the set of all current player item UIDs.
    // ---------------------------------------------------------------------------
    static std::set<std::string> SnapshotInventory(SDK::URadiusContainerSubsystem* cs)
    {
        std::set<std::string> snap;
        try
        {
            auto items = cs->GetPlayersInventory();
            for (int i = 0; i < items.Num(); i++)
            {
                if (items[i])
                {
                    snap.insert(items[i]->InstanceUid.ToString());
                }
            }
        }
        catch (...) {}
        return snap;
    }

    // ---------------------------------------------------------------------------
    // SpawnItem  -  spawns an item in the world (does NOT attach it).
    //              Prefer spawning near the parent container (so physics/
    //              registration behaves more like a natural placement), with
    //              a player-forward fallback when we can't resolve a parent.
    // ---------------------------------------------------------------------------
    static SDK::FVector RotateVectorByRotator(const SDK::FVector& v, const SDK::FRotator& r)
    {
        // Unreal uses degrees. Approx rotation order: Yaw(Z) -> Pitch(Y) -> Roll(X)
        const double degToRad = 3.14159265358979323846 / 180.0;
        const double yaw = r.Yaw * degToRad;
        const double pitch = r.Pitch * degToRad;
        const double roll = r.Roll * degToRad;

        const double cy = std::cos(yaw);
        const double sy = std::sin(yaw);
        const double cp = std::cos(pitch);
        const double sp = std::sin(pitch);
        const double cr = std::cos(roll);
        const double sr = std::sin(roll);

        // Yaw (Z)
        const double x1 = v.X * cy - v.Y * sy;
        const double y1 = v.X * sy + v.Y * cy;
        const double z1 = v.Z;

        // Pitch (Y)
        const double x2 = x1 * cp + z1 * sp;
        const double y2 = y1;
        const double z2 = -x1 * sp + z1 * cp;

        // Roll (X)
        const double x3 = x2;
        const double y3 = y2 * cr - z2 * sr;
        const double z3 = y2 * sr + z2 * cr;

        SDK::FVector out;
        out.X = static_cast<float>(x3);
        out.Y = static_cast<float>(y3);
        out.Z = static_cast<float>(z3);
        return out;
    }

    static bool TryGetObjectWorldPose(SDK::UObject* obj, SDK::FVector& outLoc, SDK::FRotator& outRot)
    {
        if (!obj)
        {
            return false;
        }

        // Many container "objects" are scene components.
        try
        {
            if (obj->IsA(SDK::USceneComponent::StaticClass()))
            {
                auto* comp = static_cast<SDK::USceneComponent*>(obj);
                outLoc = comp->K2_GetComponentLocation();
                outRot = comp->K2_GetComponentRotation();
                return true;
            }
        }
        catch (...) {}

        // Some containers are holster components (UActorComponent), which don't have a transform
        // but their owner actor does.
        try
        {
            if (obj->IsA(SDK::UActorComponent::StaticClass()))
            {
                auto* comp = static_cast<SDK::UActorComponent*>(obj);
                SDK::AActor* owner = comp->GetOwner();
                if (owner)
                {
                    outLoc = owner->K2_GetActorLocation();
                    outRot = owner->K2_GetActorRotation();
                    return true;
                }
            }
        }
        catch (...) {}

        // Some may be actors.
        try
        {
            if (obj->IsA(SDK::AActor::StaticClass()))
            {
                auto* actor = static_cast<SDK::AActor*>(obj);
                outLoc = actor->K2_GetActorLocation();
                outRot = actor->K2_GetActorRotation();
                return true;
            }
        }
        catch (...) {}

        return false;
    }

    static SDK::UFunction* FindFunctionByNameInClassChain(SDK::UClass* cls, const char* funcName)
    {
        if (!cls || !funcName)
            return nullptr;

        for (SDK::UStruct* s = cls; s; s = s->SuperStruct)
        {
            for (SDK::UField* f = s->Children; f; f = f->Next)
            {
                if (!f)
                    continue;
                if (!f->HasTypeFlag(SDK::EClassCastFlags::Function))
                    continue;
                if (f->GetName() == funcName)
                    return static_cast<SDK::UFunction*>(f);
            }
        }
        return nullptr;
    }

    static bool TryStartHolsteringWithRelativeTransform(SDK::UObject* container, SDK::AActor* actor, const SDK::FTransform& relativeTransform)
    {
        if (!container || !actor || !container->Class)
        {
            return false;
        }

        SDK::UFunction* fn = nullptr;
        try
        {
            fn = FindFunctionByNameInClassChain(container->Class, "StartHolstering");
        }
        catch (...) { fn = nullptr; }

        if (!fn)
        {
            return false;
        }

        // Generic parms layout for StartHolstering(AActor* ItemActor, const FTransform& RelativeTransform)
        // Matches the Dumper-7 generated params for known holster implementations.
        struct GenericStartHolsteringParms
        {
            SDK::AActor* ItemActor;
            uint8_t Pad_8[0x8];
            SDK::FTransform RelativeTransform;
        };

        try
        {
            GenericStartHolsteringParms parms{};
            parms.ItemActor = actor;
            parms.RelativeTransform = relativeTransform;
            container->ProcessEvent(fn, &parms);
            return true;
        }
        catch (...)
        {
            return false;
        }

        return false;
    }

    static bool TryInstantHolsterWithRelativeTransform(SDK::UObject* container, SDK::AActor* actor, SDK::FTransform& inOutRelativeTransform)
    {
        if (!container || !actor || !container->Class)
            return false;

        SDK::UFunction* fn = nullptr;
        try { fn = FindFunctionByNameInClassChain(container->Class, "InstantHolsterActor"); }
        catch (...) { fn = nullptr; }

        if (!fn)
            return false;

        // Layout matches ItemContainerInterface_InstantHolsterActor (Assertions.inl): size 0x70, RelativeTransform at 0x10
        struct Parms
        {
            SDK::AActor* ItemActor;
            uint8_t Pad_8[0x8];
            SDK::FTransform RelativeTransform;
        };
        static_assert(sizeof(Parms) == 0x70, "InstantHolsterActor parms size mismatch");

        try
        {
            Parms parms{};
            parms.ItemActor = actor;
            parms.RelativeTransform = inOutRelativeTransform;
            container->ProcessEvent(fn, &parms);
            inOutRelativeTransform = parms.RelativeTransform;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool TryPutItemToContainerWithRelativeTransform(SDK::UObject* container, SDK::AActor* actor, SDK::FTransform& inOutRelativeTransform)
    {
        if (!container || !actor || !container->Class)
            return false;

        SDK::UFunction* fn = nullptr;
        try { fn = FindFunctionByNameInClassChain(container->Class, "PutItemToContainer"); }
        catch (...) { fn = nullptr; }

        if (!fn)
            return false;

        // Layout matches ItemContainerInterface_PutItemToContainer (Assertions.inl): size 0x80, ReturnValue at 0x70
        struct Parms
        {
            SDK::AActor* ItemActor;
            uint8_t Pad_8[0x8];
            SDK::FTransform RelativeTransform;
            bool ReturnValue;
            uint8_t Pad_71[0xF];
        };
        static_assert(sizeof(Parms) == 0x80, "PutItemToContainer parms size mismatch");

        try
        {
            Parms parms{};
            parms.ItemActor = actor;
            parms.RelativeTransform = inOutRelativeTransform;
            parms.ReturnValue = false;
            container->ProcessEvent(fn, &parms);
            inOutRelativeTransform = parms.RelativeTransform;
            return parms.ReturnValue;
        }
        catch (...)
        {
            return false;
        }
    }

    static SDK::AActor* SpawnItem(SDK::UWorld* world, const LoadoutItem& item, SDK::UObject* spawnNear)
    {
        if (item.itemTypeTag.empty())
        {
            LOG_ERROR("[Loadout] SpawnItem: Empty type tag");
            return nullptr;
        }

        std::wstring wideTag = ToWideString(item.itemTypeTag);
        SDK::FGameplayTag typeTag;
        typeTag.TagName = SDK::BasicFilesImpleUtils::StringToName(wideTag.c_str());

        SDK::FVector anchorLoc{0, 0, 0};
        SDK::FRotator anchorRot{0, 0, 0};
        bool haveAnchor = TryGetObjectWorldPose(spawnNear, anchorLoc, anchorRot);

        if (!haveAnchor)
        {
            SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
            if (playerPawn)
            {
                anchorLoc = playerPawn->K2_GetActorLocation();
                anchorRot = playerPawn->K2_GetActorRotation();
            }
        }

        // item.transform is LOCAL to its parent container.
        SDK::FVector localPos(item.transform.posX, item.transform.posY, item.transform.posZ);
        SDK::FRotator localRot(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll);
        SDK::FVector localScale(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ);

        SDK::FVector worldLoc = anchorLoc;
        SDK::FRotator worldRot = anchorRot;
        LOG_INFO("[Loadout] Spawning item '" << item.itemTypeTag << "' near " 
                 << (spawnNear ? spawnNear->GetName() : "null") 
                 << " with local offset " << localPos.X << "," << localPos.Y << "," << localPos.Z);
        LOG_INFO("[Loadout] Anchor location: " << anchorLoc.X << "," << anchorLoc.Y << "," << anchorLoc.Z
                 << " rotation: " << anchorRot.Pitch << "," << anchorRot.Yaw << "," << anchorRot.Roll);


        if (haveAnchor)
        {
            LOG_INFO("[Loadout] Applying local offset to anchor...");
            // Approximate initial world placement at parent + local offset.
            SDK::FVector rotatedOffset = RotateVectorByRotator(localPos, anchorRot);
            worldLoc.X += rotatedOffset.X;
            worldLoc.Y += rotatedOffset.Y;
            worldLoc.Z += rotatedOffset.Z;

            worldRot.Pitch += localRot.Pitch;
            worldRot.Yaw += localRot.Yaw;
            worldRot.Roll += localRot.Roll;
        }
        else
        {
            LOG_INFO("[Loadout] No anchor found, using default world location...");
        }

        SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(worldLoc, worldRot, localScale);
        LOG_INFO("[Loadout] Spawn transform location: " << spawnTransform.Translation.X << "," << spawnTransform.Translation.Y << "," << spawnTransform.Translation.Z
            << " rotation: " << spawnTransform.Rotation.X << "," << spawnTransform.Rotation.Y << "," << spawnTransform.Rotation.Z << "," << spawnTransform.Rotation.W);

        SDK::FItemConfiguration cfg{};
        cfg.bShopItem = false;
        cfg.StartDurabilityRatio = item.durability;
        int stackAmount = 0;
        for (const auto& kv : item.stackedItems)
        {
            if (kv.second > 0)
                stackAmount += kv.second;
        }
        cfg.StackAmount = (stackAmount > 0) ? stackAmount : 1;

        try
        {
            SDK::AActor* actor = SDK::UFLSpawn::SpawnItemByTypeTag(
                world, typeTag, spawnTransform, cfg, true);

            if (actor)
            {
                LOG_INFO("[Loadout] Spawned: " << actor->GetName()
                         << "  type=" << item.itemTypeTag);
            }
            else
            {
                LOG_ERROR("[Loadout] SpawnItemByTypeTag returned null for: " << item.itemTypeTag);
            }
            return actor;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception spawning " << item.itemTypeTag << ": " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] Unknown exception spawning " << item.itemTypeTag);
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // AttachToContainer  -  tries InstantHolsterActor then PutItemToContainer.
    // ---------------------------------------------------------------------------
    // Attach an actor to a container, optionally providing a relative transform.
    // If the container implements IItemContainerInterface the 3‑arg overload is
    // used to ensure the transform is applied; otherwise the world-subsystem
    // fallback is attempted and ignores the transform.
    static bool AttachToContainer(SDK::URadiusContainerSubsystem* cs,
                                  SDK::UObject* container,
                                  SDK::AActor* actor,
                                  const std::string& label,
                                  const SDK::FTransform* desiredRelativeTransform,
                                  SDK::UWorld* world,
                                  bool preferPutFirst,
                                  bool forbidInstantHolster)
    {
        if (!container || !actor)
        {
            LOG_WARN("[Loadout] AttachToContainer: null container or actor for " << label);
            return false;
        }

        const std::string clsName = (container && container->Class) ? container->Class->GetName() : std::string("<no-class>");
        std::string clsLower = clsName;
        std::transform(clsLower.begin(), clsLower.end(), clsLower.begin(), ::tolower);

        // Prefer container-native holstering with an explicit relative transform when available.
        // This is especially important for modular vest/backpack attachment slots, where
        // URadiusContainerSubsystem::InstantHolsterActor has been observed to warp items.
        if (desiredRelativeTransform)
        {
            try
            {
                // Some containers (notably AttachSlot.*) must NOT use InstantHolsterActor.
                // Best-effort attach: PutItemToContainer + then apply captured relative transform.
                if (preferPutFirst)
                {
                    LOG_INFO("[Loadout] AttachToContainer: preferPutFirst enabled for " << label << " (class=" << clsName << ")");
                    bool ok = false;
                    try { ok = cs->PutItemToContainer(container, actor); } catch (...) { ok = false; }
                    if (ok)
                    {
                        SDK::FHitResult hit{};
                        actor->K2_SetActorRelativeTransform(*desiredRelativeTransform, false, &hit, true);
                        LOG_INFO("[Loadout] Attached via PutItemToContainer + SetRelativeTransform: " << label);
                        return true;
                    }
                    LOG_WARN("[Loadout] preferPutFirst: PutItemToContainer returned false for " << label);
                }

                // Molle slots: InstantHolsterActor is the warp source; prefer PutItemToContainer first.
                if (clsLower.find("molle") != std::string::npos)
                {
                    LOG_INFO("[Loadout] AttachToContainer: molle container detected (" << clsName << "), trying PutItemToContainer first for " << label);
                    bool ok = false;
                    try
                    {
                        ok = cs->PutItemToContainer(container, actor);
                    }
                    catch (...) { ok = false; }

                    if (ok)
                    {
                        // Now that we're attached, apply the captured relative transform.
                        SDK::FHitResult hit{};
                        actor->K2_SetActorRelativeTransform(*desiredRelativeTransform, false, &hit, true);
                        LOG_INFO("[Loadout] Attached via PutItemToContainer + SetRelativeTransform: " << label);
                        return true;
                    }
                    LOG_WARN("[Loadout] PutItemToContainer failed on molle container; will try other paths: " << label);
                }

                LOG_INFO("[Loadout] AttachToContainer: trying StartHolstering on container class=" << clsName << " for " << label);

                if (TryStartHolsteringWithRelativeTransform(container, actor, *desiredRelativeTransform))
                {
                    LOG_INFO("[Loadout] Attached via StartHolstering (relative transform): " << label);
                    return true;
                }

                // Next best: container-local InstantHolsterActor(ItemActor, RelativeTransform*)
                SDK::FTransform rel = *desiredRelativeTransform;
                LOG_INFO("[Loadout] AttachToContainer: trying container.InstantHolsterActor(rel) for " << label);
                if (TryInstantHolsterWithRelativeTransform(container, actor, rel))
                {
                    LOG_INFO("[Loadout] Attached via container.InstantHolsterActor(rel): " << label);
                    return true;
                }

                // Next: container-local PutItemToContainer(ItemActor, RelativeTransform*)
                rel = *desiredRelativeTransform;
                LOG_INFO("[Loadout] AttachToContainer: trying container.PutItemToContainer(rel) for " << label);
                if (TryPutItemToContainerWithRelativeTransform(container, actor, rel))
                {
                    LOG_INFO("[Loadout] Attached via container.PutItemToContainer(rel): " << label);
                    return true;
                }

                LOG_INFO("[Loadout] AttachToContainer: no container-local holster functions worked; will fall back to subsystem attach: " << label);
            }
            catch (...)
            {
                LOG_WARN("[Loadout] StartHolstering ProcessEvent threw for " << label);
            }
        }

        if (forbidInstantHolster)
        {
            // Critical safety: for some attachment-slot containers InstantHolsterActor teleports the item
            // to a different holster/container far away, causing extreme FPS drops.
            LOG_ERROR("[Loadout] AttachToContainer: forbidInstantHolster active; skipping InstantHolsterActor for " << label << " (class=" << clsName << ")");
            try
            {
                bool ok = cs->PutItemToContainer(container, actor);
                if (ok)
                {
                    LOG_INFO("[Loadout] Attached via PutItemToContainer (forbidInstantHolster): " << label);
                }
                else
                {
                    LOG_WARN("[Loadout] PutItemToContainer returned false (forbidInstantHolster): " << label);
                }
                return ok;
            }
            catch (...) {}
            return false;
        }

        try
        {
            SDK::FVector beforeLoc{};
            try { beforeLoc = actor->K2_GetActorLocation(); } catch (...) {}

            LOG_INFO("[Loadout] World Location before InstantHolsterActor: " 
                     << beforeLoc.X << "," 
                     << beforeLoc.Y << "," 
                     << beforeLoc.Z);
            cs->InstantHolsterActor(container, actor);
            LOG_INFO("[Loadout] Attached via InstantHolsterActor: " << label);
            // print the world location to the log, if the location doesnt change, then the location has to be changed first
            SDK::FVector afterLoc{};
            try { afterLoc = actor->K2_GetActorLocation(); } catch (...) {}
            LOG_INFO("[Loadout] World Location after InstantHolsterActor: " 
                     << afterLoc.X << "," 
                     << afterLoc.Y << "," 
                     << afterLoc.Z);

            // If InstantHolsterActor warped the item to an extreme distance, treat as failure.
            // (This happens for some modular vest molle slots; it snaps to a different container.)
            if (desiredRelativeTransform)
            {
                try
                {
                    SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
                    if (playerPawn)
                    {
                        SDK::FVector p = playerPawn->K2_GetActorLocation();
                        const float dx = afterLoc.X - p.X;
                        const float dy = afterLoc.Y - p.Y;
                        const float dz = afterLoc.Z - p.Z;
                        const float distSq = dx*dx + dy*dy + dz*dz;
                        if (distSq > 250000000.0f) // 50000 uu
                        {
                            LOG_ERROR("[Loadout] InstantHolsterActor warp detected (distSq=" << distSq << ") for " << label << "; will fall back");
                            return false;
                        }
                    }
                }
                catch (...) {}
            }
            return true;
        }
        catch (const std::exception& e)
        {
            LOG_WARN("[Loadout] InstantHolsterActor failed for " << label << ": " << e.what());
        }
        catch (...)
        {
            LOG_WARN("[Loadout] InstantHolsterActor threw for " << label);
        }

        try
        {
            bool ok = cs->PutItemToContainer(container, actor);
            if (ok)
            {
                LOG_INFO("[Loadout] Attached via PutItemToContainer: " << label);
            }
            else
            {
                LOG_WARN("[Loadout] PutItemToContainer returned false for: " << label);
            }
            return ok;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] PutItemToContainer threw for " << label << ": " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] PutItemToContainer threw for " << label);
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // GetPlayerBodySlotContainer
    //
    // Maps a "Player-Holster.Player.XXX" UID to the actual player component.
    // Returns nullptr for unknown or intentionally-skipped slots (Tablet).
    // ---------------------------------------------------------------------------
    SDK::UObject* LoadoutSubsystem::GetPlayerBodySlotContainer(SDK::UWorld* world,
                                                               const std::string& parentContainerUid)
    {
        LOG_INFO("[Loadout] Looking up player body slot for: " << parentContainerUid);

        // Tablet slot - we intentionally skip the tablet
        if (parentContainerUid.find("Tablet") != std::string::npos)
        {
            LOG_INFO("[Loadout] Skipping Tablet slot");
            return nullptr;
        }

        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        if (!playerPawn)
        {
            LOG_ERROR("[Loadout] GetPlayerBodySlotContainer: No player pawn");
            return nullptr;
        }

        auto* playerChar = static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C*>(playerPawn);

        try
        {
            if (parentContainerUid.find(".Vest") != std::string::npos)
            {
                LOG_INFO("[Loadout] -> BPC_Vest_Slot");
                return playerChar->BPC_Vest_Slot;
            }
            if (parentContainerUid.find(".Head") != std::string::npos ||
                parentContainerUid.find("Helmet") != std::string::npos)
            {
                LOG_INFO("[Loadout] -> BPC_Head_Slot");
                return playerChar->BPC_Head_Slot;
            }
            if (parentContainerUid.find(".Backpack") != std::string::npos)
            {
                LOG_INFO("[Loadout] -> BPC_BackpackHolster");
                return playerChar->BPC_BackpackHolster;
            }
            if (parentContainerUid.find("RightForearm") != std::string::npos)
            {
                LOG_INFO("[Loadout] -> BPC_RightForearm_Slot");
                return playerChar->BPC_RightForearm_Slot;
            }
            if (parentContainerUid.find("LeftForearm") != std::string::npos)
            {
                LOG_INFO("[Loadout] -> BPC_LeftForearm_Slot");
                return playerChar->BPC_LeftForearm_Slot;
            }

            // Unknown player slot - return nullptr so caller places this in front of
            // the player rather than silently putting it in the wrong slot
            LOG_WARN("[Loadout] Unknown player slot, no match: " << parentContainerUid);
            return nullptr;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception in GetPlayerBodySlotContainer: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] Unknown exception in GetPlayerBodySlotContainer");
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // SpawnAndAttachItem - kept as legacy shim, not used by new ApplyLoadout
    // ---------------------------------------------------------------------------
    SDK::AActor* LoadoutSubsystem::SpawnAndAttachItem(
        SDK::UWorld* world,
        SDK::URadiusContainerSubsystem* containerSubsystem,
        const LoadoutItem& item,
        SDK::UObject* parentContainer)
    {
        SDK::AActor* actor = SpawnItem(world, item, parentContainer);
        if (actor && parentContainer)
        {
            // build transform from capture
            SDK::FTransform relTransform = SDK::UKismetMathLibrary::MakeTransform(
                SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                SDK::FRotator(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll),
                SDK::FVector(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ)
            );

            AttachToContainer(containerSubsystem, parentContainer, actor, item.itemTypeTag, &relTransform, world,
                              false, false);
            // apply relative transform after attachment
            if (parentContainer && actor)
            {
                SDK::FHitResult hit{};
                actor->K2_SetActorRelativeTransform(relTransform, false, &hit, true);
                LOG_INFO("[Loadout] Applied relative transform on " << item.itemTypeTag);
            }
        }
        return actor;
    }

    // ---------------------------------------------------------------------------
    // ApplyLoadout  -  the main apply logic.
    //
    // Algorithm:
    //   1. Load file, backup current gear, destroy current gear.
    //   2. Collect all items from the loadout into a flat pending list.
    //      Items whose itemTypeTag contains "Tablet" are skipped entirely.
    //   3. Multi-pass topological spawn loop:
    //      - Items with a "Player-" parent are spawned + attached to the
    //        appropriate body slot immediately.
    //      - Items with an item-slot parent wait until their parent's OLD uid
    //        has been mapped to a NEW container id.  The new slot container id
    //        is then constructed as  newParentContainerID + "-" + slotSuffix
    //        and passed to GetContainerObject().
    //   4. After every successful spawn the inventory is snapshotted before &
    //      after to discover the new item's container ID, which is stored in
    //      oldUidToNewUid for its children.
    //   5. Items that still can't be placed after all passes are spawned in
    //      front of the player as a last resort.
    // ---------------------------------------------------------------------------
    std::string LoadoutSubsystem::ApplyLoadout(SDK::UWorld* world, const std::string& loadoutName)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        LOG_INFO("[Loadout] ========== APPLY LOADOUT START ==========");
        LOG_INFO("[Loadout] Loadout name: " << loadoutName);

        if (loadoutName.empty())  { return "Error: Loadout name cannot be empty"; }
        if (!world)               { return "Error: World not ready"; }

        // Load the file
        LoadoutData loadout;
        if (!LoadLoadoutFromFile(loadoutName, loadout))
        {
            return "Error: Failed to load loadout file '" + loadoutName + "'";
        }
        LOG_INFO("[Loadout] Loaded " << loadout.items.size() << " items from loadout file");

        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        if (!playerPawn) { return "Error: Player pawn not found"; }

        SDK::URadiusContainerSubsystem* cs = GetContainerSubsystem(world);
        if (!cs)         { return "Error: Container subsystem not found"; }

        // --- Step 1: Backup current gear ---
        LOG_INFO("[Loadout] Step 1: Backing up current gear...");
        BackupCurrentLoadout(world);

        // --- Step 2: Clear current gear ---
        LOG_INFO("[Loadout] Step 2: Clearing current gear...");
        int cleared = ClearPlayerLoadout(world);
        LOG_INFO("[Loadout] Cleared " << cleared << " items");

        // --- Step 3: Flatten item list (handle both flat and nested formats) ---
        // The file format is flat, but the in-memory structure may have .attachments
        // populated if the file had nested [ITEM] blocks. Flatten everything.
        std::vector<LoadoutItem> pending;
        {
            std::function<void(const std::vector<LoadoutItem>&)> flattenInto =
                [&](const std::vector<LoadoutItem>& src)
            {
                for (const auto& it : src)
                {
                    // Skip Tablet items entirely
                    if (it.itemTypeTag.find("Tablet") != std::string::npos)
                    {
                        LOG_INFO("[Loadout] Skipping tablet item: " << it.itemTypeTag);
                        continue;
                    }
                    pending.push_back(it);
                    flattenInto(it.attachments);
                }
            };
            flattenInto(loadout.items);
        }

         LOG_INFO("[Loadout] Pending items to spawn: " << (int)pending.size());
         LOG_INFO("[Loadout] Step 3 complete: " << (int)pending.size() << " items pending spawn");
         LOG_INFO("[Loadout] NOTE: saved transform values are LOCAL offsets/rotations relative to their parent container.");
        for (int i = 0; i < (int)pending.size(); i++)
        {
            const auto& it = pending[i];
            LOG_INFO("[Loadout]   pending[" << i << "] type=" << it.itemTypeTag
                     << " parent=" << it.parentContainerUid
                << " localPos=(" << it.transform.posX
                     << "," << it.transform.posY
                     << "," << it.transform.posZ << ")");
        }
        // oldUid -> new container ID (from inventory snapshot diff)
        std::map<std::string, std::string> uidMap;

        int spawnedCount = 0;
        int failedCount  = 0;

        // --- Step 4: Multi-pass topological spawn ---
        // We allow enough passes for deeply nested hierarchies.
        const int maxPasses = 15;

        for (int pass = 0; pass < maxPasses && !pending.empty(); pass++)
        {
            std::vector<LoadoutItem> deferred;
            bool anyProgressThisPass = false;

            for (const auto& item : pending)
            {
                // Parse the parent UID
                auto [baseUid, slotSuffix] = ParseParentUid(item.parentContainerUid);
                const bool isAttachSlot = (!slotSuffix.empty() && slotSuffix.find("AttachSlot") != std::string::npos);

                SDK::UObject* parentContainer = nullptr;
                bool parentResolved = false;

                if (baseUid.empty())
                {
                    // ---- Player body slot ----
                    parentContainer = GetPlayerBodySlotContainer(world, slotSuffix);
                    // Even if nullptr (unknown slot), we still resolve on first pass
                    // so the item spawns in front of the player rather than being
                    // deferred forever.
                    parentResolved = true;
                }
                else
                {
                    // ---- Item container slot ----
                    auto it = uidMap.find(baseUid);
                    if (it == uidMap.end())
                    {
                        // Parent not yet spawned; defer
                        deferred.push_back(item);
                        continue;
                    }

                    // Construct the new slot container ID:
                    //   newParentContainerID + "-" + slotSuffix
                    std::string newSlotContainerID = it->second + "-" + slotSuffix;
                    LOG_INFO("[Loadout] Resolving slot: " << newSlotContainerID);

                    std::wstring wSlot = ToWideString(newSlotContainerID);
                    SDK::FString fSlot(wSlot.c_str());
                    try
                    {
                        parentContainer = cs->GetContainerObject(fSlot);
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARN("[Loadout] GetContainerObject threw: " << e.what());
                    }
                    catch (...)
                    {
                        LOG_WARN("[Loadout] GetContainerObject threw (unknown)");
                    }

                    if (!parentContainer)
                    {
                        // Slot container may not exist yet on first pass after attach;
                        // defer one more time unless we're on the last pass.
                        if (pass < maxPasses - 2)
                        {
                            LOG_INFO("[Loadout] Slot container not found yet, deferring: " << newSlotContainerID);
                            deferred.push_back(item);
                            continue;
                        }
                        LOG_WARN("[Loadout] Slot container still not found on pass " << pass
                                 << ", will spawn in front of player: " << newSlotContainerID);
                    }
                    parentResolved = true;
                }

                if (!parentResolved)
                {
                    deferred.push_back(item);
                    continue;
                }

                LOG_INFO("[Loadout] [Pass " << pass << "] --- Item: " << item.itemTypeTag);
                LOG_INFO("[Loadout]   uid=" << item.instanceUid
                         << " parent=" << item.parentContainerUid);
                LOG_INFO("[Loadout]   localPos=("
                         << item.transform.posX << ","
                         << item.transform.posY << ","
                         << item.transform.posZ << ")"
                         << " localRot=("
                         << item.transform.rotPitch << ","
                         << item.transform.rotYaw << ","
                         << item.transform.rotRoll << ")");
                LOG_INFO("[Loadout]   parentContainer=" << (void*)parentContainer);

                // --- Snapshot before spawn ---
                std::set<std::string> snapBefore = SnapshotInventory(cs);

                // --- Spawn the item ---
                SDK::UObject* spawnNear = parentContainer;
                if (isAttachSlot)
                {
                    // Attachment slots: the container object may not have a meaningful world pose.
                    // Spawn near player and let the attach call place it.
                    spawnNear = nullptr;
                }

                SDK::AActor* actor = SpawnItem(world, item, spawnNear);
                if (!actor)
                {
                    LOG_ERROR("[Loadout] Spawn failed: " << item.itemTypeTag);
                    failedCount++;
                    // Don't re-queue; move on.
                    continue;
                }

                // Log world position immediately after spawn (should be ~100cm in front of player)
                try
                {
                    SDK::FVector spawnPos = actor->K2_GetActorLocation();
                    LOG_INFO("[Loadout]   post-spawn worldPos=("
                             << spawnPos.X << "," << spawnPos.Y << "," << spawnPos.Z << ")");
                }
                catch (...) { LOG_WARN("[Loadout]   Could not read post-spawn worldPos"); }

                // --- Attach to parent container ---
                if (parentContainer)
                {
                    SDK::FTransform relTransform = SDK::UKismetMathLibrary::MakeTransform(
                        SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                        SDK::FRotator(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll),
                        SDK::FVector(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ)
                    );
                    
                    // // We must attach first, because SetRelativeTransform requires a parent to be relative TO.
                    // // Note: PutItemToContainer often resets the transform to the socket default.
                    // // AttachToContainer(cs, parentContainer, actor, item.itemTypeTag);
                    // if (parentContainer && actor)
                    // {
                    //     // Now apply the captured relative transform.
                    //     SDK::FHitResult hit{};
                    //     actor->K2_SetActorRelativeTransform(relTransform, false, &hit, true);
                        
                    //     LOG_INFO("[Loadout] Applied relative transform on " << item.itemTypeTag 
                    //              << " Loc: " << item.transform.posX << "," << item.transform.posY << "," << item.transform.posZ);

                    //     try
                    //     {
                    //         SDK::FVector postAttachPos = actor->K2_GetActorLocation();
                    //         LOG_INFO("[Loadout]   post-attach worldPos=("
                    //                  << postAttachPos.X << ","
                    //                  << postAttachPos.Y << ","
                    //                  << postAttachPos.Z << ")");
                    //     }
                    //     catch (...) { LOG_WARN("[Loadout]   Could not read post-attach worldPos"); }
                    // }

                    // Pre-positioning helps some container attach paths that rely on current world pose,
                    // but it's actively harmful for AttachSlot.* containers.
                    if (parentContainer && !isAttachSlot)
                    {
                        SDK::FVector parentWorldLoc;
                        SDK::FRotator parentWorldRot;
                        if (TryGetObjectWorldPose(parentContainer, parentWorldLoc, parentWorldRot))
                        {
                            // calculate the desired world transform of the item based on the captured local transform and the parent's world transform
                            SDK::FVector desiredWorldLoc = parentWorldLoc;
                            SDK::FRotator desiredWorldRot = parentWorldRot;

                            // apply the local offset/rotation to get the desired world transform
                            SDK::FVector rotatedOffset = RotateVectorByRotator(
                                SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                                parentWorldRot);
                            desiredWorldLoc.X += rotatedOffset.X;
                            desiredWorldLoc.Y += rotatedOffset.Y;
                            desiredWorldLoc.Z += rotatedOffset.Z;

                            desiredWorldRot.Pitch += item.transform.rotPitch;
                            desiredWorldRot.Yaw += item.transform.rotYaw;
                            desiredWorldRot.Roll += item.transform.rotRoll;

                            // set the item's world transform to the desired location before attaching
                            try
                            {
                                // usage is:
                                // bool K2_SetActorLocation(const struct FVector& NewLocation, bool bSweep, struct FHitResult* SweepHitResult, bool bTeleport);
                                actor->K2_SetActorLocation(desiredWorldLoc, false, nullptr, true);
                                actor->K2_SetActorRotation(desiredWorldRot, false);
                                LOG_INFO("[Loadout] Moved item to pre-attach world position: "
                                         << desiredWorldLoc.X << "," << desiredWorldLoc.Y << "," << desiredWorldLoc.Z);
                            }
                            catch (...)
                            {
                                LOG_WARN("[Loadout] Failed to set pre-attach world position");
                            }
                        }
                        else
                        {
                            LOG_WARN("[Loadout] Could not get parent container world pose");
                        }

                    }

                    const SDK::FTransform* attachRel = nullptr;
                    if (!baseUid.empty())
                    {
                        // Only item-slot attachments should use the explicit relative-transform holster path.
                        // Player body-slot holsters are left on the subsystem InstantHolsterActor path.
                        attachRel = &relTransform;
                    }

                    // AttachSlot.* containers are not holsters; InstantHolsterActor on the subsystem can
                    // attach them to the wrong place (huge offsets). Force PutItemToContainer-only.
                    const bool preferPutFirst = isAttachSlot;
                    const bool forbidInstantHolster = isAttachSlot;

                    AttachToContainer(cs, parentContainer, actor, item.itemTypeTag, attachRel, world,
                                      preferPutFirst, forbidInstantHolster);



                }
                // (If parentContainer is nullptr, item sits in front of player - acceptable fallback)

                // --- Snapshot after to discover new container ID ---
                std::set<std::string> snapAfter = SnapshotInventory(cs);
                std::string newContainerID;
                for (const auto& uid : snapAfter)
                {
                    if (snapBefore.find(uid) == snapBefore.end())
                    {
                        newContainerID = uid;
                        break;
                    }
                }

                if (!newContainerID.empty())
                {
                    LOG_INFO("[Loadout] UID mapped: " << item.instanceUid
                             << " -> " << newContainerID);
                    uidMap[item.instanceUid] = newContainerID;
                }
                else
                {
                    // Inventory snapshot didn't change - item may not have been
                    // registered (e.g. not properly attached).  Still log.
                    LOG_WARN("[Loadout] Could not determine new container ID for: "
                             << item.itemTypeTag << " (old uid=" << item.instanceUid << ")");
                }

                spawnedCount++;
                anyProgressThisPass = true;
            }

            pending = deferred;
            LOG_INFO("[Loadout] Pass " << pass << " done: spawned=" << spawnedCount
                     << " deferred=" << (int)pending.size());

            if (!anyProgressThisPass)
            {
                LOG_WARN("[Loadout] No progress this pass, breaking early");
                break;
            }
        }

        // --- Step 5: Last resort - spawn remaining items in front of player ---
        for (const auto& item : pending)
        {
            LOG_WARN("[Loadout] Last-resort spawn in front of player: " << item.itemTypeTag
                     << " (parent=" << item.parentContainerUid << ")");
            SDK::AActor* actor = SpawnItem(world, item, nullptr);
            if (actor)
            {
                spawnedCount++;
            }
            else
            {
                failedCount++;
            }
        }

        std::ostringstream result;
        result << "Applied loadout '" << loadoutName << "': "
               << spawnedCount << " spawned";
        if (failedCount > 0)
        {
            result << ", " << failedCount << " failed";
        }

        LOG_INFO("[Loadout] ========== APPLY LOADOUT END ==========");
        LOG_INFO("[Loadout] " << result.str());

        ModFeedback::ShowMessage(
            (L"Loadout applied: " + std::wstring(loadoutName.begin(), loadoutName.end())).c_str(),
            3.0f,
            SDK::FLinearColor{0.5f, 1.0f, 0.0f, 1.0f});

        return result.str();
    }
}
