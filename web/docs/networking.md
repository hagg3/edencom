# Networking & World Sharing (Web Port)

Read [`../../docs/networking.md`](../../docs/networking.md) for the client/protocol
this section is about — it is **not currently ported**, not reimplemented, and not
unchanged; treat world sharing as absent in the web build.

## Status: deferred, not replaced
All six world-sharing files are seam-excluded and explicitly marked "NOT YET
REPLACED" in `CMakeLists.txt`'s `EDEN_SEAM_EXCLUDE` comments: `FileDownload.mm`,
`FileUpload.mm`, `SharedList.mm`, `ShareUtil.mm`, `ShareMenu.mm`, `Alert.mm`
(`Alert.mm`'s dialog *semantics* are reused by the DOM alert seams — see
[ui.md](ui.md) — but the file itself isn't compiled). This is a locked scope
decision, not an oversight: the web port's plan treats world-sharing networking as
out of scope for now, with "feature-flag Shared Worlds off if the endpoint can't be
reached" as an acceptable outcome.

`src/shim/foundation/NSURLConnection.h` exists only as a header-only link-time stub —
no `.mm`, no implementation.

## What's stripped, not deferred
`Classes/Appirater.mm` (the App Store rating nag) and Flurry analytics are stripped
entirely, per root `CLAUDE.md` — their one caller (`EdenAppDelegate.mm`) is itself
seam-excluded, so there's nothing to wire up even for a stub.

## External reference, not a crib source
The developer's own separately-hosted live web build (a from-scratch portable-C++
rewrite, not this ObjC++ engine — zero `objc_msgSend`) confirms the world-sharing
endpoints referenced in root docs/networking.md are still live, but it's a different
codebase and not a source to port code from directly.
