# Render-thread window-message regression

This Windows-only test compiles the actual `RenderThreadManager` and thread
identity sources. It requires the dedicated `keeperfx-dev` WSL distribution and
its MinGW Win32-thread-model compiler. It does not need game data, SDL, OpenGL,
audio devices or a running game. Its message-only window is never shown.

```powershell
./local-dev/render-thread-test.ps1 -OutputDirectory C:/Temp/keeperfx-render-wait-test -Architecture both
```

Use a new output directory each time. The runner preserves individual logs and
a source-hashed JSON report. Each process has an external watchdog; it terminates
only the test process it created.

Initialization, frame work, presentation followed by another frame, presentation
followed by shutdown, cleanup and restart each synchronously message the window
owner from the render thread. `SendMessageTimeoutW` bounds the send, so code that
does not service sent messages fails a delivery assertion. The window procedure
calls `IsWorkDone()` to catch a wait dispatching messages while holding the
manager mutex. A posted keyboard message must remain queued for the normal
input loop.

To verify the regression against the original source, add `-ExpectFailure`.
That mode requires every case to fail specifically at the bounded synchronous
message assertion. It does not accept arbitrary crashes or watchdog timeouts.
This synthetic test proves the window-message wait contract; live renderer
startup and shutdown still need their own smoke checks.
