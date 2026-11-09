// The few browser pages the service serves (sign-in verification, checkout return, dev-only fakes). No scripts; inline style only.

export function escapeHtml(text: string): string {
  return text.replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]!);
}

/** body is trusted markup built by the caller with escapeHtml on every dynamic value. */
export function page(title: string, body: string): string {
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escapeHtml(title)}</title>
<style>body{font-family:system-ui,sans-serif;max-width:32rem;margin:3rem auto;padding:0 1rem;color:#1a1a1a;background:#fff}
input,button{font-size:1.1rem;padding:.5rem .75rem;margin:.25rem 0}code{font-size:1.2rem;letter-spacing:.1em}
@media (prefers-color-scheme:dark){body{background:#151515;color:#eee}}</style></head>
<body><h1>${escapeHtml(title)}</h1>${body}</body></html>`;
}
