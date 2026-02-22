/*
AILEARNINGS
- GetPlayersInventory returns player items as TArray<URadiusItemDynamicData*>
- ItemType.TagName gives the FGameplayTag's FName which can be converted to string
- Attachments are stored directly in URadiusItemDynamicData::Attachments array
- Transform data is in DynamicTransform.LocalTransform (FTransform)
- Use UFLSpawn::SpawnItemByTypeTag with FItemConfiguration to spawn items
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
        if (!world)
        {
            LOG_ERROR("[Loadout] World is null");
            return nullptr;
        }
        
        // Get the RadiusContainerSubsystem from the world
        SDK::URadiusContainerSubsystem* subsystem = nullptr;
        
        // Try to find it via UWorld subsystems
        // The subsystem should be accessible via world->GetSubsystem<URadiusContainerSubsystem>()
        // But since that's a template, we need to use FindObject or similar
        
        // Search for the subsystem in GObjects
        for (int i = 0; i < SDK::UObject::GObjects->Num(); i++)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
            if (!obj) continue;
            
            // Check class name
            SDK::UClass* cls = obj->Class;
            if (!cls) continue;
            
            std::string className = cls->GetName();
            if (className == "RadiusContainerSubsystem")
            {
                subsystem = static_cast<SDK::URadiusContainerSubsystem*>(obj);
                break;
            }
        }
        
        if (!subsystem)
        {
            LOG_ERROR("[Loadout] Could not find RadiusContainerSubsystem");
        }
        
        return subsystem;
    }
    
    std::string LoadoutSubsystem::CaptureLoadout(SDK::UWorld* world, const std::string& loadoutName)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        if (loadoutName.empty())
        {
            return "Error: Loadout name cannot be empty";
        }
        
        if (!world)
        {
            return "Error: World not ready";
        }
        
        LOG_INFO("[Loadout] Capturing loadout: " << loadoutName);
        
        // Get the container subsystem
        SDK::URadiusContainerSubsystem* containerSubsystem = GetContainerSubsystem(world);
        if (!containerSubsystem)
        {
            return "Error: Could not find container subsystem";
        }
        
        // Get player's inventory items
        SDK::TArray<SDK::URadiusItemDynamicData*> playerItems;
        try
        {
            playerItems = containerSubsystem->GetPlayersInventory();
        }
        catch (...)
        {
            LOG_ERROR("[Loadout] Exception calling GetPlayersInventory");
            return "Error: Failed to get player inventory";
        }
        
        LOG_INFO("[Loadout] Found " << playerItems.Num() << " items in player inventory");
        
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
        
        // Capture each item
        for (int i = 0; i < playerItems.Num(); i++)
        {
            SDK::URadiusItemDynamicData* itemData = playerItems[i];
            if (!itemData)
            {
                continue;
            }
            
            try
            {
                LoadoutItem item = CaptureItemData(itemData);
                
                // Only add top-level items (items whose parent is the player inventory, not another item)
                // We check if parent is a player container vs. another item
                bool isTopLevel = true;
                for (int j = 0; j < playerItems.Num(); j++)
                {
                    if (i == j) continue;
                    SDK::URadiusItemDynamicData* otherItem = playerItems[j];
                    if (!otherItem) continue;
                    
                    // Check if this item's parent is another item's UID
                    if (item.parentContainerUid == otherItem->InstanceUid.ToString())
                    {
                        isTopLevel = false;
                        break;
                    }
                }
                
                if (isTopLevel)
                {
                    loadout.items.push_back(item);
                    LOG_INFO("[Loadout] Captured top-level item: " << item.itemTypeTag 
                             << " (UID: " << item.instanceUid << ")");
                }
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("[Loadout] Exception capturing item: " << e.what());
            }
        }
        
        LOG_INFO("[Loadout] Captured " << loadout.items.size() << " top-level items");
        
        // Save to file
        if (!SaveLoadoutToFile(loadout))
        {
            return "Error: Failed to save loadout file";
        }
        
        std::ostringstream result;
        result << "Captured loadout '" << loadoutName << "' with " << loadout.items.size() << " items";
        
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
    
    std::string LoadoutSubsystem::ApplyLoadout(SDK::UWorld* world, const std::string& loadoutName)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        if (loadoutName.empty())
        {
            return "Error: Loadout name cannot be empty";
        }
        
        if (!world)
        {
            return "Error: World not ready";
        }
        
        LOG_INFO("[Loadout] Applying loadout: " << loadoutName);
        
        // Load the loadout file
        LoadoutData loadout;
        if (!LoadLoadoutFromFile(loadoutName, loadout))
        {
            return "Error: Failed to load loadout file '" + loadoutName + "'";
        }
        
        LOG_INFO("[Loadout] Loaded " << loadout.items.size() << " items from loadout");
        
        // Get player character for spawn location reference
        SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
        if (!playerPawn)
        {
            return "Error: Player pawn not found";
        }
        
        // Spawn each item
        int spawnedCount = 0;
        int failedCount = 0;
        
        for (const auto& item : loadout.items)
        {
            if (SpawnItemFromLoadout(world, item, ""))
            {
                spawnedCount++;
            }
            else
            {
                failedCount++;
            }
        }
        
        std::ostringstream result;
        result << "Applied loadout '" << loadoutName << "': " << spawnedCount << " spawned";
        if (failedCount > 0)
        {
            result << ", " << failedCount << " failed";
        }
        
        // Show in-game feedback
        ModFeedback::ShowMessage(
            (L"Loadout applied: " + std::wstring(loadoutName.begin(), loadoutName.end())).c_str(),
            3.0f,
            SDK::FLinearColor{0.5f, 1.0f, 0.0f, 1.0f});
        
        return result.str();
    }
    
    bool LoadoutSubsystem::SpawnItemFromLoadout(SDK::UWorld* world, const LoadoutItem& item, const std::string& parentContainer)
    {
        if (item.itemTypeTag.empty())
        {
            LOG_ERROR("[Loadout] Item has empty type tag");
            return false;
        }
        
        LOG_INFO("[Loadout] Spawning item: " << item.itemTypeTag);
        
        // Create FGameplayTag from the tag string
        // Convert to wide string, then use SDK helper to get FName from string table
        std::wstring wideTag = ToWideString(item.itemTypeTag);
        SDK::FGameplayTag typeTag;
        typeTag.TagName = SDK::BasicFilesImpleUtils::StringToName(wideTag.c_str());
        
        // Create spawn transform
        SDK::FVector location{item.transform.posX, item.transform.posY, item.transform.posZ};
        SDK::FRotator rotation{item.transform.rotPitch, item.transform.rotYaw, item.transform.rotRoll};
        SDK::FVector scale{item.transform.scaleX, item.transform.scaleY, item.transform.scaleZ};
        
        // If this is a top-level item, spawn in front of player
        if (parentContainer.empty())
        {
            SDK::APawn* playerPawn = GameContext::GetPlayerPawn(world);
            if (playerPawn)
            {
                SDK::FVector playerLoc = playerPawn->K2_GetActorLocation();
                SDK::FRotator playerRot = playerPawn->K2_GetActorRotation();
                SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(playerRot);
                
                // Spawn 150cm in front of player, at waist height
                location.X = playerLoc.X + forward.X * 150.0f;
                location.Y = playerLoc.Y + forward.Y * 150.0f;
                location.Z = playerLoc.Z + 50.0f; // Slightly above ground
            }
        }
        
        SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(location, rotation, scale);
        
        // Create item configuration
        SDK::FItemConfiguration itemConfig{};
        itemConfig.bShopItem = false;
        itemConfig.StartDurabilityRatio = item.durability;
        itemConfig.StackAmount = 1;
        
        // Set up stacked items for magazines
        // Note: This may need to be done differently - the magazine contents
        // might need to be set after spawning using AddAdditionalData or similar
        
        // Spawn the item
        try
        {
            SDK::AActor* spawnedActor = SDK::UFLSpawn::SpawnItemByTypeTag(
                world,
                typeTag,
                spawnTransform,
                itemConfig,
                true // bCreateDynamicData
            );
            
            if (!spawnedActor)
            {
                LOG_ERROR("[Loadout] Failed to spawn item: " << item.itemTypeTag);
                return false;
            }
            
            LOG_INFO("[Loadout] Successfully spawned: " << spawnedActor->GetName());
            
            // Handle attachments
            // Note: Attachments may need to be spawned and then attached programmatically
            // This might require using the item's container interface
            for (const auto& attachment : item.attachments)
            {
                // For now, just spawn attachments near the parent
                // Full attachment handling would require more complex logic
                SpawnItemFromLoadout(world, attachment, item.instanceUid);
            }
            
            return true;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[Loadout] Exception spawning item: " << e.what());
            return false;
        }
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
}
