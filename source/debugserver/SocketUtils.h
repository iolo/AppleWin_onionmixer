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
 * Socket Utilities
 * Common socket type definitions and utilities for cross-platform networking.
 */

#pragma once

// Platform-specific socket includes
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
    #pragma comment(lib, "ws2_32.lib")
    #endif
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
#endif

namespace debugserver {

// Socket type abstraction
#ifdef _WIN32
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
    constexpr int SOCKET_ERROR_VALUE = SOCKET_ERROR;
    constexpr int SEND_FLAGS = 0;  // Windows doesn't need special flags
    inline int CloseSocket(socket_t s) { return closesocket(s); }
    inline int GetLastSocketError() { return WSAGetLastError(); }
#else
    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_VALUE = -1;
    constexpr int SOCKET_ERROR_VALUE = -1;
    // MSG_NOSIGNAL prevents SIGPIPE signal when client disconnects
    // Without this, writing to a closed socket kills the process
    constexpr int SEND_FLAGS = MSG_NOSIGNAL;
    inline int CloseSocket(socket_t s) { return close(s); }
    inline int GetLastSocketError() { return errno; }
#endif

// Safe send wrapper that uses appropriate flags for each platform
inline ssize_t SafeSend(socket_t s, const void* buf, size_t len) {
    return send(s, static_cast<const char*>(buf), static_cast<int>(len), SEND_FLAGS);
}

} // namespace debugserver
