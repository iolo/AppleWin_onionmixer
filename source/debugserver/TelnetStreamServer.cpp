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

#include "StdAfx.h"

#include "TelnetStreamServer.h"
#include "DebugStreamProvider.h"

#include <algorithm>
#include <cstring>

namespace debugserver {

#ifdef _WIN32
bool TelnetStreamServer::s_wsaInitialized = false;
int TelnetStreamServer::s_wsaRefCount = 0;
std::mutex TelnetStreamServer::s_wsaMutex;

bool TelnetStreamServer::InitWinsock() {
    std::lock_guard<std::mutex> lock(s_wsaMutex);
    if (s_wsaInitialized) {
        s_wsaRefCount++;
        return true;
    }

    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        return false;
    }

    s_wsaInitialized = true;
    s_wsaRefCount = 1;
    return true;
}

void TelnetStreamServer::CleanupWinsock() {
    std::lock_guard<std::mutex> lock(s_wsaMutex);
    if (!s_wsaInitialized) return;

    s_wsaRefCount--;
    if (s_wsaRefCount <= 0) {
        WSACleanup();
        s_wsaInitialized = false;
        s_wsaRefCount = 0;
    }
}
#endif

TelnetStreamServer::TelnetStreamServer(uint16_t port, const std::string& bindAddress)
    : m_port(port)
    , m_bindAddress(bindAddress)
    , m_serverSocket(INVALID_SOCKET_VALUE)
    , m_running(false)
    , m_shouldStop(false)
    , m_provider(nullptr)
{
}

TelnetStreamServer::~TelnetStreamServer() {
    Stop();
}

bool TelnetStreamServer::Start() {
    if (m_running.load()) {
        return true;
    }

#ifdef _WIN32
    if (!InitWinsock()) {
        m_lastError = "Failed to initialize Winsock";
        return false;
    }
#endif

    if (!InitSocket()) {
#ifdef _WIN32
        CleanupWinsock();
#endif
        return false;
    }

    m_shouldStop.store(false);
    m_running.store(true);

    m_acceptThread = std::thread([this]() {
        AcceptLoop();
    });

    return true;
}

void TelnetStreamServer::Stop() {
    if (!m_running.load()) {
        return;
    }

    m_shouldStop.store(true);
    m_running.store(false);

    // Close server socket to unblock accept()
    CleanupSocket();

    // Wait for accept thread to finish
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (socket_t client : m_clients) {
            if (client != INVALID_SOCKET_VALUE) {
                CloseSocket(client);
            }
        }
        m_clients.clear();
    }

#ifdef _WIN32
    CleanupWinsock();
#endif
}

bool TelnetStreamServer::InitSocket() {
    m_serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_serverSocket == INVALID_SOCKET_VALUE) {
        m_lastError = "Failed to create socket: " + std::to_string(GetLastSocketError());
        return false;
    }

    if (!SetSocketReuseAddr(m_serverSocket)) {
        CloseSocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET_VALUE;
        return false;
    }

    sockaddr_in serverAddr;
    std::memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_bindAddress.c_str(), &serverAddr.sin_addr) != 1) {
        m_lastError = "Invalid bind address: " + m_bindAddress;
        CloseSocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET_VALUE;
        return false;
    }

    if (bind(m_serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR_VALUE) {
        m_lastError = "Failed to bind to port " + std::to_string(m_port) + ": " + std::to_string(GetLastSocketError());
        CloseSocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET_VALUE;
        return false;
    }

    if (listen(m_serverSocket, LISTEN_BACKLOG) == SOCKET_ERROR_VALUE) {
        m_lastError = "Failed to listen: " + std::to_string(GetLastSocketError());
        CloseSocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET_VALUE;
        return false;
    }

    return true;
}

void TelnetStreamServer::CleanupSocket() {
    if (m_serverSocket != INVALID_SOCKET_VALUE) {
        CloseSocket(m_serverSocket);
        m_serverSocket = INVALID_SOCKET_VALUE;
    }
}

bool TelnetStreamServer::SetSocketNonBlocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool TelnetStreamServer::SetSocketReuseAddr(socket_t sock) {
    int optval = 1;
#ifdef _WIN32
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&optval), sizeof(optval)) == 0;
#else
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                      &optval, sizeof(optval)) == 0;
#endif
}

void TelnetStreamServer::AcceptLoop() {
    // Set socket to non-blocking for periodic cleanup
    SetSocketNonBlocking(m_serverSocket);

    while (!m_shouldStop.load()) {
#ifdef _WIN32
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_serverSocket, &readSet);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms

        int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selectResult == SOCKET_ERROR_VALUE || m_shouldStop.load()) {
            break;
        }

        if (selectResult == 0) {
            // Timeout - check for dead clients
            CleanupDeadClients();
            continue;
        }
