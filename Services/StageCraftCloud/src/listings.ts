// Listings: publish (server re-validation, ADR 0032 §4), moderation states, query, download entitlement checks, unpublish.
import { createHash, randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { BlobKeys } from './blob/blobStore.ts';
import { big, num, str, strOrNull, UniqueViolation, type Queryable, type Row } from './db/driver.ts';
import type { AppContext } from './context.ts';
import type { Principal } from './http/router.ts';
import { cleanText, PackageError, parseGuid, isValidGuid, readPackage, ANONYMOUS_AUTHOR, MaxAuthorLength } from './package/stagePackage.ts';
import { buildListingQuery, searchTextFor, titleKeyFor, type ListingQuery } from './query.ts';
import { getAccount } from './auth/sessions.ts';

export const LISTING_STATUSES = ['PendingReview', 'Approved', 'Rejected', 'Unpublished'] as const;
export type ListingStatus = (typeof LISTING_STATUSES)[number];

export interface Listing {
  id: string;
  authorId: string;
  authorName: string;
  title: string;
  description: string;
  kind: string;
  category: string;
  keywords: string[];
  credits: bigint;
  realMoneyMinor: bigint;
  currency: string;
  itemCount: number;
  fixtureCount: number;
  cueCount: number;
  packageBytes: number;
  hasThumbnail: boolean;
  allowExport: boolean;
  status: ListingStatus;
  reviewReason: string | null;
  downloads: number;
  createdAt: number;
  publishedAt: number | null;
  packageKey: string;
  thumbnailKey: string | null;
}

export function rowToListing(row: Row): Listing {
  return {
    id: str(row.id),
    authorId: str(row.author_id),
    authorName: str(row.author_name),
    title: str(row.title),
    description: str(row.description),
    kind: str(row.kind),
    category: str(row.category),
    keywords: JSON.parse(str(row.keywords_json)) as string[],
    credits: big(row.credits),
    realMoneyMinor: big(row.real_money_minor),
    currency: str(row.currency),
    itemCount: num(row.item_count),
    fixtureCount: num(row.fixture_count),
    cueCount: num(row.cue_count),
    packageBytes: num(row.package_bytes),
    hasThumbnail: num(row.has_thumbnail) === 1,
    allowExport: num(row.allow_export) === 1,
    status: str(row.status) as ListingStatus,
    reviewReason: strOrNull(row.review_reason),
    downloads: num(row.downloads),
    createdAt: num(row.created_at),
    publishedAt: row.published_at === null ? null : num(row.published_at),
    packageKey: str(row.package_key),
    thumbnailKey: strOrNull(row.thumbnail_key),
  };
}

export function isFree(listing: Listing): boolean {
  return listing.credits === 0n && listing.realMoneyMinor === 0n;
}

/** The FStageMarketplaceListing shape. status/reviewReason only for the author and admins. */
export function listingToJson(listing: Listing, includeReview = false): Record<string, unknown> {
  const json: Record<string, unknown> = {
    listingId: listing.id,
    title: listing.title,
    description: listing.description,
    authorName: listing.authorName,
    kind: listing.kind,
    category: listing.category,
    keywords: listing.keywords,
    price: { credits: listing.credits, realMoneyMinorUnits: listing.realMoneyMinor, realMoneyCurrency: listing.currency },
    downloads: listing.downloads,
    publishedAtUtc: new Date(listing.publishedAt ?? listing.createdAt).toISOString(),
    itemCount: listing.itemCount,
    fixtureCount: listing.fixtureCount,
    cueCount: listing.cueCount,
    packageBytes: listing.packageBytes,
    hasThumbnail: listing.hasThumbnail,
    allowsExport: listing.allowExport,
  };
  if (includeReview) {
    json.status = listing.status;
    json.reviewReason = listing.reviewReason;
  }
  return json;
}

/** A listing id from a path: the hyphenated or 32-digit GUID forms, canonicalised. NotFound for anything else. */
export function parseListingId(text: string): string {
  const id = parseGuid(text);
  if (!isValidGuid(id)) {
    throw new ApiError('NotFound', 'no such listing');
  }
  return id;
}

export async function getListing(q: Queryable, id: string): Promise<Listing | null> {
  const rows = await q.query('SELECT * FROM listings WHERE id = $1', [id]);
  return rows.length > 0 ? rowToListing(rows[0]!) : null;
}

function canSeeUnapproved(listing: Listing, principal: Principal | null): boolean {
  return principal !== null && (principal.accountId === listing.authorId || principal.roles.includes('admin'));
}

/** Approved listings for everyone; the author and admins also see pending, rejected and unpublished ones. */
export async function getVisibleListing(ctx: AppContext, id: string, principal: Principal | null): Promise<Listing> {
  const listing = await getListing(ctx.db, id);
  if (!listing || (listing.status !== 'Approved' && !canSeeUnapproved(listing, principal))) {
    throw new ApiError('NotFound', 'no such listing');
  }
  return listing;
}

export async function hasActiveEntitlement(q: Queryable, accountId: string, listingId: string): Promise<boolean> {
  const rows = await q.query('SELECT 1 AS owned FROM entitlements WHERE account_id = $1 AND listing_id = $2 AND revoked_at IS NULL', [
    accountId,
    listingId,
  ]);
  return rows.length > 0;
}

export async function sellerCanSellForMoney(q: Queryable, accountId: string): Promise<boolean> {
  const rows = await q.query('SELECT payouts_enabled FROM sellers WHERE account_id = $1', [accountId]);
  return rows.length > 0 && num(rows[0]!.payouts_enabled) === 1;
}

function packageErrorToApi(error: PackageError): ApiError {
  return new ApiError(error.result, `package refused: ${error.message}`);
}

/**
 * Publishes a package: re-read with the server's own reader, author stamped from the account, real-money rules, stored, and queued
 * for review (PendingReview, visible only to the author until an admin approves it).
 */
export async function publishListing(ctx: AppContext, principal: Principal, bytes: Buffer): Promise<Listing> {
  let read;
  try {
    read = readPackage(bytes);
  } catch (error) {
    if (error instanceof PackageError) {
      throw packageErrorToApi(error);
    }
    throw error;
  }
  const { manifest, scene, thumbnail } = read;
  if (scene.originListingId !== null) {
    throw new ApiError('NotOriginal', 'a stage acquired from the marketplace cannot be published again');
  }
  if (manifest.credits > 0) {
    throw new ApiError('InvalidRequest', 'credit prices are not sold by StageCraft Cloud yet; publish free or with a real-money price');
  }
  if (manifest.realMoneyMinorUnits > 0) {
    if (!ctx.config.settlementCurrencies.includes(manifest.realMoneyCurrency)) {
      throw new ApiError('CurrencyNotSupported', `prices must be in ${ctx.config.settlementCurrencies.join(', ')}`);
    }
    if (!(await sellerCanSellForMoney(ctx.db, principal.accountId))) {
      throw new ApiError('SellerNotOnboarded', 'finish payout onboarding before publishing with a price');
    }
  }
  if (await getListing(ctx.db, manifest.packageId)) {
    throw new ApiError('AlreadyExists', 'a listing with this package id already exists');
  }

  const account = await getAccount(ctx.db, principal.accountId);
  // The author is the signed-in account, never the manifest's text.
  const cleanedName = cleanText(account?.displayName ?? '', false).slice(0, MaxAuthorLength);
  const authorName = cleanedName.length > 0 ? cleanedName : ANONYMOUS_AUTHOR;

  // A unique suffix: a refused duplicate upload can never overwrite the stored bytes of an existing listing.
  const upload = randomUUID().slice(0, 8);
  const packageKey = BlobKeys.package(`${manifest.packageId}-${upload}`);
  const thumbnailKey = thumbnail.length > 0 ? BlobKeys.thumbnail(`${manifest.packageId}-${upload}`) : null;
  await ctx.blob.put(packageKey, bytes, 'application/octet-stream');
  if (thumbnailKey) {
    await ctx.blob.put(thumbnailKey, thumbnail, 'image/png');
  }

  const now = ctx.clock.now();
  try {
    await ctx.db.query(
      `INSERT INTO listings (id, author_id, author_name, title, description, kind, category, keywords_json, credits, real_money_minor,
         currency, item_count, fixture_count, cue_count, package_bytes, package_sha256, has_thumbnail, allow_export, manifest_version,
         required_items_json, search_text, title_key, status, created_at, package_key, thumbnail_key)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, 'PendingReview', $23, $24, $25)`,
      [
        manifest.packageId,
        principal.accountId,
        authorName,
        manifest.title,
        manifest.description,
        manifest.kind,
        manifest.category,
        JSON.stringify(manifest.keywords),
        BigInt(manifest.credits),
        BigInt(manifest.realMoneyMinorUnits),
        manifest.realMoneyCurrency,
        manifest.itemCount,
        manifest.fixtureCount,
        manifest.cueCount,
        bytes.length,
        createHash('sha256').update(bytes).digest('hex'),
        thumbnail.length > 0 ? 1 : 0,
        manifest.allowExport ? 1 : 0,
        manifest.manifestVersion,
        JSON.stringify(manifest.requiredItems),
        searchTextFor({ title: manifest.title, description: manifest.description, authorName, keywords: manifest.keywords }),
        titleKeyFor(manifest.title),
        now,
        packageKey,
        thumbnailKey,
      ],
    );
  } catch (error) {
    await ctx.blob.delete(packageKey);
    if (thumbnailKey) await ctx.blob.delete(thumbnailKey);
    if (error instanceof UniqueViolation) {
      throw new ApiError('AlreadyExists', 'a listing with this package id already exists');
    }
    throw error;
  }
  ctx.log.info('listing published for review', { listingId: manifest.packageId, author: principal.accountId, bytes: bytes.length });
  return (await getListing(ctx.db, manifest.packageId))!;
}

export async function queryListings(ctx: AppContext, query: ListingQuery): Promise<{ listings: Listing[]; totalMatches: number }> {
  const { where, params, orderBy } = buildListingQuery(query);
  const count = await ctx.db.query(`SELECT COUNT(*) AS n FROM listings WHERE ${where}`, params);
  const limitIndex = params.length + 1;
  const rows = await ctx.db.query(`SELECT * FROM listings WHERE ${where} ORDER BY ${orderBy} LIMIT $${limitIndex} OFFSET $${limitIndex + 1}`, [
    ...params,
    query.limit,
    query.offset,
  ]);
  return { listings: rows.map(rowToListing), totalMatches: num(count[0]!.n) };
}

export async function listOwnListings(ctx: AppContext, accountId: string): Promise<Listing[]> {
  const rows = await ctx.db.query('SELECT * FROM listings WHERE author_id = $1 ORDER BY created_at DESC, id', [accountId]);
  return rows.map(rowToListing);
}

export type DownloadPurpose = 'acquire' | 'preview';

/**
 * The entitlement check of GET /v1/listings/{id}/package: the author and admins always; free approved listings for any signed-in
 * user; paid listings only for an owner (a preview of a paid listing that is not owned is NotOwned). An owner keeps access after the
 * author unpublishes. Returns the short-lived URL.
 */
export async function authorizeDownload(
  ctx: AppContext,
  principal: Principal,
  listingId: string,
  purpose: DownloadPurpose,
): Promise<{ url: string; expiresAt: number }> {
  const listing = await getListing(ctx.db, listingId);
  if (!listing) {
    throw new ApiError('NotFound', 'no such listing');
  }
  const privileged = canSeeUnapproved(listing, principal);
  const owned = await hasActiveEntitlement(ctx.db, principal.accountId, listing.id);
  if (!privileged) {
    const visible = listing.status === 'Approved' || (listing.status === 'Unpublished' && owned);
    if (!visible) {
      throw new ApiError('NotFound', 'no such listing');
    }
    if (!isFree(listing) && !owned) {
      throw new ApiError('NotOwned', 'buy this listing before downloading it');
    }
  }
  if (purpose === 'acquire') {
    const inserted = await ctx.db.query(
      'INSERT INTO listing_downloads (account_id, listing_id, first_at) VALUES ($1, $2, $3) ON CONFLICT (account_id, listing_id) DO NOTHING RETURNING listing_id',
      [principal.accountId, listing.id, ctx.clock.now()],
    );
    if (inserted.length > 0) {
      await ctx.db.query('UPDATE listings SET downloads = downloads + 1 WHERE id = $1', [listing.id]);
    }
  }
  const ttl = ctx.config.downloadUrlTtlSeconds;
  const url = await ctx.blob.downloadUrl(listing.packageKey, ttl, `${listing.id.replace(/-/g, '')}.stage`);
  return { url, expiresAt: ctx.clock.now() + ttl * 1000 };
}

export async function getThumbnail(ctx: AppContext, listingId: string, principal: Principal | null): Promise<Buffer> {
  const listing = await getVisibleListing(ctx, listingId, principal);
  const bytes = listing.thumbnailKey ? await ctx.blob.get(listing.thumbnailKey) : null;
  if (!bytes) {
    throw new ApiError('NotFound', 'this listing has no thumbnail');
  }
  return bytes;
}

/** The author takes a listing off sale. Owners keep their downloads. */
export async function unpublishListing(ctx: AppContext, principal: Principal, listingId: string): Promise<Listing> {
  const listing = await getListing(ctx.db, listingId);
  if (!listing || (listing.status !== 'Approved' && listing.authorId !== principal.accountId)) {
    throw new ApiError('NotFound', 'no such listing');
  }
  if (listing.authorId !== principal.accountId) {
    throw new ApiError('NotYours', 'only the author can unpublish a listing');
  }
  if (listing.status !== 'Unpublished') {
    await ctx.db.query("UPDATE listings SET status = 'Unpublished' WHERE id = $1", [listing.id]);
  }
  return (await getListing(ctx.db, listing.id))!;
}
