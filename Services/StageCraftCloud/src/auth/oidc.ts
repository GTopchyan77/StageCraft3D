// OpenID Connect sign-in (ADR 0032 §5, D6): authorization code with PKCE against any provider's discovery document, and RS256 ID
// token verification against its JWKS (signature, issuer, audience, expiry, nonce). No token claim ever grants a role.
import { createHash, createPublicKey, randomBytes, verify as verifySignature, type JsonWebKey, type KeyObject } from 'node:crypto';

export interface OidcOptions {
  issuer: string;
  clientId: string;
  clientSecret: string | null;
  redirectUri: string;
  scopes: string;
  now: () => number;
  fetchImpl?: typeof fetch;
}

interface Discovery {
  issuer: string;
  authorization_endpoint: string;
  token_endpoint: string;
  jwks_uri: string;
}

export interface IdentityClaims {
  issuer: string;
  subject: string;
  email: string | null;
  name: string | null;
}

export class OidcError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'OidcError';
  }
}

const CLOCK_SKEW_SECONDS = 60;
const DISCOVERY_TTL_MS = 3_600_000;
const JWKS_MIN_REFRESH_MS = 60_000;

export function pkcePair(): { verifier: string; challenge: string } {
  const verifier = randomBytes(32).toString('base64url');
  return { verifier, challenge: createHash('sha256').update(verifier).digest('base64url') };
}

function decodeSegment(segment: string): Record<string, unknown> {
  try {
    const value = JSON.parse(Buffer.from(segment, 'base64url').toString('utf8'));
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      throw new Error('not an object');
    }
    return value as Record<string, unknown>;
  } catch {
    throw new OidcError('malformed ID token');
  }
}

export class OidcClient {
  private readonly options: OidcOptions;
  private discovery: { value: Discovery; at: number } | null = null;
  private keys = new Map<string, KeyObject>();
  private keysFetchedAt = 0;

  constructor(options: OidcOptions) {
    this.options = options;
  }

  private fetch(): typeof fetch {
    return this.options.fetchImpl ?? fetch;
  }

  private async getJson(url: string, init?: RequestInit): Promise<Record<string, unknown>> {
    let response: Response;
    try {
      response = await this.fetch()(url, { ...init, signal: AbortSignal.timeout(10_000) });
    } catch {
      throw new OidcError(`identity provider unreachable (${new URL(url).host})`);
    }
    if (!response.ok) {
      throw new OidcError(`identity provider answered ${response.status}`);
    }
    try {
      return (await response.json()) as Record<string, unknown>;
    } catch {
      throw new OidcError('identity provider answered with invalid JSON');
    }
  }

  async getDiscovery(): Promise<Discovery> {
    if (this.discovery && this.options.now() - this.discovery.at < DISCOVERY_TTL_MS) {
      return this.discovery.value;
    }
    const doc = await this.getJson(`${this.options.issuer}/.well-known/openid-configuration`);
    const value = doc as unknown as Discovery;
    if (
      value.issuer !== this.options.issuer ||
      typeof value.authorization_endpoint !== 'string' ||
      typeof value.token_endpoint !== 'string' ||
      typeof value.jwks_uri !== 'string'
    ) {
      throw new OidcError('discovery document does not match the configured issuer');
    }
    this.discovery = { value, at: this.options.now() };
    return value;
  }

  async authorizationUrl(input: { state: string; nonce: string; codeChallenge: string }): Promise<string> {
    const discovery = await this.getDiscovery();
    const url = new URL(discovery.authorization_endpoint);
    url.searchParams.set('response_type', 'code');
    url.searchParams.set('client_id', this.options.clientId);
    url.searchParams.set('redirect_uri', this.options.redirectUri);
    url.searchParams.set('scope', this.options.scopes);
    url.searchParams.set('state', input.state);
    url.searchParams.set('nonce', input.nonce);
    url.searchParams.set('code_challenge', input.codeChallenge);
    url.searchParams.set('code_challenge_method', 'S256');
    return url.toString();
  }