#else
        pollfd pfd;
        pfd.fd = m_serverSocket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pollResult = poll(&pfd, 1, 100);  // 100ms timeout
        if (pollResult < 0 || m_shouldStop.load()) {
            break;
        }

        if (pollResult == 0) {
            // Timeout - check for dead clients
            CleanupDeadClients();
            continue;
        }
#endif

        sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        socket_t clientSocket = accept(m_serverSocket,
                                       reinterpret_cast<sockaddr*>(&clientAddr),
                                       &clientAddrLen);

        if (clientSocket == INVALID_SOCKET_VALUE) {
            continue;
        }

        AddClient(clientSocket);
    }
}

void TelnetStreamServer::AddClient(socket_t clientSocket) {
    // Send telnet initialization
    SendTelnetInit(clientSocket);

    // Send welcome message with initial state
    SendWelcome(clientSocket);

    // Add to client list
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.push_back(clientSocket);
    }

    // Call connection callback if set
    if (m_onClientConnected) {
        m_onClientConnected(clientSocket);
    }
}

void TelnetStreamServer::CleanupDeadClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    m_clients.erase(
        std::remove_if(m_clients.begin(), m_clients.end(),
            [](socket_t sock) {
                if (sock == INVALID_SOCKET_VALUE) {
                    return true;
                }

                // Check if socket is still connected using recv with MSG_PEEK
                char buf;
#ifdef _WIN32
                int result = recv(sock, &buf, 1, MSG_PEEK);
                if (result == 0 || (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
#else
                int result = recv(sock, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
                if (result == 0 || (result == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
#endif
                    CloseSocket(sock);
                    return true;
                }
                return false;
            }),
        m_clients.end()
    );
}

void TelnetStreamServer::SendTelnetInit(socket_t clientSocket) {
    // Minimal Telnet negotiation:
    // IAC WILL ECHO (0xFF 0xFB 0x01) - Server will echo
    // IAC WILL SUPPRESS-GO-AHEAD (0xFF 0xFB 0x03) - Suppress go-ahead
    unsigned char initSeq[] = {
        0xFF, 0xFB, 0x01,  // IAC WILL ECHO
        0xFF, 0xFB, 0x03,  // IAC WILL SUPPRESS-GO-AHEAD
    };

    send(clientSocket, reinterpret_cast<const char*>(initSeq), sizeof(initSeq), 0);
}

void TelnetStreamServer::SendWelcome(socket_t clientSocket) {
    if (m_provider) {
        // Send hello message
        std::string hello = m_provider->GetHelloMessage();
        if (!hello.empty()) {
            hello += "\r\n";
            send(clientSocket, hello.c_str(), static_cast<int>(hello.size()), 0);
        }

        // Send initial snapshot
        auto snapshot = m_provider->GetFullSnapshot();
        for (const auto& line : snapshot) {
            std::string data = line + "\r\n";
            send(clientSocket, data.c_str(), static_cast<int>(data.size()), 0);
        }
    }
}

bool TelnetStreamServer::SendToClient(socket_t clientSocket, const std::string& data) {
    if (clientSocket == INVALID_SOCKET_VALUE) {
        return false;
    }

    std::string line = data;
    // Ensure line ends with \r\n for telnet compatibility
    if (line.empty() || line.back() != '\n') {
        line += "\r\n";
    } else if (line.size() >= 2 && line[line.size()-2] != '\r') {
        // Replace \n with \r\n
        line.insert(line.size()-1, "\r");
    }

    int totalSent = 0;
    int remaining = static_cast<int>(line.size());
    const char* ptr = line.c_str();

    while (remaining > 0) {
        int sent = send(clientSocket, ptr + totalSent, remaining, 0);
        if (sent == SOCKET_ERROR_VALUE) {
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }

    return true;
}

void TelnetStreamServer::Broadcast(const std::string& data) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    // Prepare data with proper line ending
    std::string line = data;
    if (line.empty() || line.back() != '\n') {
        line += "\r\n";
    } else if (line.size() >= 2 && line[line.size()-2] != '\r') {
        line.insert(line.size()-1, "\r");
    }

    std::vector<socket_t> deadClients;

    for (socket_t client : m_clients) {
        if (client != INVALID_SOCKET_VALUE) {
            int result = send(client, line.c_str(), static_cast<int>(line.size()), 0);
            if (result == SOCKET_ERROR_VALUE) {
                deadClients.push_back(client);
            }
        }
    }

    // Remove dead clients
    for (socket_t dead : deadClients) {
        m_clients.erase(
            std::remove(m_clients.begin(), m_clients.end(), dead),
            m_clients.end()
        );
        CloseSocket(dead);
    }
}

size_t TelnetStreamServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    return m_clients.size();
}

} // namespace debugserver
