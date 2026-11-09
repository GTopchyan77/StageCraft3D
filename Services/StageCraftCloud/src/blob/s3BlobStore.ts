// Production blob store: S3 through pre-signed URLs (5-minute downloads, ADR 0032 §4). Credentials come from the environment, EKS
// Pod Identity (container credentials endpoint) or IRSA (web identity token exchanged with STS), refreshed before they expire.
// Written to the documented S3/STS REST contracts; not yet exercised against real AWS (see docs/RUNBOOK.md).
import { readFile } from 'node:fs/promises';
import { assertBlobKey, type BlobStore } from './blobStore.ts';
import { presignUrl, uriEncode, type AwsCredentials } from './sigv4.ts';

export interface CredentialProvider {
  get(): Promise<AwsCredentials>;
}

interface Cached {
  credentials: AwsCredentials;
  expiresAt: number;
}

type Env = Record<string, string | undefined>;

/** Static keys from the environment, else Pod Identity, else IRSA. Refreshes five minutes before expiry. */
export function defaultCredentialProvider(env: Env, now: () => number, fetchImpl: typeof fetch = fetch): CredentialProvider {
  let cached: Cached | null = null;
  const load = async (): Promise<Cached> => {
    if (env.AWS_ACCESS_KEY_ID && env.AWS_SECRET_ACCESS_KEY) {
      return {
        credentials: { accessKeyId: env.AWS_ACCESS_KEY_ID, secretAccessKey: env.AWS_SECRET_ACCESS_KEY, sessionToken: env.AWS_SESSION_TOKEN ?? null },
        expiresAt: Number.POSITIVE_INFINITY,
      };
    }
    if (env.AWS_CONTAINER_CREDENTIALS_FULL_URI) {
      const headers: Record<string, string> = {};
      if (env.AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE) {
        headers.Authorization = (await readFile(env.AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE, 'utf8')).trim();
      }
      const response = await fetchImpl(env.AWS_CONTAINER_CREDENTIALS_FULL_URI, { headers, signal: AbortSignal.timeout(5000) });
      if (!response.ok) {
        throw new Error(`container credentials endpoint answered ${response.status}`);
      }
      const body = (await response.json()) as Record<string, string>;
      return {
        credentials: { accessKeyId: body.AccessKeyId!, secretAccessKey: body.SecretAccessKey!, sessionToken: body.Token ?? null },
        expiresAt: Date.parse(body.Expiration ?? '') || now() + 15 * 60_000,
      };
    }
    if (env.AWS_WEB_IDENTITY_TOKEN_FILE && env.AWS_ROLE_ARN) {
      const token = (await readFile(env.AWS_WEB_IDENTITY_TOKEN_FILE, 'utf8')).trim();
      const region = env.AWS_REGION ?? 'us-east-1';
      const query = new URLSearchParams({
        Action: 'AssumeRoleWithWebIdentity',
        Version: '2011-06-15',
        RoleArn: env.AWS_ROLE_ARN,
        RoleSessionName: 'stagecraft-cloud',
        WebIdentityToken: token,
      });
      const response = await fetchImpl(`https://sts.${region}.amazonaws.com/?${query}`, { signal: AbortSignal.timeout(5000) });
      const xml = await response.text();
      const field = (name: string): string => {
        const match = new RegExp(`<${name}>([^<]+)</${name}>`).exec(xml);
        if (!response.ok || !match) {
          throw new Error(`STS AssumeRoleWithWebIdentity failed (${response.status})`);
        }
        return match[1]!;
      };
      return {
        credentials: { accessKeyId: field('AccessKeyId'), secretAccessKey: field('SecretAccessKey'), sessionToken: field('SessionToken') },
        expiresAt: Date.parse(field('Expiration')),
      };
    }
    throw new Error('no AWS credentials: set static keys, EKS Pod Identity or IRSA');
  };
  return {
    async get() {
      if (!cached || cached.expiresAt - 5 * 60_000 <= now()) {
        cached = await load();
      }
      return cached.credentials;
    },
  };
}

export interface S3Options {
  bucket: string;
  region: string;
  /** Path-style endpoint for S3-compatible stores (MinIO, LocalStack); null = virtual-hosted AWS. */
  endpoint: string | null;
  prefix: string;
  credentials: CredentialProvider;
  now: () => number;
  fetchImpl?: typeof fetch;
}

export class S3BlobStore implements BlobStore {
  private readonly options: S3Options;

  constructor(options: S3Options) {
    this.options = options;
  }

  objectUrl(key: string): string {
    assertBlobKey(key);
    const path = `${this.options.prefix}${key}`.split('/').map((segment) => uriEncode(segment)).join('/');
    return this.options.endpoint
      ? `${this.options.endpoint}/${this.options.bucket}/${path}`
      : `https://${this.options.bucket}.s3.${this.options.region}.amazonaws.com/${path}`;
  }

  private async sign(method: 'GET' | 'PUT' | 'DELETE' | 'HEAD', key: string, expiresSeconds: number, query?: Record<string, string>): Promise<string> {
    return presignUrl({
      method,
      url: this.objectUrl(key),
      region: this.options.region,
      credentials: await this.options.credentials.get(),
      now: this.options.now(),
      expiresSeconds,
      query,
    });
  }

  private fetch(): typeof fetch {
    return this.options.fetchImpl ?? fetch;
  }

  async put(key: string, data: Buffer, contentType: string): Promise<void> {
    const response = await this.fetch()(await this.sign('PUT', key, 60), {
      method: 'PUT',
      headers: { 'Content-Type': contentType },
      body: data,
      signal: AbortSignal.timeout(120_000),
    });
    if (!response.ok) {
      throw new Error(`S3 PUT answered ${response.status}`);
    }
  }

  async get(key: string): Promise<Buffer | null> {
    const response = await this.fetch()(await this.sign('GET', key, 60), { signal: AbortSignal.timeout(60_000) });
    if (response.status === 404 || response.status === 403) {
      // Without s3:ListBucket a missing object answers 403; both mean "not there" to the caller.
      return null;
    }
    if (!response.ok) {
      throw new Error(`S3 GET answered ${response.status}`);
    }
    return Buffer.from(await response.arrayBuffer());
  }

  async delete(key: string): Promise<void> {
    const response = await this.fetch()(await this.sign('DELETE', key, 60), { method: 'DELETE', signal: AbortSignal.timeout(30_000) });
    if (!response.ok && response.status !== 404) {
      throw new Error(`S3 DELETE answered ${response.status}`);
    }
  }

  async downloadUrl(key: string, ttlSeconds: number, fileName: string): Promise<string> {
    const safeName = fileName.replace(/[^A-Za-z0-9._-]/g, '_');
    return this.sign('GET', key, ttlSeconds, { 'response-content-disposition': `attachment; filename="${safeName}"` });
  }

  async check(): Promise<void> {
    await this.options.credentials.get();
  }
}
