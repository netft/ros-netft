#pragma once

#ifdef _WIN32
#if defined(NETFT_BUILDING_LIBRARY)
#define NETFT_API __declspec(dllexport)
#elif defined(NETFT_USING_LIBRARY)
#define NETFT_API __declspec(dllimport)
#else
#define NETFT_API
#endif
#define NETFT_LOCAL
#else
#define NETFT_API __attribute__((visibility("default")))
#define NETFT_LOCAL __attribute__((visibility("hidden")))
#endif
