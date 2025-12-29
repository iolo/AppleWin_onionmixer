/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2024, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*
 * Telnet Stream Server
 * Provides real-time debug output streaming via Telnet-compatible TCP connection.
 * Outputs JSON Lines format according to OUTPUT_SPEC_V01.md specification.
 */

#pragma once

#include "SocketUtils.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

namespace debugserver {

// Forward declaration
class DebugStreamProvider;

/**
 * TelnetStreamServer - Telnet-compatible debug streaming server
 *
 * Features:
 * - Telnet protocol compatible (minimal negotiation)
 * - Multi-client support with broadcast capability
 * - JSON Lines format output (one JSON object per line)
 * - Thread-safe broadcasting
 * - Automatic client cleanup on disconnect
 *
 * Usage:
 *   TelnetStreamServer server(65505);
 *   server.SetProvider(provider);
 *   server.Start();
 *   // ... during emulation ...
 *   server.Broadcast(jsonLine);
 *   // ... on shutdown ...
 *   server.Stop();
 */
class TelnetStreamServer {
public:
    // Callback type for new client connections
    using OnClientConnected = std::function<void(socket_t clientSocket)>;

    // Constructor with port and optional bind address
    explicit TelnetStreamServer(uint16_t port, const std::string& bindAddress = "127.0.0.1");
    ~TelnetStreamServer();

    // Non-copyable
    TelnetStreamServer(const TelnetStreamServer&) = delete;
    TelnetStreamServer& operator=(const TelnetStreamServer&) = delete;

    // Set the provider for initial snapshot on connection
    void SetProvider(DebugStreamProvider* provider) { m_provider = provider; }

    // Set callback for new client connections
    void SetOnClientConnected(OnClientConnected callback) { m_onClientConnected = callback; }

    // Start the server
    bool Start();

    // Stop the server
    void Stop();

    // Check if server is running
    bool IsRunning() const { return m_running.load(); }

    // Get the port
    uint16_t GetPort() const { return m_port; }

    // Get the bind address
    const std::string& GetBindAddress() const { return m_bindAddress; }

    // Get last error message
    const std::string& GetLastError() const { return m_lastError; }

    // Broadcast data to all connected clients (thread-safe)
    void Broadcast(const std::string& data);

    // Get number of connected clients
    size_t GetClientCount() const;

private:
    // Main accept loop (runs in separate thread)
    void AcceptLoop();

    // Add a new client
    void AddClient(socket_t clientSocket);

    // Remove disconnected clients
    void CleanupDeadClients();

    // Send data to a specific client
    bool SendToClient(socket_t clientSocket, const std::string& data);

    // Send telnet initialization sequence
    void SendTelnetInit(socket_t clientSocket);

    // Send welcome message with initial state
    void SendWelcome(socket_t clientSocket);

    // Socket operations
    bool InitSocket();
    void CleanupSocket();
    bool SetSocketNonBlocking(socket_t sock);
    bool SetSocketReuseAddr(socket_t sock);

#ifdef _WIN32
    // Windows socket initialization
    static bool InitWinsock();
    static void CleanupWinsock();
    static bool s_wsaInitialized;
    static int s_wsaRefCount;
    static std::mutex s_wsaMutex;
#endif

    uint16_t m_port;
    std::string m_bindAddress;
    socket_t m_serverSocket;
    std::thread m_acceptThread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_shouldStop;
    std::string m_lastError;

    // Provider for generating initial state
    DebugStreamProvider* m_provider;

    // Callback for new client connections
    OnClientConnected m_onClientConnected;

    // Client management
    mutable std::mutex m_clientsMutex;
    std::vector<socket_t> m_clients;

    // Constants
    static constexpr int LISTEN_BACKLOG = 5;
    static constexpr size_t SEND_BUFFER_SIZE = 4096;
};

} // namespace debugserver
