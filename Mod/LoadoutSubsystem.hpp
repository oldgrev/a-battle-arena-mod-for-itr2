#pragma once

/*
AILEARNINGS
- URadiusContainerSubsystem::GetPlayersInventory() returns all player items as URadiusItemDynamicData
- URadiusItemDynamicData contains ItemType (FGameplayTag), Durability, Attachments, StackedItems
- UFLSpawn::SpawnItemByTypeTag spawns items from FGameplayTag
- UFLItems::CreateNewDynamicData can create dynamic data before spawning
- Player containers have IDs like "PlayerInventory", and can be traversed via GetAllChildItemDynamicDatas
*/

#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "..\CppSDK\SDK.hpp"

namespace Mod::Loadout
{
    /**
     * Represents a single item with all its state for serialization.
     * Recursively includes attachments.
     */
    struct LoadoutItem
    {
        // FGameplayTag::TagName as string (e.g., "Item.Weapon.AK74")
        std::string itemTypeTag;
        
        // Unique instance ID (for parent-child relationships during apply)
        std::string instanceUid;
        
        // Parent container ID (e.g., "PlayerInventory", or parent item's UID)
        std::string parentContainerUid;
        
        // Durability value (0.0 to 1.0)
        float durability = 1.0f;
        
        // Stacked items (for magazines, ammo boxes, etc)
        // Format: "tag:count" entries
        std::vector<std::pair<std::string, int>> stackedItems;
        
        // Additional data key-value pairs
        std::map<std::string, std::string> additionalData;
        
        // Relative transform in parent container
        struct 
        {
            float posX = 0, posY = 0, posZ = 0;
            float rotPitch = 0, rotYaw = 0, rotRoll = 0;
            float scaleX = 1, scaleY = 1, scaleZ = 1;
        } transform;
        
        // Attachments (child items)
        std::vector<LoadoutItem> attachments;
    };
    
    /**
     * Top-level loadout data structure.
     */
    struct LoadoutData
    {
        std::string name;
        std::string timestamp;
        int version = 1;
        
        // All items on the player (backpack, armor, weapons, etc.)
        std::vector<LoadoutItem> items;
    };
    
    /**
     * Manages player loadouts - capture, save, load, and apply.
     */
    class LoadoutSubsystem
    {
    public:
        static LoadoutSubsystem* Get();
        
        void Initialize();
        void Shutdown();
        
        /**
         * Capture the player's current equipment and save to a loadout file.
         * @param world The game world
         * @param loadoutName Name for the loadout (used as filename)
         * @return Success message or error string
         */
        std::string CaptureLoadout(SDK::UWorld* world, const std::string& loadoutName);
        
        /**
         * Apply a saved loadout to the player.
         * This clears current equipment and spawns items from the loadout.
         * @param world The game world
         * @param loadoutName Name of the loadout to apply
         * @return Success message or error string
         */
        std::string ApplyLoadout(SDK::UWorld* world, const std::string& loadoutName);
        
        /**
         * List all available loadouts.
         * @return Formatted string with loadout names
         */
        std::string ListLoadouts() const;
        
        /**
         * Get the currently selected loadout name for arena start.
         */
        std::string GetSelectedLoadout() const;
        
        /**
         * Set the loadout to be auto-applied on arena start.
         */
        void SetSelectedLoadout(const std::string& name);
        
        /**
         * Check if a loadout exists.
         */
        bool LoadoutExists(const std::string& name) const;
        
        /**
         * Backup current player loadout to a temp file.
         * @return true if backup succeeded
         */
        bool BackupCurrentLoadout(SDK::UWorld* world);
        
        /**
         * Clear all items from player's equipment.
         * @return Number of items dropped
         */
        int ClearPlayerLoadout(SDK::UWorld* world);
        
    private:
        // Helper to recursively capture item data (with correct relative transforms)
        LoadoutItem CaptureItemData(SDK::URadiusItemDynamicData* itemData, 
                                    SDK::URadiusContainerSubsystem* cs,
                                    const std::map<std::string, SDK::AActor*>& itemActors);
        
        // Helper to get player's container subsystem
        SDK::URadiusContainerSubsystem* GetContainerSubsystem(SDK::UWorld* world) const;
        
        // File I/O helpers
        bool SaveLoadoutToFile(const LoadoutData& loadout);
        bool LoadLoadoutFromFile(const std::string& name, LoadoutData& outLoadout);
        std::string GetLoadoutFilePath(const std::string& name) const;
        std::string GetLoadoutsDirectory() const;
        
        // Serialization helpers
        std::string SerializeLoadout(const LoadoutData& loadout) const;
        bool DeserializeLoadout(const std::string& content, LoadoutData& outLoadout);
        std::string SerializeItem(const LoadoutItem& item, int depth) const;
        
        // Apply helpers (shim - real logic is in ApplyLoadout)
        SDK::AActor* SpawnAndAttachItem(
            SDK::UWorld* world,
            SDK::URadiusContainerSubsystem* containerSubsystem,
            const LoadoutItem& item,
            SDK::UObject* parentContainer);
        
        // Get the player's body-slot container component for top-level items
        SDK::UObject* GetPlayerBodySlotContainer(SDK::UWorld* world, const std::string& parentContainerUid);
        
        mutable std::recursive_mutex mutex_;
        std::string selectedLoadout_;
        bool initialized_ = false;
    };
}
