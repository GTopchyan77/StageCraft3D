// Strict, padded, standard-alphabet base64 check. Linear: a grouped regex such as (?:[A-Za-z0-9+/]{4})* overflows V8's stack on
// multi-megabyte input (a 4 MiB screenshot).
const BODY = /^[A-Za-z0-9+/]*={0,2}$/;

export function isStrictBase64(text: string): boolean {
  return text.length % 4 === 0 && BODY.test(text);
}
