// AWS Signature Version 4 query-string signing (pre-signed URLs) for S3, written on node:crypto.
// Verified against the example in AWS's "Authenticating Requests: Using Query Parameters (AWS Signature Version 4)".
import { createHash, createHmac } from 'node:crypto';

export interface AwsCredentials {
  accessKeyId: string;
  secretAccessKey: string;
  sessionToken?: string | null;
}

export interface PresignInput {
  method: 'GET' | 'PUT' | 'DELETE' | 'HEAD';
  /** Full URL without query, e.g. https://bucket.s3.us-east-1.amazonaws.com/key */
  url: string;
  region: string;
  service?: string;
  credentials: AwsCredentials;
  /** Epoch ms of signing. */
  now: number;
  expiresSeconds: number;
  /** Extra query parameters to sign (e.g. response-content-disposition). */
  query?: Record<string, string>;
}

/** RFC 3986 encoding as SigV4 wants it (encodeURIComponent leaves !'()* alone). */
export function uriEncode(value: string, encodeSlash = true): string {
  const encoded = encodeURIComponent(value).replace(/[!'()*]/g, (char) => `%${char.charCodeAt(0).toString(16).toUpperCase()}`);
  return encodeSlash ? encoded : encoded.replace(/%2F/g, '/');
}

function hmac(key: Buffer | string, data: string): Buffer {
  return createHmac('sha256', key).update(data, 'utf8').digest();
}

export function amzDate(now: number): string {
  return new Date(now).toISOString().replace(/[-:]/g, '').replace(/\.\d{3}/, '');
}

export function presignUrl(input: PresignInput): string {
  const service = input.service ?? 's3';
  const url = new URL(input.url);
  const datetime = amzDate(input.now);
  const date = datetime.slice(0, 8);
  const scope = `${date}/${input.region}/${service}/aws4_request`;

  const params: Record<string, string> = {
    ...(input.query ?? {}),
    'X-Amz-Algorithm': 'AWS4-HMAC-SHA256',
    'X-Amz-Credential': `${input.credentials.accessKeyId}/${scope}`,
    'X-Amz-Date': datetime,
    'X-Amz-Expires': String(input.expiresSeconds),
    'X-Amz-SignedHeaders': 'host',
  };
  if (input.credentials.sessionToken) {
    params['X-Amz-Security-Token'] = input.credentials.sessionToken;
  }
  const canonicalQuery = Object.keys(params)
    .sort()
    .map((key) => `${uriEncode(key)}=${uriEncode(params[key]!)}`)
    .join('&');
  // The path is already percent-encoded by URL; decode then re-encode each segment the SigV4 way.
  const canonicalPath = url.pathname
    .split('/')
    .map((segment) => uriEncode(decodeURIComponent(segment)))
    .join('/');
  const canonicalRequest = [input.method, canonicalPath, canonicalQuery, `host:${url.host}`, '', 'host', 'UNSIGNED-PAYLOAD'].join('\n');
  const stringToSign = [
    'AWS4-HMAC-SHA256',
    datetime,
    scope,
    createHash('sha256').update(canonicalRequest, 'utf8').digest('hex'),
  ].join('\n');
  const signingKey = hmac(hmac(hmac(hmac(`AWS4${input.credentials.secretAccessKey}`, date), input.region), service), 'aws4_request');
  const signature = createHmac('sha256', signingKey).update(stringToSign, 'utf8').digest('hex');
  return `${url.origin}${canonicalPath}?${canonicalQuery}&X-Amz-Signature=${signature}`;
}
