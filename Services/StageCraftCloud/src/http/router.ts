// A small router over node:http: path patterns, per-route body limits, auth level and rate limits, security headers, request ids,
// one access-log line per request, and the {error:{code,message}} shape for every failure (ADR 0032 §4.1).
import { randomUUID } from 'node:crypto';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { ApiError } from '../errors.ts';
import type { Logger } from '../log.ts';
import { RatePolicies, type RateLimiter, type RatePolicyName } from './rateLimit.ts';

export type AuthLevel = 'none' | 'optional' | 'user' | 'admin';

export interface Principal {
  accountId: string;
  roles: string[];
  sessionHash: string;
}

export interface Reply {
  status: number;
  json?: unknown;
  body?: Buffer | string;
  contentType?: string;
  headers?: Record<string, string>;
}

export interface RequestContext {
  readonly req: IncomingMessage;
  readonly method: string;
  readonly path: string;
  readonly query: URLSearchParams;
  readonly params: Record<string, string>;
  readonly requestId: string;
  readonly ip: string;
  readonly principal: Principal | null;
  /** The raw body, at most the route's limit (413 TooLarge beyond it). */
  body(): Promise<Buffer>;
  /** The body parsed as a JSON object; 415 unless Content-Type is application/json, 400 for anything but an object. */
  json(): Promise<Record<string, unknown>>;
  /** Requires a signed-in principal (for 'optional' routes that need one in some branches). */
  requireUser(): Principal;
}

export type Handler = (context: RequestContext) => Promise<Reply>;

export interface RateRule {
  policy: RatePolicyName;
  by: 'ip' | 'account';
}

export interface RouteOptions {
  auth?: AuthLevel;
  /** Bytes; default 64 KiB. */
  bodyLimit?: number;
  limits?: RateRule[];
  /** Skip the default per-IP limit (signed webhooks from the provider's many addresses). */
  skipIpLimit?: boolean;
}

interface Route extends Required<Omit<RouteOptions, 'limits'>> {
  method: string;
  pattern: string;
  regex: RegExp;
  keys: string[];
  handler: Handler;
  limits: RateRule[];
}

export interface RouterDeps {
  log: Logger;
  limiter: RateLimiter;
  /** Resolves a bearer token to a principal, or null. */
  authenticate(token: string): Promise<Principal | null>;
  trustProxyHops: number;
}

const DEFAULT_BODY_LIMIT = 64 * 1024;

const SECURITY_HEADERS: Record<string, string> = {
  'X-Content-Type-Options': 'nosniff',
  'X-Frame-Options': 'DENY',
  'Referrer-Policy': 'no-referrer',
  'Strict-Transport-Security': 'max-age=63072000; includeSubDomains',
  'Content-Security-Policy': "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'",
  'Cross-Origin-Resource-Policy': 'same-origin',
  'Permissions-Policy': 'interest-cohort=()',
};

function compile(pattern: string): { regex: RegExp; keys: string[] } {
  const keys: string[] = [];
  const source = pattern
    .split('/')
    .map((segment) => {
      if (segment.startsWith(':')) {
        keys.push(segment.slice(1));
        return '([^/]+)';
      }
      if (segment === '*') {
        keys.push('rest');
        return '(.+)';
      }
      return segment.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    })
    .join('/');
  return { regex: new RegExp(`^${source}$`), keys };
}

/** The client address: the socket, or with N trusted proxy hops (the ALB) the Nth address from the right of X-Forwarded-For. */
export function clientIp(req: IncomingMessage, trustProxyHops: number): string {
  const socketIp = req.socket.remoteAddress ?? 'unknown';
  if (trustProxyHops <= 0) {
    return socketIp;
  }
  const header = req.headers['x-forwarded-for'];
  const chain = (Array.isArray(header) ? header.join(',') : header ?? '').split(',').map((part) => part.trim()).filter(Boolean);
  return chain.length >= trustProxyHops ? chain[chain.length - trustProxyHops]! : socketIp;
}

function serialize(value: unknown): string {
  return JSON.stringify(value, (_key, entry) => {
    if (typeof entry === 'bigint') {
      if (entry > BigInt(Number.MAX_SAFE_INTEGER) || entry < BigInt(Number.MIN_SAFE_INTEGER)) {
        throw new RangeError('amount exceeds the JSON-safe integer range');
      }
      return Number(entry);
    }
    return entry;
  });
}

