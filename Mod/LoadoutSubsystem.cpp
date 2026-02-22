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
*/

#include "LoadoutSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <set>
#include <functional>

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
                    LoadoutItem item = CaptureItemData(itemData);
                    loadout.items.push_back(item);
                    
                    int attachmentCount = item.attachments.size();
                    LOG_INFO("[Loadout] Top-level item: " << item.itemTypeTag 
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
    
    LoadoutItem LoadoutSubsystem::CaptureItemData(SDK::URadiusItemDynamicData* itemData)
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
        // DynamicTransform contains FConfirmedDynamicTransform with DynamicLocation and DynamicRotation
        SDK::FVector loc;
        SDK::FRotator rot;
        SDK::FVector scale{1.0f, 1.0f, 1.0f};
        
        if (itemData->DynamicTransform.bInitializedWithValues)
        {
            // DynamicLocation and DynamicRotation are FVector_NetQuantize which inherit from FVector
            loc.X = itemData->DynamicTransform.DynamicLocation.X;
            loc.Y = itemData->DynamicTransform.DynamicLocation.Y;
            loc.Z = itemData->DynamicTransform.DynamicLocation.Z;
            
            // DynamicRotation is stored as a vector (Pitch, Yaw, Roll)
            rot.Pitch = static_cast<double>(itemData->DynamicTransform.DynamicRotation.X);
            rot.Yaw = static_cast<double>(itemData->DynamicTransform.DynamicRotation.Y);
            rot.Roll = static_cast<double>(itemData->DynamicTransform.DynamicRotation.Z);
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
                LoadoutItem attachmentItem = CaptureItemData(attachment);
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
    //              Item is placed slightly in front of the player so
    //              it doesn't immediately clip into geometry.
    // ---------------------------------------------------------------------------
    static SDK::AActor* SpawnItem(SDK::UWorld* world, const LoadoutItem& item)
    {
        if (item.itemTypeTag.empty())
        {
            LOG_ERROR("[Loadout] SpawnItem: Empty type tag");
            return nullptr;
        }

        std::wstring wideTag = ToWideString(item.itemTypeTag);
        SDK::FGameplayTag typeTag;
        typeTag.TagName = SDK::BasicFilesImpleUtils::StringToName(wideTag.c_str());

        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        SDK::FVector loc{0, 0, 0};
        SDK::FRotator rot{0, 0, 0};
        SDK::FVector scale{1, 1, 1};

        if (playerPawn)
        {
            SDK::FVector pLoc = playerPawn->K2_GetActorLocation();
            SDK::FRotator pRot = playerPawn->K2_GetActorRotation();
            SDK::FVector fwd = SDK::UKismetMathLibrary::GetForwardVector(pRot);
            // Place 100 cm ahead, 10 cm above feet - keeps it accessible if attach fails
            loc.X = pLoc.X + fwd.X * 100.0f;
            loc.Y = pLoc.Y + fwd.Y * 100.0f;
            loc.Z = pLoc.Z + 10.0f;
        }

        SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(loc, rot, scale);

        SDK::FItemConfiguration cfg{};
        cfg.bShopItem = false;
        cfg.StartDurabilityRatio = item.durability;
        cfg.StackAmount = 1;

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
                                  const std::string& label)
    {
        if (!container || !actor)
        {
            LOG_WARN("[Loadout] AttachToContainer: null container or actor for " << label);
            return false;
        }

        try
        {
            cs->InstantHolsterActor(container, actor);
            LOG_INFO("[Loadout] Attached via InstantHolsterActor: " << label);
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
        SDK::AActor* actor = SpawnItem(world, item);
        if (actor && parentContainer)
        {
            // build transform from capture
            SDK::FTransform relTransform = SDK::UKismetMathLibrary::MakeTransform(
                SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                SDK::FRotator(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll),
                SDK::FVector(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ)
            );
            AttachToContainer(containerSubsystem, parentContainer, actor, item.itemTypeTag);
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

                // --- Snapshot before spawn ---
                std::set<std::string> snapBefore = SnapshotInventory(cs);

                // --- Spawn the item ---
                SDK::AActor* actor = SpawnItem(world, item);
                if (!actor)
                {
                    LOG_ERROR("[Loadout] Spawn failed: " << item.itemTypeTag);
                    failedCount++;
                    // Don't re-queue; move on.
                    continue;
                }

                // --- Attach to parent container ---
                if (parentContainer)
                {
                    SDK::FTransform relTransform = SDK::UKismetMathLibrary::MakeTransform(
                        SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                        SDK::FRotator(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll),
                        SDK::FVector(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ)
                    );
                    AttachToContainer(cs, parentContainer, actor, item.itemTypeTag);
                    if (parentContainer && actor)
                    {
                        SDK::FHitResult hit{};
                        actor->K2_SetActorRelativeTransform(relTransform, false, &hit, true);
                        LOG_INFO("[Loadout] Applied relative transform on " << item.itemTypeTag);
                    }
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
            SDK::AActor* actor = SpawnItem(world, item);
            if (actor)
            {
                // apply captured world transform as a final adjustment
                SDK::FTransform worldTransform = SDK::UKismetMathLibrary::MakeTransform(
                    SDK::FVector(item.transform.posX, item.transform.posY, item.transform.posZ),
                    SDK::FRotator(item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll),
                    SDK::FVector(item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ)
                );
                try
                {
                    SDK::FHitResult hit{};
                    actor->K2_SetActorTransform(worldTransform, false, &hit, true);
                    LOG_INFO("[Loadout] Applied world transform on fallback item " << item.itemTypeTag);
                }
                catch (...)
                {
                    LOG_WARN("[Loadout] Failed to set world transform on fallback item");
                }

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
