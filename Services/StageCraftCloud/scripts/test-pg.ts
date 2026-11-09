// Runs the whole test suite against a throwaway postgres:16-alpine container (npm run test:pg), then removes the container.
// Every test that uses a database (storage, ledger, orders, webhooks, API, e2e) runs on Postgres through TEST_DATABASE_URL.
import { spawnSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';

const image = process.env.PG_IMAGE ?? 'postgres:16-alpine';
const name = `stagecraft-cloud-test-${randomBytes(4).toString('hex')}`;
const password = randomBytes(12).toString('hex');

function docker(args: string[], allowFailure = false): string {
  const result = spawnSync('docker', args, { encoding: 'utf8' });
  if (result.status !== 0 && !allowFailure) {
    throw new Error(`docker ${args[0]} failed: ${result.stderr || result.stdout}`);
  }
  return (result.stdout ?? '').trim();
}

function sleep(ms: number): void {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

let exitCode = 1;
try {
  docker(['run', '-d', '--rm', '--name', name, '-e', `POSTGRES_PASSWORD=${password}`, '-e', 'POSTGRES_DB=stagecraft', '-p', '127.0.0.1::5432', image]);
  const port = docker(['port', name, '5432/tcp']).split('\n')[0]!.split(':').pop();
  let ready = false;
  for (let attempt = 0; attempt < 60 && !ready; attempt++) {
    // pg_isready can report ready during the image's init restart, so check with a real query.
    ready = spawnSync('docker', ['exec', name, 'psql', '-U', 'postgres', '-d', 'stagecraft', '-c', 'SELECT 1'], { encoding: 'utf8' }).status === 0;
    if (!ready) sleep(1000);
  }
  if (!ready) {
    throw new Error('Postgres did not become ready');
  }
  sleep(1000);
  const url = `postgres://postgres:${password}@127.0.0.1:${port}/stagecraft`;
  process.stdout.write(`Postgres ${image} on port ${port}\n`);
  const tests = spawnSync(process.execPath, ['--test', '--test-concurrency=1', 'test/**/*.test.ts'], {
    stdio: 'inherit',
    env: { ...process.env, TEST_DATABASE_URL: url },
  });
  exitCode = tests.status ?? 1;
} catch (error) {
  process.stderr.write(`${(error as Error).message}\n`);
} finally {
  docker(['rm', '-f', name], true);
}
process.exit(exitCode);
