// Entry point: validate configuration, open the database (migrating when MIGRATE_ON_START), serve, and shut down gracefully.
import { ConfigError, describeConfig, loadConfig } from './config.ts';
import { buildContext, createHttpServer } from './bootstrap.ts';
import { releaseMaturedSales } from './payouts.ts';

async function main(): Promise<void> {
  let config;
  try {
    config = loadConfig(process.env);
  } catch (error) {
    if (error instanceof ConfigError) {
      process.stderr.write(`${error.message}\n`);
      process.exit(2);
    }
    throw error;
  }
  const ctx = await buildContext(config);
  const server = createHttpServer(ctx);
  server.listen(config.port, () => ctx.log.info('StageCraft Cloud listening', describeConfig(config)));

  // Hourly: move sales past their hold period from pending to available, so seller balances are current between payout runs.
  const release = setInterval(() => {
    releaseMaturedSales(ctx).catch((error: unknown) => ctx.log.error('release of matured sales failed', { error }));
  }, 3_600_000);
  release.unref();

  const shutdown = (signal: string): void => {
    ctx.log.info('shutting down', { signal });
    clearInterval(release);
    server.close(() => {
      ctx.db.close().finally(() => process.exit(0));
    });
    // Long polls hold connections up to 25 s; the pod's termination grace period is longer than this.
    setTimeout(() => process.exit(1), 30_000).unref();
  };
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
}

main().catch((error: unknown) => {
  process.stderr.write(`fatal: ${(error as Error).message}\n`);
  process.exit(1);
});