export function errorReply(error: ApiError): Reply {
  return { status: error.status, json: { error: { code: error.code, message: error.message } }, headers: error.headers };
}

/** A refused body up to this size is read and discarded, so the client can finish sending and read the error; beyond it the
 * connection is closed instead (we never read megabytes we already refused). */
const DRAIN_MAX = 2 * 1024 * 1024;

function declaredLength(req: IncomingMessage): number | null {
  const declared = req.headers['content-length'];
  return declared !== undefined && /^[0-9]{1,15}$/.test(declared) ? Number(declared) : null;
}

async function drain(req: IncomingMessage): Promise<void> {
  let size = 0;
  for await (const chunk of req) {
    size += (chunk as Buffer).length;
    if (size > DRAIN_MAX) {
      throw new Error('too much to drain');
    }
  }
}

async function readBody(req: IncomingMessage, limit: number): Promise<Buffer> {
  const tooLarge = new ApiError('TooLarge', `body is over the ${limit}-byte limit for this route`);
  const declared = declaredLength(req);
  if (req.headers['content-length'] !== undefined && declared === null) {
    throw new ApiError('InvalidRequest', 'invalid Content-Length');
  }
  if (declared !== null && declared > limit) {
    throw tooLarge;
  }
  const chunks: Buffer[] = [];
  let size = 0;
  let overflow = false;
  for await (const chunk of req) {
    size += (chunk as Buffer).length;
    if (size > limit + DRAIN_MAX) {
      throw tooLarge; // leaving the loop destroys the request stream
    }
    if (size > limit) {
      overflow = true;
      continue;
    }
    chunks.push(chunk as Buffer);
  }
  if (overflow) {
    throw tooLarge;
  }
  return Buffer.concat(chunks, size);
}

export class Router {
  private readonly routes: Route[] = [];
  private readonly deps: RouterDeps;

  constructor(deps: RouterDeps) {
    this.deps = deps;
  }

  add(method: string, pattern: string, handler: Handler, options: RouteOptions = {}): void {
    const { regex, keys } = compile(pattern);
    this.routes.push({
      method,
      pattern,
      regex,
      keys,
      handler,
      auth: options.auth ?? 'none',
      bodyLimit: options.bodyLimit ?? DEFAULT_BODY_LIMIT,
      limits: options.limits ?? [],
      skipIpLimit: options.skipIpLimit ?? false,
    });
  }

  private match(method: string, path: string): { route: Route; params: Record<string, string> } | 'method' | null {
    let pathMatched = false;
    for (const route of this.routes) {
      const match = route.regex.exec(path);
      if (!match) continue;
      pathMatched = true;
      if (route.method !== method) continue;
      const params: Record<string, string> = {};
      route.keys.forEach((key, index) => {
        params[key] = decodeURIComponent(match[index + 1]!);
      });
      return { route, params };
    }
    return pathMatched ? 'method' : null;
  }

  private send(res: ServerResponse, requestId: string, reply: Reply): void {
    if (res.headersSent) {
      return;
    }
    const headers: Record<string, string> = { ...SECURITY_HEADERS, 'X-Request-Id': requestId, 'Cache-Control': 'no-store', ...(reply.headers ?? {}) };
    let body: Buffer | string = '';
    if (reply.json !== undefined) {
      body = serialize(reply.json);
      headers['Content-Type'] = 'application/json; charset=utf-8';
    } else if (reply.body !== undefined) {
      body = reply.body;
      headers['Content-Type'] = reply.contentType ?? 'application/octet-stream';
    }
    headers['Content-Length'] = String(Buffer.byteLength(body));
    res.writeHead(reply.status, headers);
    res.end(body);
  }

  private rateLimit(rule: RateRule, key: string): void {
    const result = this.deps.limiter.take(rule.policy, RatePolicies[rule.policy], key);
    if (!result.ok) {
      throw new ApiError('RateLimited', 'too many requests; try again later', { 'Retry-After': String(result.retryAfterSeconds) });
    }
  }

