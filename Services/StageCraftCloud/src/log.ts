// Structured JSON logs, one object per line. Every string field is passed through the redaction rules, so a token, key, webhook
// secret or e-mail address that reaches a log call by mistake is still scrubbed (Rules §14, ADR 0032 §9).
import { redact } from './redaction.ts';

export type LogLevel = 'debug' | 'info' | 'warn' | 'error';
const ORDER: Record<LogLevel, number> = { debug: 10, info: 20, warn: 30, error: 40 };

export interface Logger {
  debug(message: string, fields?: Record<string, unknown>): void;
  info(message: string, fields?: Record<string, unknown>): void;
  warn(message: string, fields?: Record<string, unknown>): void;
  error(message: string, fields?: Record<string, unknown>): void;
}

const FORBIDDEN_KEYS = /^(authorization|cookie|password|secret|token|sessiontoken|devicecode|code|code_verifier|id_token|email|card)$/i;

function scrub(value: unknown, depth = 0): unknown {
  if (typeof value === 'string') {
    return redact(value);
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  if (value instanceof Error) {
    return { name: value.name, message: redact(value.message) };
  }
  if (Array.isArray(value)) {
    return depth > 4 ? '[…]' : value.map((entry) => scrub(entry, depth + 1));
  }
  if (typeof value === 'object' && value !== null) {
    if (depth > 4) {
      return '[…]';
    }
    const out: Record<string, unknown> = {};
    for (const [key, entry] of Object.entries(value)) {
      out[key] = FORBIDDEN_KEYS.test(key) ? '[redacted]' : scrub(entry, depth + 1);
    }
    return out;
  }
  return value;
}

export function createLogger(level: LogLevel, write: (line: string) => void = (line) => process.stdout.write(`${line}\n`)): Logger {
  const emit = (at: LogLevel, message: string, fields: Record<string, unknown> = {}): void => {
    if (ORDER[at] < ORDER[level]) {
      return;
    }
    const record = { time: new Date().toISOString(), level: at, msg: redact(message), ...(scrub(fields) as Record<string, unknown>) };
    write(JSON.stringify(record));
  };
  return {
    debug: (message, fields) => emit('debug', message, fields),
    info: (message, fields) => emit('info', message, fields),
    warn: (message, fields) => emit('warn', message, fields),
    error: (message, fields) => emit('error', message, fields),
  };
}

/** A logger that keeps lines in memory; tests assert on what would have been logged. */
export function createMemoryLogger(): Logger & { lines: string[] } {
  const lines: string[] = [];
  return Object.assign(createLogger('debug', (line) => lines.push(line)), { lines });
}
