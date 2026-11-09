// GET /v1/listings query parameters: the fields of FStageMarketplaceQuery (StageMarketplaceTypes.h) with the rules of
// StageMarketplaceRules::SanitizeQuery / Matches / Apply, translated to SQL that runs the same on SQLite and Postgres.
import { ApiError } from './errors.ts';
import { CATEGORIES, PACKAGE_KINDS, type PackageKind } from './package/stagePackage.ts';
import type { SqlValue } from './db/driver.ts';

export const MaxPageSize = 200;
export const MaxSearchTextLength = 128;
export const DefaultPageSize = 50;

export const SORTS = ['Popular', 'Newest', 'PriceLowToHigh', 'PriceHighToLow', 'Title'] as const;
export const PRICE_FILTERS = ['Any', 'Free', 'Paid'] as const;
export type Sort = (typeof SORTS)[number];
export type PriceFilter = (typeof PRICE_FILTERS)[number];

export interface ListingQuery {
  searchText: string;
  kinds: PackageKind[];
  category: string | null;
  priceFilter: PriceFilter;
  maxCredits: number;
  sort: Sort;
  offset: number;
  limit: number;
}

function integerParam(params: URLSearchParams, name: string, fallback: number): number {
  const raw = params.get(name);
  if (raw === null || raw === '') {
    return fallback;
  }
  if (!/^-?[0-9]{1,15}$/.test(raw)) {
    throw new ApiError('InvalidRequest', `${name} must be an integer`);
  }
  return Number(raw);
}

function enumParam<T extends string>(params: URLSearchParams, name: string, allowed: readonly T[], fallback: T): T {
  const raw = params.get(name);
  if (raw === null || raw === '') {
    return fallback;
  }
  const found = allowed.find((value) => value.toLowerCase() === raw.toLowerCase());
  if (!found) {
    throw new ApiError('InvalidRequest', `${name} must be one of ${allowed.join(', ')}`);
  }
  return found;
}

/** Parses and sanitises like SanitizeQuery: search text cut to 128 then trimmed, offset >= 0, maxCredits >= 0, limit 1..200. */
export function parseListingQuery(params: URLSearchParams): ListingQuery {
  const searchText = (params.get('searchText') ?? '').replace(/[\u0000-\u001f\u007f]/g, ' ').slice(0, MaxSearchTextLength).trim();

  const kinds: PackageKind[] = [];
  for (const raw of params.getAll('kinds').flatMap((value) => value.split(','))) {
    const name = raw.trim();
    if (name === '') continue;
    const kind = PACKAGE_KINDS.find((value) => value.toLowerCase() === name.toLowerCase());
    if (!kind) {
      throw new ApiError('InvalidRequest', `unknown kind ${name.slice(0, 32)}`);
    }
    if (!kinds.includes(kind)) kinds.push(kind);
  }

  const rawCategory = params.get('category');
  let category: string | null = null;
  if (rawCategory !== null && rawCategory !== '') {
    category = CATEGORIES.find((value) => value.toLowerCase() === rawCategory.toLowerCase()) ?? null;
    if (category === null) {
      throw new ApiError('InvalidRequest', 'unknown category');
    }
  }

  return {
    searchText,
    kinds,
    category,
    priceFilter: enumParam(params, 'priceFilter', PRICE_FILTERS, 'Any'),
    maxCredits: Math.max(0, integerParam(params, 'maxCredits', 0)),
    sort: enumParam(params, 'sort', SORTS, 'Popular'),
    offset: Math.max(0, integerParam(params, 'offset', 0)),
    limit: Math.min(MaxPageSize, Math.max(1, integerParam(params, 'limit', DefaultPageSize))),
  };
}

/** LIKE pattern for one word: %, _ and the escape character are literal. */
export function likePattern(word: string): string {
  return `%${word.toLowerCase().replace(/[\\%_]/g, (char) => `\\${char}`)}%`;
}

export interface QuerySql {
  where: string;
  params: SqlValue[];
  orderBy: string;
}

const HAS_REAL = 'CASE WHEN real_money_minor > 0 THEN 1 ELSE 0 END';
const PRICE_AMOUNT = 'CASE WHEN real_money_minor > 0 THEN real_money_minor ELSE credits END';

/** WHERE (approved listings only) and ORDER BY, with the client's tie-break (title case-insensitively, then id). */
export function buildListingQuery(query: ListingQuery): QuerySql {
  const params: SqlValue[] = [];
  const clauses = ["status = 'Approved'"];
  const bind = (value: SqlValue): string => {
    params.push(value);
    return `$${params.length}`;
  };
  if (query.kinds.length > 0) {
    clauses.push(`kind IN (${query.kinds.map(bind).join(', ')})`);
  }
  if (query.category !== null) {
    clauses.push(`category = ${bind(query.category)}`);
  }
  if (query.priceFilter === 'Free') {
    clauses.push('credits = 0 AND real_money_minor = 0');
  } else if (query.priceFilter === 'Paid') {
    clauses.push('(credits > 0 OR real_money_minor > 0)');
  }
  if (query.maxCredits > 0) {
    // Real-money listings never pass a credits limit: the two prices cannot be compared.
    clauses.push(`real_money_minor = 0 AND credits <= ${bind(query.maxCredits)}`);
  }
  for (const word of query.searchText.split(/\s+/).filter(Boolean)) {
    clauses.push(`search_text LIKE ${bind(likePattern(word))} ESCAPE '\\'`);
  }
  const tieBreak = 'title_key ASC, id ASC';
  const orderBy = {
    Popular: `downloads DESC, ${tieBreak}`,
    Newest: `published_at DESC, ${tieBreak}`,
    PriceLowToHigh: `${HAS_REAL} ASC, ${PRICE_AMOUNT} ASC, ${tieBreak}`,
    PriceHighToLow: `${HAS_REAL} DESC, ${PRICE_AMOUNT} DESC, ${tieBreak}`,
    Title: tieBreak,
  }[query.sort];
  return { where: clauses.join(' AND '), params, orderBy };
}

/** search_text: lower-cased title, description, author and keywords joined by U+0001 (never part of a search word). */
export function searchTextFor(fields: { title: string; description: string; authorName: string; keywords: string[] }): string {
  return [fields.title, fields.description, fields.authorName, ...fields.keywords].map((field) => field.toLowerCase()).join('\u0001');
}

/** title_key: each UTF-16 unit of the lower-cased title as 4 hex digits, so any collation sorts it like the client's compare. */
export function titleKeyFor(title: string): string {
  const lower = title.toLowerCase();
  let key = '';
  for (let index = 0; index < lower.length; index++) {
    key += lower.charCodeAt(index).toString(16).padStart(4, '0');
  }
  return key;
}
