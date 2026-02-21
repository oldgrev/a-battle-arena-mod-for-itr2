#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    namespace AI
    {
        struct SpawnParams
        {
            std::string className;
            bool useFullName = false;
            float distance = 300.0f;
            float lateralOffset = 0.0f;
        };

        struct SpawnResult
        {
            bool success = false;
            std::string message;
            SDK::AActor *actor = nullptr;
        };

        class AISubsystem
        {
        public:
            using SpawnCallback = std::function<void(const SpawnResult &)>;

            AISubsystem() = default;
            ~AISubsystem() = default;

            // Prevent copying
            AISubsystem(const AISubsystem &) = delete;
            AISubsystem &operator=(const AISubsystem &) = delete;

            // Allow moving
            AISubsystem(AISubsystem &&) = default;
            AISubsystem &operator=(AISubsystem &&) = default;

            // Initialize the subsystem
            void Initialize();

            // Shutdown the subsystem
            void Shutdown();

            // Spawn an NPC
            SpawnResult SpawnNPC(SDK::UWorld *world, const SpawnParams &params);

            // Clear all NPCs
            int ClearNPCs(SDK::UWorld *world);

            // List NPCs
            std::string ListNPCs(SDK::UWorld *world, size_t maxLines = 10);

            // Get NPC count
            int GetNPCCount(SDK::UWorld *world);

            // Reinitialize NPC system
            void ReinitNPC(SDK::UWorld *world);

            // Check if subsystem is initialized
            bool IsInitialized() const { return initialized_; }

        private:
            bool initialized_ = false;

            // Helper methods
            SDK::FVector CalculateSpawnLocation(SDK::UWorld *world, float distance, float lateralOffset);
            bool RegisterNPCWithSubsystem(SDK::UWorld *world, SDK::APawn *pawn);
            bool UnregisterNPCWithSubsystem(SDK::UWorld *world, SDK::APawn *pawn);
        };
    }
}
