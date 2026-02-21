#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "..\SDK.hpp"

namespace Mod
{
    namespace Items
    {
        struct SpawnParams
        {
            std::string className;
            bool useFullName = false;
            float distance = 200.0f; // Default spawn distance in front of player
            float lateralOffset = 0.0f;
            float heightOffset = 0.0f;
        };

        struct SpawnResult
        {
            bool success = false;
            std::string message;
            SDK::AActor *actor = nullptr;
        };

        class ItemSubsystem
        {
        public:
            using SpawnCallback = std::function<void(const SpawnResult &)>;

            ItemSubsystem() = default;
            ~ItemSubsystem() = default;

            // Prevent copying
            ItemSubsystem(const ItemSubsystem &) = delete;
            ItemSubsystem &operator=(const ItemSubsystem &) = delete;

            // Allow moving
            ItemSubsystem(ItemSubsystem &&) = default;
            ItemSubsystem &operator=(ItemSubsystem &&) = default;

            // Initialize the subsystem
            void Initialize();

            // Shutdown the subsystem
            void Shutdown();

            // Spawn an item in the world
            SpawnResult SpawnItem(SDK::UWorld *world, const SpawnParams &params);

            // Spawn an item directly to player inventory
            SpawnResult SpawnToInventory(SDK::UWorld *world, const SpawnParams &params);

            // Clear all items in the world
            int ClearItems(SDK::UWorld *world);

            // List items
            std::string ListItems(SDK::UWorld *world, size_t maxLines = 10);

            // Get item count
            int GetItemCount(SDK::UWorld *world);

            // Check if subsystem is initialized
            bool IsInitialized() const { return initialized_; }

        private:
            bool initialized_ = false;

            // Helper methods
            SDK::FVector CalculateSpawnLocation(SDK::UWorld *world, float distance, float lateralOffset, float heightOffset);
        };
    }
}
