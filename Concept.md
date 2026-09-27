# Linardp

Linardp is a Rust-based open-source macOS RDP host intended to replace Remotr on this computer. It is a fork of [clintcan/macrdp](https://github.com/clintcan/macrdp), which already serves the active desktop to standard RDP clients with authenticated TLS/NLA sessions, clipboard sharing and system sound. This fork's remaining requirements are Korean/English Caps Lock input and client microphone redirection as a macOS input device.

The implementation stack is Rust with IronRDP for RDP sessions and macOS platform APIs for screen capture, input events, pasteboard and audio. The fork retains its upstream licenses and attribution. Unimplemented capabilities are not advertised as working.

The first release targets a signed and notarized distribution outside the Mac App Store when a Developer ID Application identity is available. A later Mac App Store submission needs a separate sandbox and entitlement feasibility review.