  async handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const started = performance.now();
    const requestId = randomUUID();
    const method = req.method ?? 'GET';
    let url: URL;
    try {
      url = new URL(req.url ?? '/', 'http://localhost');
    } catch {
      url = new URL('http://localhost/');
    }
    const ip = clientIp(req, this.deps.trustProxyHops);
    let routePattern = 'unmatched';
    let principal: Principal | null = null;
    let reply: Reply;
    try {
      const found = this.match(method, url.pathname);
      if (found === null) {
        throw new ApiError('NotFound', 'no such endpoint');
      }
      if (found === 'method') {
        throw new ApiError('MethodNotAllowed', 'method not allowed on this endpoint');
      }
      const { route, params } = found;
      routePattern = route.pattern;
      if (!route.skipIpLimit) {
        this.rateLimit({ policy: 'ipDefault', by: 'ip' }, ip);
      }

      const authorization = req.headers.authorization;
      if (authorization && route.auth !== 'none') {
        const match = /^Bearer ([A-Za-z0-9_-]{16,256})$/.exec(authorization);
        principal = match ? await this.deps.authenticate(match[1]!) : null;
        if (!principal) {
          throw new ApiError('Unauthenticated', 'session is missing, expired or revoked');
        }
      }
      if ((route.auth === 'user' || route.auth === 'admin') && !principal) {
        throw new ApiError('Unauthenticated', 'sign in first');
      }
      if (route.auth === 'admin' && !principal!.roles.includes('admin')) {
        throw new ApiError('Forbidden', 'admin role required');
      }
      if (principal) {
        this.rateLimit({ policy: 'accountDefault', by: 'account' }, principal.accountId);
      }
      for (const rule of route.limits) {
        if (rule.by === 'account' && principal) {
          this.rateLimit(rule, principal.accountId);
        } else if (rule.by === 'ip' && !principal) {
          this.rateLimit(rule, ip);
        }
      }

      let bodyPromise: Promise<Buffer> | null = null;
      const currentPrincipal = principal;
      const context: RequestContext = {
        req,
        method,
        path: url.pathname,
        query: url.searchParams,
        params,
        requestId,
        ip,
        principal: currentPrincipal,
        body: () => (bodyPromise ??= readBody(req, route.bodyLimit)),
        json: async () => {
          const contentType = req.headers['content-type'] ?? '';
          if (!/^application\/json\b/i.test(contentType)) {
            throw new ApiError('UnsupportedMediaType', 'Content-Type must be application/json');
          }
          const raw = await context.body();
          let parsed: unknown;
          try {
            parsed = JSON.parse(raw.toString('utf8'));
          } catch {
            throw new ApiError('InvalidRequest', 'body is not valid JSON');
          }
          if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
            throw new ApiError('InvalidRequest', 'body must be a JSON object');
          }
          return parsed as Record<string, unknown>;
        },
        requireUser: () => {
          if (!currentPrincipal) {
            throw new ApiError('Unauthenticated', 'sign in first');
          }
          return currentPrincipal;
        },
      };
      reply = await route.handler(context);
    } catch (error) {
      if (error instanceof ApiError) {
        reply = errorReply(error);
      } else {
        this.deps.log.error('unhandled error', { requestId, route: routePattern, error });
        reply = errorReply(new ApiError('Internal', `internal error (request ${requestId})`));
      }
    }
    // A body the handler did not read: drain a small one so the client sees the answer; close the connection for a large one.
    if (!req.readableEnded && !req.destroyed) {
      const declared = declaredLength(req);
      let drained = false;
      // No Content-Length: no body (a GET) or a chunked one, which drain() stops reading past DRAIN_MAX.
      if (declared === null || declared <= DRAIN_MAX) {
        drained = await drain(req).then(
          () => true,
          () => false,
        );
      }
      if (!drained) {
        reply.headers = { ...(reply.headers ?? {}), Connection: 'close' };
      }
    }
    this.send(res, requestId, reply);
    if (reply.headers?.Connection === 'close') {
      res.on('finish', () => req.destroy());
    }
    this.deps.log.info('request', {
      requestId,
      method,
      route: routePattern,
      status: reply.status,
      ms: Math.round(performance.now() - started),
      account: principal?.accountId ?? null,
    });
  }
}
