// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once

#include "bflib_basics.h"
#include <cstdint>

class IUIRenderer;
struct SpriteScale;

namespace kfx::render {

// Synchronous game-thread destination for the callbacks invoked by
// ProduceWorldDraws. Borrowed source arguments must be copied before returning;
// neither engine pointers nor deferred callbacks belong in an owned frame.
class WorldSubmissionCapture {
public:
    virtual ~WorldSubmissionCapture() = default;
    virtual void BeginSpriteBucket(int32_t bucket) = 0;
    virtual void CaptureKeeperSprite(const SpriteScale* scale, const uint8_t* data,
        int width, int height, int32_t content_height, unsigned int draw_flags,
        const uint8_t* remap, int32_t sprite_id, int owner, bool wants_outline,
        uint8_t alpha) = 0;

    // This renderer must implement every drawing entry point it accepts. In
    // particular, inheriting IUIRenderer's immediate software drawing defaults
    // is not a capture implementation. Unsupported draws must reject the frame.
    virtual IUIRenderer& UI() = 0;
    virtual TbBool DrawTextResized(int32_t x, int32_t y, int32_t units_per_px,
        const char* text) = 0;
};

// Install only around one synchronous world traversal. A nested scope or an
// active custom software lens is rejected; callers must check Active() before
// dispatching. Destruction restores normal routing, including on exceptions.
// The capture must outlive the scope, on the same game thread.
class ScopedWorldSubmissionCapture {
public:
    explicit ScopedWorldSubmissionCapture(WorldSubmissionCapture& capture) noexcept;
    ~ScopedWorldSubmissionCapture();
    ScopedWorldSubmissionCapture(const ScopedWorldSubmissionCapture&) = delete;
    ScopedWorldSubmissionCapture& operator=(const ScopedWorldSubmissionCapture&) = delete;
    ScopedWorldSubmissionCapture(ScopedWorldSubmissionCapture&&) = delete;
    ScopedWorldSubmissionCapture& operator=(ScopedWorldSubmissionCapture&&) = delete;
    bool Active() const noexcept { return m_capture != nullptr; }
private:
    WorldSubmissionCapture* m_capture = nullptr;
};

} // namespace kfx::render
