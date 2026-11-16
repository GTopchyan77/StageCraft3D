// StageCraft Pro licence tokens (ADR 0034 §17.2): SC1.<base64url(payload)>.<base64url(Ed25519 signature)>, the signature over the
// ASCII bytes "SC1.<base64url(payload)>". The client (StageLicenceToken.cpp) verifies them offline with the public key compiled into
// it, so the payload layout below is a contract: never rename a field, add new ones instead.
import { createPrivateKey, createPublicKey, sign, verify, type KeyObject } from 'node:crypto';

export const TOKEN_PREFIX = 'SC1.';
export const TOKEN_VERSION = 1;

/** The exact entitlement tags a Pro plan grants (StageCraft.Entitlement.Pro.*; ADR 0034 §2.1). */
export const PRO_ENTITLEMENTS = [
  'StageCraft.Entitlement.Pro.DmxUniverses',
  'StageCraft.Entitlement.Pro.DmxIn',
  'StageCraft.Entitlement.Pro.MidiOsc',
  'StageCraft.Entitlement.Pro.WebRemote',
  'StageCraft.Entitlement.Pro.MvrExport',
] as const;

/**
 * Development only: signs tokens on development and test servers. Its public key is trusted by non-Shipping clients only, and a
 * production server refuses to start with it (config.ts), so publishing it here gives nobody a Pro licence on a real build.
 */
export const DEV_KEY_ID = 'dev-1';
export const DEV_SIGNING_KEY_PKCS8 = 'MC4CAQAwBQYDK2VwBCIEIJRsisQ5mwqcV0+MHG77BVSyGi00d1Y3b0F+5w754JSA';

export interface LicencePayload {
  v: number;
  kid: string;
  /** Account id. */
  sub: string;
  /** The machine digest the client sent (64 lowercase hex). */
  mid: string;
  plan: string;
  ent: string[];
  /** Unix seconds. */
  iat: number;
  exp: number;
}

export interface LicenceSigner {
  readonly keyId: string;
  sign(payload: Omit<LicencePayload, 'v' | 'kid'>): string;
  /** The raw 32-byte public key, for the client's trusted list. */
  publicKeyRaw(): Buffer;
}

/** Loads a PKCS#8 (DER, base64) Ed25519 private key. Throws when it is not one. */
export function loadSigningKey(pkcs8Base64: string): KeyObject {
  const key = createPrivateKey({ key: Buffer.from(pkcs8Base64, 'base64'), format: 'der', type: 'pkcs8' });
  if (key.asymmetricKeyType !== 'ed25519') {
    throw new Error('LICENCE_SIGNING_KEY is not an Ed25519 key');
  }
  return key;
}

export function createSigner(keyId: string, privateKey: KeyObject): LicenceSigner {
  const publicKey = createPublicKey(privateKey);
  return {
    keyId,
    sign(claims) {
      const payload: LicencePayload = { v: TOKEN_VERSION, kid: keyId, ...claims };
      const signingInput = TOKEN_PREFIX + Buffer.from(JSON.stringify(payload), 'utf8').toString('base64url');
      const signature = sign(null, Buffer.from(signingInput, 'ascii'), privateKey);
      return `${signingInput}.${signature.toString('base64url')}`;
    },
    publicKeyRaw() {
      return Buffer.from(publicKey.export({ format: 'jwk' }).x as string, 'base64url');
    },
  };
}

/** Server-side check of a token this server issued (tests and support). Null when the signature or layout is wrong. */
export function verifyToken(token: string, publicKeyRaw: Buffer): LicencePayload | null {
  if (!token.startsWith(TOKEN_PREFIX)) {
    return null;
  }
  const dot = token.lastIndexOf('.');
  if (dot <= TOKEN_PREFIX.length) {
    return null;
  }
  const signingInput = token.slice(0, dot);
  const publicKey = createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: publicKeyRaw.toString('base64url') }, format: 'jwk' });
  if (!verify(null, Buffer.from(signingInput, 'ascii'), publicKey, Buffer.from(token.slice(dot + 1), 'base64url'))) {
    return null;
  }
  try {
    return JSON.parse(Buffer.from(signingInput.slice(TOKEN_PREFIX.length), 'base64url').toString('utf8')) as LicencePayload;
  } catch {
    return null;
  }
}
