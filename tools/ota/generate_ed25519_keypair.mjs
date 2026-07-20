import { generateKeyPairSync, createHash, sign, verify } from 'node:crypto';
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..', '..');
const privatePath = resolve(root, '.secrets', 'ota_ed25519_private.pem');
const publicPath = resolve(root, 'keys', 'ota_ed25519_public.pem');
const rawPublicPath = resolve(root, 'keys', 'ota_ed25519_public.hex');
const fingerprintPath = resolve(root, 'keys', 'ota_ed25519_public.sha256');

for (const path of [privatePath, publicPath, rawPublicPath, fingerprintPath]) {
  mkdirSync(dirname(path), { recursive: true });
}

const { privateKey, publicKey } = generateKeyPairSync('ed25519');
const privatePem = privateKey.export({ type: 'pkcs8', format: 'pem' });
const publicPem = publicKey.export({ type: 'spki', format: 'pem' });
const publicJwk = publicKey.export({ format: 'jwk' });
const rawPublic = Buffer.from(publicJwk.x, 'base64url');

if (rawPublic.length !== 32) {
  throw new Error(`Unexpected Ed25519 public key length: ${rawPublic.length}`);
}

const probe = Buffer.from('CS_INV_OTA_KEY_SELF_TEST\n', 'ascii');
const probeSignature = sign(null, probe, privateKey);
if (probeSignature.length !== 64 || !verify(null, probe, publicKey, probeSignature)) {
  throw new Error('Generated Ed25519 key pair failed its self-test');
}

const fingerprint = createHash('sha256').update(rawPublic).digest('hex');

writeFileSync(privatePath, privatePem, { flag: 'wx', mode: 0o600 });
writeFileSync(publicPath, publicPem, { flag: 'wx', mode: 0o644 });
writeFileSync(rawPublicPath, `${rawPublic.toString('hex')}\n`, {
  flag: 'wx',
  mode: 0o644,
});
writeFileSync(fingerprintPath, `${fingerprint}\n`, {
  flag: 'wx',
  mode: 0o644,
});

process.stdout.write([
  'Ed25519 key pair generated and self-tested.',
  `Private key: ${privatePath}`,
  `Public key: ${publicPath}`,
  `Raw public key: ${rawPublicPath}`,
  `SHA-256 fingerprint: ${fingerprint}`,
].join('\n') + '\n');
