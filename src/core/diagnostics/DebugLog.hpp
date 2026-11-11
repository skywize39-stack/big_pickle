#pragma once
#include <cstdio>

// ------------------------------------------------------------------
//  Debug console logging
//
//  Every console diagnostic in the project (startup, config, rage,
//  doubletap, ESP / PaintTraverse, night mode, ...) goes through DBGLOG,
//  which is compiled out by default. The entry point also creates the
//  console only when logging is enabled, so a normal build is fully
//  silent: no console window, no console output. Only the loader that
//  injected the DLL stays visible.
//
//  Flip CSSO_DEBUG_LOG to 1 (or add it to the project preprocessor
//  definitions) to bring the console and every diagnostic back at once,
//  without touching a single call site.
// ------------------------------------------------------------------
#ifndef CSSO_DEBUG_LOG
#define CSSO_DEBUG_LOG 0
#endif

#if CSSO_DEBUG_LOG
#define DBGLOG( ... )  std::printf( "[INFO] " __VA_ARGS__ )
#define DBGLOG_WARN( ... )  std::printf( "[WARN] " __VA_ARGS__ )
#define DBGLOG_ERROR( ... )  std::printf( "[ERROR] " __VA_ARGS__ )
#define DBGLOG_DEBUG( ... )  std::printf( "[DEBUG] " __VA_ARGS__ )
#else
#define DBGLOG( ... )  ( (void)0 )
#define DBGLOG_WARN( ... )  ( (void)0 )
#define DBGLOG_ERROR( ... )  ( (void)0 )
#define DBGLOG_DEBUG( ... )  ( (void)0 )
#endif

