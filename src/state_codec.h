// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_STATE_CODEC_H
#define KFX_STATE_CODEC_H

#include <stddef.h>
#include <stdint.h>

// This fork's state protocol is deliberately incompatible with raw-memory peers.
// LuaJIT bytecode and simulation determinism are not interchangeable between
// x86 and x64 sessions. The C state schema itself remains architecture neutral.
#if UINTPTR_MAX > UINT32_MAX
#define KFX_STATE_PROTOCOL UINT32_C(0x4b466401)
#else
#define KFX_STATE_PROTOCOL UINT32_C(0x4b465801)
#endif
#define KFX_STATE_VERSION 1
#define KFX_STATE_HEADER_SIZE 32
#define KFX_STATE_MAX_SIZE (96u * 1024u * 1024u)

enum KfxStateKind {
    KfxState_Game = 1,
    KfxState_Intralevel = 2,
    KfxState_Catalogue = 3,
    KfxState_PacketHeader = 4,
    KfxState_Continue = 5,
    KfxState_Navigation = 6,
};

#ifdef __cplusplus
extern "C" {
#endif
struct Game;
// Fixed schema sizes include the version/schema/checksum header. Zero = unknown kind.
size_t kfx_state_size(enum KfxStateKind kind);
// malloc-owned output, released with free(). Input state is never modified.
int kfx_state_encode(enum KfxStateKind kind, const void *state, unsigned char **data, size_t *length);
// Validation checks framing, schema, length and checksum without changing state.
int kfx_state_validate(enum KfxStateKind kind, const void *data, size_t length);
// Decode into staging memory; destination is untouched on failure.
int kfx_state_decode(enum KfxStateKind kind, void *state, const void *data, size_t length);
// Copies an already decoded Game and rebinds its internal dungeon references.
void kfx_state_copy_game(struct Game *destination, const struct Game *source);
uint32_t kfx_state_read_u32(const void *data);
void kfx_state_write_u32(void *data, uint32_t value);
uint32_t kfx_state_checksum(const void *data, size_t length);
#ifdef __cplusplus
}
#endif
#endif
