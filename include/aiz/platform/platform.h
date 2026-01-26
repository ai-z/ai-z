#pragma once

#if defined(_WIN32) || defined(_WIN64)
#ifndef AI_Z_PLATFORM_WINDOWS
#define AI_Z_PLATFORM_WINDOWS 1
#endif
#endif

#if defined(__linux__)
#ifndef AI_Z_PLATFORM_LINUX
#define AI_Z_PLATFORM_LINUX 1
#endif
#endif
