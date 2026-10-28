// strings.h — G.3's experimental MSVC-ABI (clang-cl) leg only (see unistd.h beside it).
#ifndef EDEN_COMPAT_MSVC_STRINGS_H
#define EDEN_COMPAT_MSVC_STRINGS_H
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
