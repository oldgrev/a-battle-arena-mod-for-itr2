#pragma once

#include <atomic>

namespace Mod
{
    class CommandQueue;
    class CommandHandlerRegistry;

    namespace RuntimeState
    {
        void SetCommandQueue(CommandQueue *queue);
        CommandQueue *GetCommandQueue();

        void SetCommandHandlerRegistry(CommandHandlerRegistry *registry);
        CommandHandlerRegistry *GetCommandHandlerRegistry();
    }
}
