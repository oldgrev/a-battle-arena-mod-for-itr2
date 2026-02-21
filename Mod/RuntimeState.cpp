#include "pch.h"

#include "RuntimeState.hpp"

namespace Mod::RuntimeState
{
    namespace
    {
        std::atomic<Mod::CommandQueue *> gQueue{nullptr};
        std::atomic<Mod::CommandHandlerRegistry *> gRegistry{nullptr};
    }

    void SetCommandQueue(CommandQueue *queue)
    {
        gQueue.store(queue, std::memory_order_release);
    }

    CommandQueue *GetCommandQueue()
    {
        return gQueue.load(std::memory_order_acquire);
    }

    void SetCommandHandlerRegistry(CommandHandlerRegistry *registry)
    {
        gRegistry.store(registry, std::memory_order_release);
    }

    CommandHandlerRegistry *GetCommandHandlerRegistry()
    {
        return gRegistry.load(std::memory_order_acquire);
    }
}
