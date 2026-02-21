

#include "TcpServer.hpp"

#include <cctype>
#include <future>
#include <iostream>
#include <vector>

#include "CommandQueue.hpp"

#pragma comment(lib, "Ws2_32.lib")

namespace Mod
{
    namespace
    {
        constexpr int kSelectTimeoutMicros = 250000;
        constexpr int kRecvTimeoutMillis = 250;

        std::string TrimLineEndings(std::string value)
        {
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            {
                value.pop_back();
            }
            return value;
        }

        std::string TrimWhitespace(std::string value)
        {
            size_t start = 0;
            while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            {
                ++start;
            }

            size_t end = value.size();
            while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
            {
                --end;
            }

            return value.substr(start, end - start);
        }

        bool IsAllowedChar(char ch)
        {
            unsigned char uch = static_cast<unsigned char>(ch);
            return std::isalnum(uch) || ch == '_' || ch == '/' || ch == '.';
        }

        std::string SanitizeCommandLine(const std::string &value)
        {
            std::string trimmed = TrimWhitespace(value);
            std::vector<std::string> tokens;
            std::string current;

            for (char ch : trimmed)
            {
                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    if (!current.empty())
                    {
                        tokens.push_back(std::move(current));
                        current.clear();
                    }
                    continue;
                }

                if (IsAllowedChar(ch))
                {
                    current.push_back(ch);
                }
            }

            if (!current.empty())
            {
                tokens.push_back(std::move(current));
            }

            std::string sanitized;
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].empty())
                {
                    continue;
                }

                if (!sanitized.empty())
                {
                    sanitized.push_back(' ');
                }
                sanitized.append(tokens[i]);
            }

            return sanitized;
        }

        bool SendResponse(SOCKET socket, const std::string &response)
        {
            if (response.empty())
            {
                return true;
            }

            std::string normalized;
            normalized.reserve(response.size() + 4);

            for (size_t i = 0; i < response.size(); ++i)
            {
                char ch = response[i];
                if (ch == '\n')
                {
                    if (i == 0 || response[i - 1] != '\r')
                    {
                        normalized.push_back('\r');
                    }
                    normalized.push_back('\n');
                }
                else
                {
                    normalized.push_back(ch);
                }
            }

            if (normalized.empty() || normalized.back() != '\n')
            {
                normalized.append("\r\n");
            }

            size_t totalSent = 0;
            while (totalSent < normalized.size())
            {
                int sent = send(socket, normalized.data() + totalSent, static_cast<int>(normalized.size() - totalSent), 0);
                if (sent == SOCKET_ERROR || sent == 0)
                {
                    return false;
                }
                totalSent += static_cast<size_t>(sent);
            }

            return true;
        }
    }

    TcpServer::TcpServer() = default;

    TcpServer::~TcpServer()
    {
        Stop();
    }

    bool TcpServer::Start(uint16_t port, CommandQueue *queue)
    {
        if (running_.load())
        {
            return false;
        }

        queue_ = queue;

        // Start the server thread but wait for it to actually bind/listen so callers can
        // reliably log success and clients don't connect to a dead port.
        running_.store(true);
        auto readyPromise = std::make_shared<std::promise<bool>>();
        std::future<bool> readyFuture = readyPromise->get_future();

        thread_ = std::thread([this, port, readyPromise]()
            {
                Run(port, readyPromise);
            });

        // Wait up to 2 seconds for bind/listen.
        if (readyFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        {
            return false;
        }

        bool ok = false;
        try { ok = readyFuture.get(); } catch (...) { ok = false; }
        if (!ok)
        {
            Stop();
            return false;
        }
        return true;
    }

    void TcpServer::Stop()
    {
        if (!running_.exchange(false))
        {
            return;
        }

        CloseSocket(listenSocket_);

        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    bool TcpServer::IsRunning() const
    {
        return running_.load();
    }

    void TcpServer::Run(uint16_t port, std::shared_ptr<std::promise<bool>> ready)
    {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            std::cout << "[tcp] WSAStartup failed.\n";
            running_.store(false);
			if (ready) ready->set_value(false);
            return;
        }

        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET)
        {
            std::cout << "[tcp] Failed to create socket.\n";
            WSACleanup();
            running_.store(false);
			if (ready) ready->set_value(false);
            return;
        }

		// IMPORTANT: don't allow multiple processes to bind the same port.
		// SO_EXCLUSIVEADDRUSE prevents the "two servers bind 7777" situation on Windows.
		BOOL exclusive = TRUE;
		setsockopt(listenSocket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in service{};
        service.sin_family = AF_INET;
        service.sin_addr.s_addr = htonl(INADDR_ANY);
        service.sin_port = htons(port);

        if (bind(listenSocket_, reinterpret_cast<SOCKADDR *>(&service), sizeof(service)) == SOCKET_ERROR)
        {
            std::cout << "[tcp] Bind failed.\n";
            CloseSocket(listenSocket_);
            WSACleanup();
            running_.store(false);
			if (ready) ready->set_value(false);
            return;
        }

        if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR)
        {
            std::cout << "[tcp] Listen failed.\n";
            CloseSocket(listenSocket_);
            WSACleanup();
            running_.store(false);
			if (ready) ready->set_value(false);
            return;
        }

        std::cout << "[tcp] Listening on port " << port << ".\n";
		if (ready) ready->set_value(true);

        while (running_.load())
        {
            fd_set readSet{};
            FD_ZERO(&readSet);
            FD_SET(listenSocket_, &readSet);

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = kSelectTimeoutMicros;

            int ready = select(0, &readSet, nullptr, nullptr, &timeout);
            if (!running_.load())
            {
                break;
            }

            if (ready > 0 && FD_ISSET(listenSocket_, &readSet))
            {
                SOCKET clientSocket = accept(listenSocket_, nullptr, nullptr);
                if (clientSocket != INVALID_SOCKET)
                {
                    std::cout << "[tcp] Client connected.\n";
                    HandleClient(clientSocket);
                    CloseSocket(clientSocket);
                    std::cout << "[tcp] Client disconnected.\n";
                }
            }
        }

        CloseSocket(listenSocket_);
        WSACleanup();
    }

    void TcpServer::HandleClient(SOCKET clientSocket)
    {
        DWORD recvTimeout = kRecvTimeoutMillis;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&recvTimeout), sizeof(recvTimeout));

        std::string pending;
        std::vector<char> buffer(1024);

        while (running_.load())
        {
            int received = recv(clientSocket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received > 0)
            {
                pending.append(buffer.data(), static_cast<size_t>(received));
                size_t newlinePos = std::string::npos;
                while ((newlinePos = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, newlinePos + 1);
                    pending.erase(0, newlinePos + 1);
                    line = TrimLineEndings(std::move(line));
                    line = SanitizeCommandLine(line);
                    if (!line.empty() && queue_)
                    {
                        queue_->Push(std::move(line));
                    }
                }
            }
            else if (received == 0)
            {
                break;
            }
            else
            {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT)
                {
                    // No data this cycle, still allow response flush.
                }
                else
                {
                    break;
                }
            }

            if (!queue_)
            {
                continue;
            }

            std::vector<std::string> responses = queue_->DrainResponses();
            for (const std::string &response : responses)
            {
                if (!SendResponse(clientSocket, response))
                {
                    return;
                }
            }
        }
    }

    void TcpServer::CloseSocket(SOCKET &socket)
    {
        if (socket != INVALID_SOCKET)
        {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
    }
}
