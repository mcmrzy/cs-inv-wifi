const targetPattern = /^(esp|arm|dsp|bms)$/;
const versionPattern = /^[A-Za-z0-9._+-]{1,31}$/;
const sha256Pattern = /^[0-9a-f]{64}$/;

export function canonicalOtaMessage({
  target,
  version,
  size,
  sha256,
  securityVersion,
}) {
  if (!targetPattern.test(target)) throw new Error('Invalid OTA target');
  if (!versionPattern.test(version)) throw new Error('Invalid OTA version');
  if (!Number.isSafeInteger(size) || size <= 0 || size > 0xffffffff) {
    throw new Error('Invalid OTA size');
  }
  if (!sha256Pattern.test(sha256)) throw new Error('Invalid SHA-256');
  if (
    !Number.isSafeInteger(securityVersion) ||
    securityVersion <= 0 ||
    securityVersion > 0xffffffff
  ) {
    throw new Error('Invalid OTA security version');
  }

  return Buffer.from(
    `CS_INV_OTA_V1\n` +
      `target=${target}\n` +
      `version=${version}\n` +
      `size=${size}\n` +
      `sha256=${sha256}\n` +
      `security_version=${securityVersion}\n`,
    'ascii',
  );
}
