/* Backend-independent lock logic: password buffer, key state machine, PAM. */
#ifndef DARK_SCREENLOCK_CORE_H
#define DARK_SCREENLOCK_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const char core_keys_help[];

/* Resolves the current user and pins the password buffer in RAM. */
bool core_init(void);

/* mlock() is not inherited across fork(); call in the daemonized child. */
void core_after_fork(void);

/*
 * Feed one key press. Keysyms use X11/xkbcommon numbering (identical).
 *   base: keysym of the key at shift level 0 in the active layout, lowercased
 *   sym:  keysym with the current modifiers applied
 *   text: UTF-8 the key produces (len 0 if none)
 * Returns true once a submitted password has been accepted by PAM.
 */
bool core_key(uint32_t base, uint32_t sym, bool ctrl, bool alt,
	      const char *text, size_t len);

void core_wipe(void);

#endif
