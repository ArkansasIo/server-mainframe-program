'use strict';

const crypto = require('crypto');

/**
 * Password hashing. Uses scrypt (available in node core, no native build).
 * Stored format: scrypt$<N>$<r>$<p>$<saltHex>$<hashHex>
 */

const PARAMS = { N: 16384, r: 8, p: 1, keylen: 64 };

function hashPassword(password, salt = crypto.randomBytes(16)) {
  const { N, r, p, keylen } = PARAMS;
  const derived = crypto.scryptSync(String(password), salt, keylen, { N, r, p });
  return `scrypt$${N}$${r}$${p}$${salt.toString('hex')}$${derived.toString('hex')}`;
}

/**
 * Verify a password against a stored hash. Also understands the legacy
 * "sha256:<seed>" placeholder strings used by sql/seed.sql so that seeded
 * accounts can be recognised as "not usable for logon".
 */
function verifyPassword(password, stored) {
  if (!stored) return false;

  if (stored.startsWith('scrypt$')) {
    const parts = stored.split('$');
    if (parts.length !== 6) return false;
    const [, N, r, p, saltHex, hashHex] = parts;
    try {
      const derived = crypto.scryptSync(String(password), Buffer.from(saltHex, 'hex'), hashHex.length / 2, {
        N: Number(N), r: Number(r), p: Number(p),
      });
      return crypto.timingSafeEqual(derived, Buffer.from(hashHex, 'hex'));
    } catch {
      return false;
    }
  }

  if (stored.startsWith('sha256:')) {
    // Seed placeholders are deliberately unusable for interactive logon.
    return false;
  }

  // Plain-text fallback (development only).
  return crypto.timingSafeEqual(
    Buffer.from(String(password)),
    Buffer.from(String(stored)),
  ) && password.length === stored.length;
}

/** SHA-256 hex digest. */
function sha256(value) {
  return crypto.createHash('sha256').update(String(value)).digest('hex');
}

/** Constant-time string comparison for API keys and tokens. */
function safeEqual(a, b) {
  const bufA = Buffer.from(String(a ?? ''));
  const bufB = Buffer.from(String(b ?? ''));
  if (bufA.length !== bufB.length) return false;
  return crypto.timingSafeEqual(bufA, bufB);
}

module.exports = { hashPassword, verifyPassword, sha256, safeEqual, PARAMS };
