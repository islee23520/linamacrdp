# Linardp

Linardp is a Rust-based open-source macOS RDP host intended to replace Remotr on this computer. It is a fork of [clintcan/macrdp](https://github.com/clintcan/macrdp), which already serves the active desktop to standard RDP clients with authenticated TLS/NLA sessions, clipboard sharing and system sound. This fork's remaining requirements are Korean/English Caps Lock input and client microphone redirection as a macOS input device.

The implementation stack is Rust with IronRDP for RDP sessions and macOS platform APIs for screen capture, input events, pasteboard and audio. The fork retains its upstream licenses and attribution. Unimplemented capabilities are not advertised as working.

The first release targets a signed and notarized distribution outside the Mac App Store when a Developer ID Application identity is available. The current Keychain has Apple Distribution but no Developer ID Application identity; these are not interchangeable. A later Mac App Store submission requires replacing the private `CGVirtualDisplay` path and assessing sandbox and entitlement compatibility. This fork does not claim store readiness.

## Current verification boundary

The inherited desktop, clipboard and system-sound paths compile and their existing tests pass. Korean input-source switching and the new `AUDIO_INPUT` receiver/virtual Core Audio microphone are implemented on the feature branch, but no successful interactive RDP session or client-to-Mac recording has been observed on this host. The virtual microphone driver requires an administrator install in `/Library/Audio/Plug-Ins/HAL`; the user deferred that step. Remotr remains the active service until a replacement passes real-session QA.
