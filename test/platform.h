// Test-only socket/OS portability glue. The test binaries probe for a free
// TCP port with raw sockets; this wraps that tiny POSIX surface so the tests
// also build on Windows (winsock2) via MSYS2/MinGW.
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
static inline int sock_close(int fd) { return closesocket(fd); }
static inline void socket_lib_init()
{
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
static inline int sock_close(int fd) { return close(fd); }
static inline void socket_lib_init() {}
#endif

#endif
