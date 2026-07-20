# OTA Ed25519 Signing V1

OTA releases use RFC 8032 PureEd25519. Devices contain only the 32-byte
release public key. The private key must remain in an offline release machine,
CI secret store, or HSM and must never be copied into firmware or Git.

The signed message is ASCII with LF line endings. The final LF is required:

```text
CS_INV_OTA_V1
target=esp
version=V1.0.6
size=524288
sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
security_version=6
```

Rules:

- `target`: exactly `esp`, `arm`, `dsp`, or `bms`.
- `version`: 1-31 ASCII characters from `[A-Za-z0-9._+-]`.
- `size`: uint32 decimal without leading formatting.
- `sha256`: exactly 64 lowercase hexadecimal characters.
- `security_version`: positive uint32 decimal and strictly monotonic.
- `signature`: RFC 4648 standard Base64 with padding. An Ed25519 signature is
  64 bytes and therefore encodes to exactly 88 characters ending in `==`.

Generate a signed command with:

```powershell
node tools/ota/sign_release.mjs `
  --firmware .pio/build/esp32-c3-devkitm-1/firmware.bin `
  --target esp `
  --version V1.0.6 `
  --security-version 6 `
  --url https://firmware.example/firmware.bin `
  --out release-command.json
```

The signer checks that the private key matches `keys/ota_ed25519_public.hex`
before producing a command.
