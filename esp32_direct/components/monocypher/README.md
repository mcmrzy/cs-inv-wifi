# Monocypher

Vendored from Monocypher 4.0.3, commit
`ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`.

Only the Ed25519 verification API is referenced by the firmware. ESP-IDF's
function/data section garbage collection removes the unused algorithms from the
final image.

Upstream: https://github.com/LoupVaillant/Monocypher

License: BSD-2-Clause OR CC0-1.0. See `LICENSE.md`.
