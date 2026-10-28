// unistd.h — G.3's experimental MSVC-ABI (clang-cl) Windows leg only; on the include path only when
// MSVC (native/CMakeLists.txt). The UCRT has the POSIX calls the port uses under their old names or
// in <io.h>/<direct.h>/<process.h>; this header is the smallest bridge. MSYS2 CLANG64 (the shipping
// Windows build) has a real unistd.h and never sees this file.
#ifndef EDEN_COMPAT_MSVC_UNISTD_H
#define EDEN_COMPAT_MSVC_UNISTD_H
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdlib.h>
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define F_OK 0
#endif
#endif
