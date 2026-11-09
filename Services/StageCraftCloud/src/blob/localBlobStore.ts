// Development blob store: a folder, plus HMAC-signed, expiring URLs served by GET /v1/blobs/{key} so the package download
// behaves like production (a redirect to a short-lived URL).
import { createHmac, timingSafeEqual } from 'node:crypto';
import { mkdir, readFile, rm, writeFile, rename, access } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { assertBlobKey, type BlobStore } from './blobStore.ts';

export class LocalBlobStore implements BlobStore {
  private readonly root: string;
  private readonly publicBaseUrl: string;
  private readonly secret: string;
  private readonly now: () => number;

  constructor(dir: string, publicBaseUrl: string, secret: string, now: () => number) {
    this.root = resolve(dir);
    this.publicBaseUrl = publicBaseUrl;
    this.secret = secret;
    this.now = now;
  }

  private path(key: string): string {
    assertBlobKey(key);
    return join(this.root, ...key.split('/'));
  }

  async put(key: string, data: Buffer): Promise<void> {
    const path = this.path(key);
    await mkdir(dirname(path), { recursive: true });
    const temporary = `${path}.${process.pid}.tmp`;
    await writeFile(temporary, data);
    await rename(temporary, path);
  }

  async get(key: string): Promise<Buffer | null> {
    try {
      return await readFile(this.path(key));
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
        return null;
      }
      throw error;
    }
  }

  async delete(key: string): Promise<void> {
    await rm(this.path(key), { force: true });
  }

  private signature(key: string, expires: number): Buffer {
    return createHmac('sha256', this.secret).update(`${key}\n${expires}`).digest();
  }

  async downloadUrl(key: string, ttlSeconds: number): Promise<string> {
    assertBlobKey(key);
    const expires = Math.floor(this.now() / 1000) + ttlSeconds;
    const sig = this.signature(key, expires).toString('hex');
    return `${this.publicBaseUrl}/v1/blobs/${key}?expires=${expires}&sig=${sig}`;
  }

  /** True when sig is this store's signature of key and expires has not passed. */
  verify(key: string, expires: string | null, sig: string | null): boolean {
    if (!expires || !sig || !/^[0-9]{1,12}$/.test(expires) || !/^[0-9a-f]{64}$/.test(sig)) {
      return false;
    }
    try {
      assertBlobKey(key);
    } catch {
      return false;
    }
    if (Number(expires) < Math.floor(this.now() / 1000)) {
      return false;
    }
    return timingSafeEqual(this.signature(key, Number(expires)), Buffer.from(sig, 'hex'));
  }

  async check(): Promise<void> {
    await mkdir(this.root, { recursive: true });
    await access(this.root);
  }
}
