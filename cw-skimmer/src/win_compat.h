/**
 * Forced onto the C sources only when the build host is Windows
 * (-include src/win_compat.h). Winsock has to be included before windows.h.
 * Linux builds do not use this file.
 */
#ifndef CWSK_WIN_COMPAT_H
#define CWSK_WIN_COMPAT_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* Winsock has no SIGPIPE, and it rejects these Linux send flags. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#endif /* CWSK_WIN_COMPAT_H */
