/*
 * log_verbose.h — Diagnostics that survive between bugs.
 *
 * Every fault chased in this project so far needed the same thing: a running
 * commentary of what the client asked for and what came back. It was added by
 * hand each time and deleted once the bug closed, so the next one started from
 * nothing and the first hour went into re-adding it.
 *
 * SPOTIFYGTK_DEBUG() stays in the source permanently. It compiles to nothing
 * unless the build asks for it:
 *
 *     meson setup build -Dverbose_logging=true
 *
 * Prefer it over g_message for anything that is interesting while diagnosing
 * and noise otherwise -- request URIs, response statuses, set sizes, cache
 * decisions. Anything a user should see regardless stays g_message.
 */

#pragma once

#include <glib.h>

#ifdef SPOTIFYGTK_VERBOSE
# define SPOTIFYGTK_DEBUG(fmt, ...) \
    g_message ("[dbg] " fmt, ##__VA_ARGS__)
#else
# define SPOTIFYGTK_DEBUG(fmt, ...) \
    do { if (0) g_message ("[dbg] " fmt, ##__VA_ARGS__); } while (0)
#endif