  /** Exchanges the code and returns the verified identity. */
  async completeSignIn(code: string, codeVerifier: string, nonce: string): Promise<IdentityClaims> {
    const discovery = await this.getDiscovery();
    const form = new URLSearchParams({
      grant_type: 'authorization_code',
      code,
      redirect_uri: this.options.redirectUri,
      client_id: this.options.clientId,
      code_verifier: codeVerifier,
    });
    const headers: Record<string, string> = { 'Content-Type': 'application/x-www-form-urlencoded', Accept: 'application/json' };
    if (this.options.clientSecret) {
      headers.Authorization = `Basic ${Buffer.from(`${encodeURIComponent(this.options.clientId)}:${encodeURIComponent(this.options.clientSecret)}`).toString('base64')}`;
    }
    const tokens = await this.getJson(discovery.token_endpoint, { method: 'POST', headers, body: form.toString() });
    if (typeof tokens.id_token !== 'string') {
      throw new OidcError('token response has no id_token');
    }
    return this.verifyIdToken(tokens.id_token, nonce);
  }

  private async loadKeys(force: boolean): Promise<void> {
    if (!force && this.keys.size > 0) {
      return;
    }
    if (force && this.options.now() - this.keysFetchedAt < JWKS_MIN_REFRESH_MS) {
      return;
    }
    const discovery = await this.getDiscovery();
    const jwks = await this.getJson(discovery.jwks_uri);
    const keys = new Map<string, KeyObject>();
    for (const jwk of Array.isArray(jwks.keys) ? (jwks.keys as Record<string, unknown>[]) : []) {
      if (jwk.kty !== 'RSA' || typeof jwk.kid !== 'string' || (jwk.use !== undefined && jwk.use !== 'sig')) {
        continue;
      }
      try {
        keys.set(jwk.kid, createPublicKey({ key: jwk as JsonWebKey, format: 'jwk' }));
      } catch {
        // A key this runtime cannot import is skipped; a token signed with it fails as "unknown key".
      }
    }
    this.keys = keys;
    this.keysFetchedAt = this.options.now();
  }

  async verifyIdToken(token: string, expectedNonce: string): Promise<IdentityClaims> {
    const parts = token.split('.');
    if (parts.length !== 3) {
      throw new OidcError('malformed ID token');
    }
    const header = decodeSegment(parts[0]!);
    const claims = decodeSegment(parts[1]!);
    // Only RS256: "none", HS256 (key confusion) and anything else are refused before any key is touched.
    if (header.alg !== 'RS256' || typeof header.kid !== 'string') {
      throw new OidcError('ID token must be RS256 with a key id');
    }
    await this.loadKeys(false);
    let key = this.keys.get(header.kid);
    if (!key) {
      await this.loadKeys(true);
      key = this.keys.get(header.kid);
    }
    if (!key) {
      throw new OidcError('ID token signed with an unknown key');
    }
    const signed = Buffer.from(`${parts[0]}.${parts[1]}`, 'utf8');
    if (!verifySignature('RSA-SHA256', signed, key, Buffer.from(parts[2]!, 'base64url'))) {
      throw new OidcError('ID token signature is invalid');
    }
    const nowSeconds = Math.floor(this.options.now() / 1000);
    if (claims.iss !== this.options.issuer) {
      throw new OidcError('ID token issuer mismatch');
    }
    const audiences = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
    if (!audiences.includes(this.options.clientId) || (audiences.length > 1 && claims.azp !== this.options.clientId)) {
      throw new OidcError('ID token audience mismatch');
    }
    if (typeof claims.exp !== 'number' || claims.exp + CLOCK_SKEW_SECONDS < nowSeconds) {
      throw new OidcError('ID token expired');
    }
    if (typeof claims.iat === 'number' && claims.iat - CLOCK_SKEW_SECONDS > nowSeconds) {
      throw new OidcError('ID token issued in the future');
    }
    if (claims.nonce !== expectedNonce) {
      throw new OidcError('ID token nonce mismatch');
    }
    if (typeof claims.sub !== 'string' || claims.sub.length === 0 || claims.sub.length > 255) {
      throw new OidcError('ID token has no subject');
    }
    const email = typeof claims.email === 'string' && claims.email_verified !== false ? claims.email : null;
    const name = typeof claims.name === 'string' ? claims.name : null;
    return { issuer: this.options.issuer, subject: claims.sub, email, name };
  }
}
