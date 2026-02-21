#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <WinSock2.h>

namespace Mod
{
    class CommandQueue;

    class TcpServer
    {
    public:
        TcpServer();
        ~TcpServer();

        bool Start(uint16_t port, CommandQueue *queue);
        void Stop();
        bool IsRunning() const;

    private:
        void Run(uint16_t port, std::shared_ptr<std::promise<bool>> ready);
        void HandleClient(SOCKET clientSocket);
        static void CloseSocket(SOCKET &socket);

        std::thread thread_{};
        std::atomic<bool> running_{false};
        SOCKET listenSocket_{INVALID_SOCKET};
        CommandQueue *queue_{nullptr};
    };
}
