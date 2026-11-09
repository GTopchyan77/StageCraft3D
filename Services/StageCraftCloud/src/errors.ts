// Stable error codes the UE client maps to EStageMarketplaceResult / EStageEconomyResult (ADR 0032 §4.1).
// Codes named like a client enum value use exactly that name. Never rename a code: the client branches on it.

export const ErrorStatus = {
  // Generic
  InvalidRequest: 400,
  Unauthenticated: 401,
  Forbidden: 403,
  NotFound: 404,
  MethodNotAllowed: 405,
  UnsupportedMediaType: 415,
  RateLimited: 429,
  Internal: 500,
  BackendUnavailable: 503,
  // Packages (EStageMarketplaceResult names)
  Corrupt: 422,
  VersionMismatch: 422,
  TooLarge: 413,
  HashMismatch: 422,
  NotPortable: 422,
  NotOriginal: 422,
  AlreadyExists: 409,
  NotOwned: 403,
  NotYours: 403,
  // Commerce (EStageEconomyResult names where one exists)
  AlreadyOwned: 409,
  PurchasePending: 409,
  NotForSale: 409,
  CurrencyNotSupported: 422,
  SellerNotOnboarded: 409,
  IdempotencyConflict: 409,
  ProviderError: 502,
  IllegalTransition: 409,
  RefundExceedsCharge: 422,
  // Webhooks
  InvalidSignature: 400,
  // RFC 8628 device-flow errors (kept in RFC spelling so the client can follow the RFC)
  authorization_pending: 400,
  slow_down: 400,
  expired_token: 400,
  access_denied: 400,
  invalid_grant: 400,
} as const;

export type ErrorCode = keyof typeof ErrorStatus;

export class ApiError extends Error {
  readonly code: ErrorCode;
  readonly status: number;
  readonly headers: Record<string, string>;

  constructor(code: ErrorCode, message: string, headers: Record<string, string> = {}) {
    super(message);
    this.code = code;
    this.status = ErrorStatus[code];
    this.headers = headers;
  }
}

export function fail(code: ErrorCode, message: string): never {
  throw new ApiError(code, message);
}
