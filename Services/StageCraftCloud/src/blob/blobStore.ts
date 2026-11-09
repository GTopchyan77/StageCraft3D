// Package, thumbnail and screenshot storage. Local folder in development, S3 in production (ADR 0032 §4).

export interface BlobStore {
  put(key: string, data: Buffer, contentType: string): Promise<void>;
  /** The bytes, or null when there is no such blob. */
  get(key: string): Promise<Buffer | null>;
  delete(key: string): Promise<void>;
  /** A short-lived URL a client can GET the blob from without a session. */
  downloadUrl(key: string, ttlSeconds: number, fileName: string): Promise<string>;
  /** Readiness: can the store be reached. */
  check(): Promise<void>;
}

const KEY_PATTERN = /^[a-z0-9][a-z0-9._-]*(\/[a-z0-9][a-z0-9._-]*)*$/;

/** Keys are built by the service from ids; anything else (.., absolute paths, odd characters) is a programming error. */
export function assertBlobKey(key: string): void {
  if (!KEY_PATTERN.test(key) || key.includes('..') || key.length > 256) {
    throw new RangeError(`invalid blob key: ${key}`);
  }
}

export const BlobKeys = {
  package: (listingId: string) => `packages/${listingId.toLowerCase()}.stage`,
  thumbnail: (listingId: string) => `thumbnails/${listingId.toLowerCase()}.png`,
  screenshot: (ticketId: string) => `support/${ticketId.toLowerCase()}.png`,
};
