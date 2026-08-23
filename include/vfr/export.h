/*
 * export.h - VFRS Symbol Visibility & DLL Export/Import Macros
 * Virtual Frame Relay Switch
 */

#ifndef VFR_EXPORT_H
#define VFR_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
    #if defined(VFR_BUILD_DLL)
        /* Building dynamic library: export symbols */
        #define VFR_API __declspec(dllexport)
    #elif defined(VFR_USE_DLL)
        /* Consuming dynamic library: import symbols */
        #define VFR_API __declspec(dllimport)
    #else
        /* Monolithic executable or static library */
        #define VFR_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define VFR_API __attribute__((visibility("default")))
    #else
        #define VFR_API
    #endif
#endif

#endif /* VFR_EXPORT_H */
