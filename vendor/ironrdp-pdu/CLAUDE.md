# vendored ironrdp-pdu

Upstream: Devolutions/IronRDP@a5d1c682fd1f65287f6f5727bdead2aef7a2bea7 (same rev as the root git pins).

## Divergences

1. `gcc::KeyboardType::Korean = 8` - Korean-layout Windows mstsc sends keyboardType 8 in Client Core Data. Upstream only knows 1-7, so decoding failed with "invalid keyboard type" right after a successful CredSSP login and Windows could never connect.

Drop this vendor copy once upstream accepts the value.
