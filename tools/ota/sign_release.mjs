import {
  createHash,
  createPrivateKey,
  createPublicKey,
  randomUUID,
  sign,
  verify,
} from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { canonicalOtaMessage } from './ota_signature.mjs';

function parseArgs(argv) {
  const result = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i];
    if (!key?.startsWith('--') || argv[i + 1] === undefined) {
      throw new Error(`Invalid argument near ${key ?? '<end>'}`);
    }
    result[key.slice(2)] = argv[i + 1];
  }
  return result;
}

const args = parseArgs(process.argv.slice(2));
for (const required of ['firmware', 'target', 'version', 'security-version', 'url']) {
  if (!args[required]) throw new Error(`Missing --${required}`);
}

const root = resolve(import.meta.dirname, '..', '..');
const firmwarePath = resolve(args.firmware);
const privateKeyPath = resolve(
  args['private-key'] ?? resolve(root, '.secrets', 'ota_ed25519_private.pem'),
);
const expectedPublicHex = readFileSync(
  resolve(root, 'keys', 'ota_ed25519_public.hex'),
  'utf8',
).trim();
const firmware = readFileSync(firmwarePath);
const privateKey = createPrivateKey(readFileSync(privateKeyPath));
const publicKey = createPublicKey(privateKey);
const publicJwk = publicKey.export({ format: 'jwk' });
const actualPublicHex = Buffer.from(publicJwk.x, 'base64url').toString('hex');

if (actualPublicHex !== expectedPublicHex) {
  throw new Error('Private key does not match the repository release public key');
}

const securityVersion = Number(args['security-version']);
const sha256 = createHash('sha256').update(firmware).digest('hex');
const message = canonicalOtaMessage({
  target: args.target,
  version: args.version,
  size: firmware.length,
  sha256,
  securityVersion,
});
const signature = sign(null, message, privateKey);
if (!verify(null, message, publicKey, signature)) {
  throw new Error('Generated OTA signature failed local verification');
}

const timeout = args.timeout === undefined ? 300 : Number(args.timeout);
if (!Number.isInteger(timeout) || timeout < 60 || timeout > 3600) {
  throw new Error('Invalid --timeout; expected an integer from 60 to 3600');
}

const command = {
  t: Math.floor(Date.now() / 1000),
  v: 1,
  task_id: args['task-id'] ?? randomUUID(),
  action: 'start',
  target: args.target,
  url: args.url,
  version: args.version,
  size: firmware.length,
  sha256,
  security_version: securityVersion,
  signature: signature.toString('base64'),
  timeout,
};
const output = `${JSON.stringify(command, null, 2)}\n`;
if (args.out) writeFileSync(resolve(args.out), output, { flag: 'wx', mode: 0o600 });
else process.stdout.write(output);
