// Generates a StageCraft Pro licence signing key pair (ADR 0034 §17.6). Run once per key, on a trusted machine:
//   node scripts/make-licence-key.ts <key id>
// The private key goes into the secret store as LICENCE_SIGNING_KEY (with LICENCE_KEY_ID = the key id); it is never committed.
// The public key line goes into StageLicenceToken.cpp's AddProductionKeys, so Shipping builds trust it. Keep the previous key in
// that list until every client has a token signed with the new one (at most LICENCE_OFFLINE_DAYS after the switch).
import { generateKeyPairSync } from 'node:crypto';

const keyId = process.argv[2] ?? '';
if (!/^[A-Za-z0-9._-]{1,32}$/.test(keyId) || keyId === 'dev-1') {
  console.error('usage: node scripts/make-licence-key.ts <key id: 1-32 letters, digits, ., _ or -; not dev-1>');
  process.exit(2);
}
const { publicKey, privateKey } = generateKeyPairSync('ed25519');
const raw = Buffer.from(publicKey.export({ format: 'jwk' }).x as string, 'base64url');
const bytes = [...raw].map((byte) => `0x${byte.toString(16).padStart(2, '0')}`).join(', ');
console.log('# Secret store (never commit):');
console.log(`LICENCE_KEY_ID=${keyId}`);
console.log(`LICENCE_SIGNING_KEY=${privateKey.export({ type: 'pkcs8', format: 'der' }).toString('base64')}`);
console.log('');
console.log('// StageLicenceToken.cpp, AddProductionKeys:');
console.log(`Keys.Add({ TEXT("${keyId}"), { ${bytes} } });`);
