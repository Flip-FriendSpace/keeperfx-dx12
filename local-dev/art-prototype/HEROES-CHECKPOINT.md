# Original hero sprite checkpoint

Status: `roster_visual_review_accepted`. The user accepted the roster visual review on 2026-10-15: "This looks great". Ordinary campaign play remains pending. The durable record is [prototype.json](prototype.json).

The thirteen original heroes have 1,491 enhanced overhead frames and 78 held frames. Together with the accepted imp, the local package contains 1,688 overhead frames, 84 held frames and 158 animation entries. Other possession views and shared effects retain original art. World size, animation counts, timing and anchors are preserved.

Individual action fixtures, static frame audits, browser controls, the combined DX12 diagnostic and a matched 168-creature benchmark were checked locally. Detailed package hashes, executable hashes, captures, failed-attempt dispositions and reports remain under the workspace's `artifacts/art-prototype/heroes/`; they are not distributed here. The benchmark covers a short matched scene at 1920x1080, not every campaign or display scanout FPS. Broad OpenGL crowd performance, cold loading and total process/VRAM savings remain unqualified.

Use [README.md](README.md) for local prerequisites and direct PowerShell launch commands, and [WORKFLOW.md](WORKFLOW.md) for the maintained method. Preserve the accepted artwork and apply only requested corrections; repack and rerun affected checks after deliberate changes. Do not infer new user acceptance from successful automation. Other creature batches require a new instruction.

Large-sprite support accepts source axes up to 1024 and world shadow sources up to 512. OpenGL retains its bounded atlas with fewer cached entries; software reduces only large shadow masks while preserving sprite detail. Original data, extracted/generated pixels, runtime ZIPs and reports remain local and outside Git. Preserve upstream licences and asset notices. Only one controlled test may own a runtime at a time; never interrupt an unrelated game.
