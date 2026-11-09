// Server-side scrub of support reports and log text (ADR 0032 §7): the client scrubs first, the server scrubs again.
// Patterns: API keys (sk-ant-…, sk_live_…, whsec_…, Bearer …), StageCraft session tokens (scs_…), e-mail addresses, and the user
// name in Windows and POSIX home paths.

const RULES: { pattern: RegExp; replacement: string }[] = [
  { pattern: /sk-ant-[A-Za-z0-9_-]+/g, replacement: '[redacted-key]' },
  // Stripe secret, restricted and publishable keys in live and test mode; publishable keys are not secret but are never useful in a report.
  { pattern: /\b(?:sk|rk|pk)_(?:live|test)_[A-Za-z0-9]+/g, replacement: '[redacted-key]' },
  { pattern: /\bwhsec_[A-Za-z0-9+/=_-]+/g, replacement: '[redacted-key]' },
  { pattern: /\bscs_[A-Za-z0-9_-]+/g, replacement: '[redacted-token]' },
  { pattern: /\bBearer\s+[A-Za-z0-9._~+/=-]+/gi, replacement: 'Bearer [redacted]' },
  { pattern: /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, replacement: '[redacted-email]' },
  // C:\Users\name\..., C:/Users/name/..., \\server\Users\name, also double-escaped backslashes as they appear in JSON logs.
  { pattern: /([A-Za-z]:(?:\\{1,2}|\/)(?:Users|Documents and Settings)(?:\\{1,2}|\/))[^\\/:*?"<>|\r\n]+/gi, replacement: '$1<user>' },
  { pattern: /(\/(?:home|Users)\/)[^/\s:]+/g, replacement: '$1<user>' },
];

export function redact(text: string): string {
  let result = text;
  for (const rule of RULES) {
    result = result.replace(rule.pattern, rule.replacement);
  }
  return result;
}

/** Shows only the first character and the domain: enough to tell accounts apart in an admin view, never a full address in logs. */
export function maskEmail(email: string | null): string | null {
  if (email === null) {
    return null;
  }
  const at = email.lastIndexOf('@');
  if (at <= 0) {
    return '***';
  }
  return `${email[0]}***${email.slice(at)}`;
}
