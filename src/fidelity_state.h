// SPDX-License-Identifier: GPL-2.0-or-later
/* Read-only, selected gameplay observations for disposable fidelity sessions. */
#ifndef KFX_FIDELITY_STATE_H
#define KFX_FIDELITY_STATE_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Game;
/* Writes versioned canonical JSON; never modifies Game or invokes simulation. */
int fidelity_write_snapshot(FILE *output, const struct Game *state);
/* Creates a new file exclusively. Removes only its own incomplete output. */
int fidelity_write_snapshot_path(const char *path, const struct Game *state);
#ifdef __cplusplus
}
#endif
#endif
