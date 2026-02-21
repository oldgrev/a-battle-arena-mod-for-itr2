#pragma once

#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace Mod
{
    class CommandQueue
    {
    public:
        void Push(std::string command)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(command));
        }

        std::vector<std::string> Drain()
        {
            std::vector<std::string> drained;
            std::lock_guard<std::mutex> lock(mutex_);
            while (!queue_.empty())
            {
                drained.push_back(std::move(queue_.front()));
                queue_.pop();
            }
            return drained;
        }

        void PushResponse(std::string response)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            responses_.push(std::move(response));
        }

        std::vector<std::string> DrainResponses()
        {
            std::vector<std::string> drained;
            std::lock_guard<std::mutex> lock(mutex_);
            while (!responses_.empty())
            {
                drained.push_back(std::move(responses_.front()));
                responses_.pop();
            }
            return drained;
        }

    private:
        std::mutex mutex_{};
        std::queue<std::string> queue_{};
        std::queue<std::string> responses_{};
    };
}
