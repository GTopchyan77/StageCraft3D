import test from 'node:test';
import assert from 'node:assert/strict';
import { maskEmail, redact } from '../src/redaction.ts';
import { signStripePayload, verifyStripeSignature } from '../src/payments/webhookSignature.ts';
import { createLogger } from '../src/log.ts';

test('redaction scrubs keys, tokens, bearer headers, e-mails and user names in paths', () => {
  const input = [
    'anthropic sk-ant-api03-AbC_d-ef123',
    'stripe sk_live_51HxYzAbC and rk_test_abc123 and pk_live_xyz',
    'webhook whsec_abcDEF123+/=',
    'session scs_AbCdEf0123456789-_xyz',
    'Authorization: Bearer eyJhbGciOi.payload.sig',
    'mail gevor.someone+tag@example.co.uk',
    'C:\\Users\\Gevor\\AppData\\Local\\StageCraft\\log.txt',
    'C:\\\\Users\\\\Gevor\\\\Saved',
    'C:/Users/Gevor/Documents',
    '/home/gevor/.config and /Users/gevor/Library',
  ].join('\n');
  const output = redact(input);
  for (const secret of ['sk-ant-api03', 'sk_live_51', 'rk_test_abc', 'pk_live_xyz', 'whsec_abc', 'scs_AbC', 'eyJhbGciOi', 'gevor.someone', 'Gevor', '/home/gevor', '/Users/gevor']) {
    assert.ok(!output.includes(secret), `${secret} leaked: ${output}`);
  }
  assert.match(output, /C:\\Users\\<user>\\AppData/);
  assert.match(output, /\[redacted-email\]/);
  assert.equal(redact('nothing to see'), 'nothing to see');
  assert.equal(maskEmail('gevor@example.com'), 'g***@example.com');
  assert.equal(maskEmail('nope'), '***');
  assert.equal(maskEmail(null), null);
});

test('logs never contain tokens, secrets or e-mails, even if passed by mistake', () => {
  const lines: string[] = [];
  const log = createLogger('info', (line) => lines.push(line));
  log.info('user a@b.com signed in with scs_abcdefghijklmnop', {
    authorization: 'Bearer x',
    token: 'scs_abc',
    nested: { email: 'x@y.z', note: 'key sk_live_123', list: ['whsec_1'], amount: 5n, error: new Error('Bearer leaked') },
  });
  log.debug('hidden');
  const text = lines.join('\n');
  assert.equal(lines.length, 1);
  for (const leaked of ['a@b.com', 'scs_abcdefghijklmnop', 'Bearer x', 'x@y.z', 'sk_live_123', 'whsec_1', 'Bearer leaked']) {
    assert.ok(!text.includes(leaked), `${leaked} leaked`);
  }
  assert.equal(JSON.parse(text).nested.amount, '5');
});

const SECRET = 'whsec_test';
const BODY = Buffer.from('{"id":"evt_1"}');
const NOW = 1_700_000_000_000;
const T = Math.floor(NOW / 1000);

test('valid signatures: single v1, several v1, rotated secrets', () => {
  const header = signStripePayload(BODY, SECRET, T);
  assert.deepEqual(verifyStripeSignature(BODY, header, [SECRET], NOW), { ok: true });
  const other = signStripePayload(BODY, 'whsec_other', T).split(',')[1];
  assert.equal(verifyStripeSignature(BODY, `${header},${other}`, [SECRET], NOW).ok, true);
  assert.equal(verifyStripeSignature(BODY, `t=${T},${other},${header.split(',')[1]},v0=abc`, [SECRET], NOW).ok, true);
  assert.equal(verifyStripeSignature(BODY, header, ['whsec_old', SECRET], NOW).ok, true);
  // Within tolerance either way.
  assert.equal(verifyStripeSignature(BODY, header, [SECRET], NOW + 299_000).ok, true);
  assert.equal(verifyStripeSignature(BODY, header, [SECRET], NOW - 299_000).ok, true);
});

test('invalid signatures: missing, malformed, stale, future, tampered body, wrong secret', () => {
  const header = signStripePayload(BODY, SECRET, T);
  assert.deepEqual(verifyStripeSignature(BODY, undefined, [SECRET], NOW), { ok: false, failure: 'missing_header' });
  assert.equal(verifyStripeSignature(BODY, 'garbage', [SECRET], NOW).failure, 'malformed_header');
  assert.equal(verifyStripeSignature(BODY, `t=${T}`, [SECRET], NOW).failure, 'malformed_header');
  assert.equal(verifyStripeSignature(BODY, `t=abc,v1=${'0'.repeat(64)}`, [SECRET], NOW).failure, 'malformed_header');
  assert.equal(verifyStripeSignature(BODY, `t=${T},v1=xyz`, [SECRET], NOW).failure, 'malformed_header');
  assert.equal(verifyStripeSignature(BODY, header, [SECRET], NOW + 301_000).failure, 'stale_timestamp');
  assert.equal(verifyStripeSignature(BODY, header, [SECRET], NOW - 301_000).failure, 'stale_timestamp');
  assert.equal(verifyStripeSignature(Buffer.from('{"id":"evt_2"}'), header, [SECRET], NOW).failure, 'no_match');
  assert.equal(verifyStripeSignature(BODY, header, ['whsec_wrong'], NOW).failure, 'no_match');
  // A signature over a different timestamp does not verify with this one.
  const shifted = `t=${T + 1},${header.split(',')[1]}`;
  assert.equal(verifyStripeSignature(BODY, shifted, [SECRET], NOW).failure, 'no_match');
  assert.equal(signStripePayload('{"id":"evt_1"}', SECRET, T), header);
});
