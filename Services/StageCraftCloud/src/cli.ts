// Operator commands. `node src/cli.ts migrate` runs migrations (the Kubernetes migration Job);
// `node src/cli.ts grant-admin <accountId>` is the only way to make an admin (roles live in the database, ADR 0032 §5).
import { ConfigError, loadConfig } from './config.ts';
import { openDatabase } from './bootstrap.ts';
import { migrate } from './db/migrate.ts';
import { getAccount, grantRole } from './auth/sessions.ts';

async function run(args: string[]): Promise<number> {
  const config = loadConfig(process.env);
  const db = openDatabase(config);
  try {
    switch (args[0]) {
      case 'migrate': {
        const applied = await migrate(db);
        process.stdout.write(`${JSON.stringify({ applied })}\n`);
        return 0;
      }
      case 'grant-admin': {
        const accountId = args[1] ?? '';
        if (!(await getAccount(db, accountId))) {
          process.stderr.write('no such account (the user must sign in once first)\n');
          return 1;
        }
        await grantRole(db, accountId, 'admin', Date.now());
        process.stdout.write(`${JSON.stringify({ granted: 'admin', accountId })}\n`);
        return 0;
      }
      default:
        process.stderr.write('usage: node src/cli.ts migrate | grant-admin <accountId>\n');
        return 64;
    }
  } finally {
    await db.close();
  }
}

run(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (error: unknown) => {
    process.stderr.write(error instanceof ConfigError ? `${error.message}\n` : `fatal: ${(error as Error).message}\n`);
    process.exit(1);
  },
);
