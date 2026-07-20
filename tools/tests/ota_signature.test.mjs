import assert from 'node:assert/strict';
import { createPrivateKey, createPublicKey, sign, verify } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { canonicalOtaMessage } from '../ota/ota_signature.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = (relativePath) => readFileSync(resolve(root, relativePath), 'utf8');

function publicKeyBytesFromHeader(relativePath) {
  const source = read(relativePath);
  const initializer = source.match(
    /OTA_RELEASE_PUBLIC_KEY\s*\[\s*32\s*\]\s*=\s*\{([\s\S]*?)\}/,
  );
  assert.ok(initializer, `${relativePath} must define a 32-byte OTA release key`);
  const bytes = [...initializer[1].matchAll(/0x([0-9a-fA-F]{2})/g)]
    .map((match) => Number.parseInt(match[1], 16));
  assert.equal(bytes.length, 32, `${relativePath} must contain exactly 32 key bytes`);
  return Buffer.from(bytes);
}

const seed = Buffer.from(
  '9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
  'hex',
);
const privateKey = createPrivateKey({
  key: Buffer.concat([
    Buffer.from('302e020100300506032b657004220420', 'hex'),
    seed,
  ]),
  format: 'der',
  type: 'pkcs8',
});

test('OTA canonical message and PureEd25519 signature match the fixed vector', () => {
  const message = canonicalOtaMessage({
    target: 'esp',
    version: 'V1.0.6',
    size: 524288,
    sha256: '0123456789abcdef'.repeat(4),
    securityVersion: 6,
  });
  assert.equal(
    message.toString('ascii'),
    'CS_INV_OTA_V1\n'
      + 'target=esp\n'
      + 'version=V1.0.6\n'
      + 'size=524288\n'
      + `sha256=${'0123456789abcdef'.repeat(4)}\n`
      + 'security_version=6\n',
  );
  assert.equal(message.length, 143);
  const signature = sign(null, message, privateKey);
  assert.equal(signature.toString('base64'),
    'By1Jj32RGLFgN4AzPb9pMYo5VoMWvxu24+WsroLHzaQjPJIr6uybXtcv/T45ouK4ubM0t/f0LVsmA2av1X1JCA==');
  assert.equal(verify(null, message, createPublicKey(privateKey), signature), true);
});

test('committed Ed25519 public key matches both firmware byte arrays', () => {
  const publicKeyHex = read('keys/ota_ed25519_public.hex').trim();
  assert.match(publicKeyHex, /^[0-9a-f]{64}$/,
    'release public key must be 32 lowercase hexadecimal bytes');
  const expected = Buffer.from(publicKeyHex, 'hex');

  for (const header of [
    'src/ota_release_pubkey.h',
    'esp32c2_idf/main/ota_release_pubkey.h',
  ]) {
    assert.deepEqual(publicKeyBytesFromHeader(header), expected,
      `${header} has drifted from keys/ota_ed25519_public.hex`);
  }
});

test('OTA canonicalizer rejects ambiguous representations', () => {
  const valid = {
    target: 'esp',
    version: 'V1.0.6',
    size: 524288,
    sha256: '0123456789abcdef'.repeat(4),
    securityVersion: 6,
  };
  assert.throws(() => canonicalOtaMessage({ ...valid, target: 'ESP' }));
  assert.throws(() => canonicalOtaMessage({ ...valid, version: 'bad\nversion' }));
  assert.throws(() => canonicalOtaMessage({ ...valid, size: 1.5 }));
  assert.throws(() => canonicalOtaMessage({ ...valid, sha256: valid.sha256.toUpperCase() }));
  assert.throws(() => canonicalOtaMessage({ ...valid, securityVersion: 0 }));
});

test('fixed Ed25519 signature rejects every signed manifest field mutation', () => {
  const valid = {
    target: 'esp',
    version: 'V1.0.6',
    size: 524288,
    sha256: '0123456789abcdef'.repeat(4),
    securityVersion: 6,
  };
  const message = canonicalOtaMessage(valid);
  const signature = sign(null, message, privateKey);
  const publicKey = createPublicKey(privateKey);

  for (const changed of [
    { ...valid, target: 'arm' },
    { ...valid, version: 'V1.0.7' },
    { ...valid, size: valid.size + 1 },
    { ...valid, sha256: `1${valid.sha256.slice(1)}` },
    { ...valid, securityVersion: valid.securityVersion + 1 },
  ]) {
    assert.equal(verify(null, canonicalOtaMessage(changed), publicKey, signature), false);
  }
});

test('ESP32-C2 verifier is a real Ed25519 implementation, not a success stub', () => {
  const source = read('esp32c2_idf/main/ota_signature.c');
  assert.doesNotMatch(source, /Ed25519\s+verification\s+removed/i);
  assert.doesNotMatch(source, /\(void\)\s*signature_b64[\s\S]{0,200}return\s+true\s*;/);
  assert.match(source, /psa_verify_message\s*\(|mbedtls_pk_verify\s*\(|crypto_ed25519_check\s*\(/,
    'C2 must call a cryptographic signature verification API');
  assert.match(source, /signature_len\s*!=\s*64|sizeof\s*\([^)]*signature[^)]*\)\s*!=\s*64/,
    'C2 must require a 64-byte Ed25519 signature');
});
