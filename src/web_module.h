/*
 * web_module.h - Retro Web (with Active Channel) module interface.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 */

#ifndef WEB_MODULE_H
#define WEB_MODULE_H

#include "suite_shell.h"

/* Public entry points are declared in suite_shell.h.
 * Phases 4-6 populate this header with libwww binding and CDF declarations.
 */

/* Headless render-dump entry: fetch `url` via the SAME libwww +
 * WebDoc parse path the GUI uses, then write a deterministic
 * structured text dump of the resulting block list to `out_path`.
 *
 * Output is UTF-8 with LF line endings, version-tagged at the top
 * so format migrations break the baseline diff loudly rather than
 * silently. Returns 0 on success, non-zero on fetch or write error.
 *
 * Used by test/render_regress.ps1 for the renderer regression
 * harness. No GUI state is touched. */
int web_module_render_dump_to_file(const char *url, const char *out_path);

/* Pass 8 render-tokens coverage entry: fetch `url` via the SAME
 * libwww + WebDoc parse path the GUI and --render-dump use, then run
 * one headless layout pass through web_render_block_into and
 * web_emit_line with the in-file recorder armed. Writes a
 * deterministic text dump of every per-line, per-token, and per-
 * coalesced-underline event to `out_path`.
 *
 * The token dump records what the renderer ACTUALLY computed (final
 * token x/y/w, line metrics, style flags, color, href, byte spans,
 * coalesced underline spans, image final blit rects, HR line y, and
 * the per-block top/bottom margin contributions). It is the coverage
 * complement to --render-dump: that one protects parser/WebDoc
 * structure; this one protects the pixel-path computations. */
int web_module_render_tokens_to_file(const char *url, const char *out_path);

/* Pass 9d copy-verification entry: fetch `url`, run the same headless
 * layout pass --render-tokens uses, set up a "select all" selection,
 * then write to `out_path`:
 *   (a) a per-token diagnostic line with block_idx, is_pre, line_idx
 *       and a bytes preview — so a regression in those fields is
 *       observable WITHOUT actually putting anything on the clipboard;
 *   (b) the reconstructed clipboard plain-text the SAME helper would
 *       produce for Ctrl+C / right-click Copy on a select-all range —
 *       so the clipboard output for PRE blocks and flowing text can
 *       be validated against a known reference (e.g. a Mozilla 1.7.13
 *       copy of the same selection) by direct file comparison.
 *
 * The harness DOES NOT cover clipboard behavior; this entry is the
 * authoritative way to prove the copy path produces the right bytes. */
int web_module_render_copy_all_to_file(const char *url, const char *out_path);

/* Phase 6b additive fetch API. Synchronous on the GUI thread; used by
 * src/activedesktop_module.c at its 60s ticker and 15min CDF cadences.
 * Both helpers return immediately (NULL / 0) if a Retro Web fetch is
 * already in flight, preserving the single-parser-in-flight contract.
 * Active Desktop retries on the next timer tick.
 *
 * web_fetch_and_parse_html_sync drives libwww's HText pipeline so the
 * returned WebDoc is laid out and renderable through web_paint_to with
 * the caller's own WebRenderView. Caller frees with webdoc_free.
 *
 * web_fetch_raw_bytes_sync uses WWW_SOURCE + HTLoadToChunk to obtain
 * the response body verbatim (for application/x-cdf or any non-HTML
 * payload). On success returns 1 and writes a freshly-malloc'd buffer
 * to *out_bytes plus its length to *out_size; caller frees *out_bytes
 * with the standard free(). Returns 0 on failure or busy. */
struct WebDoc;
struct WebDoc *web_fetch_and_parse_html_sync(const char *url);
int            web_fetch_raw_bytes_sync(const char *url,
                                        char **out_bytes,
                                        int *out_size);

/* External-handler dispatch entry point (2026-06-26). Stream-descriptor
 * URLs that have no in-browser renderer (.ram RealAudio metadata) are
 * downloaded to a temp file and handed to the OS file association
 * (RealPlayer), exactly as Firefox does. This wraps the same download +
 * ShellExecuteW logic the in-web .ram intercept uses, with NO Retro Web
 * GUI state touched -- so the F1 Unicorn Desktop channel-click site can
 * launch the player directly without switching to the Retro Web module
 * (no empty page artifact).
 *
 * `url` is UTF-8 (matching the rest of this API and the CDF href model).
 * Returns TRUE if the extension was one of ours and we took the URL over
 * -- the caller MUST then skip its normal navigation, even when the
 * handoff itself failed (otherwise the empty-page fallback returns).
 * Returns FALSE if the extension is not one we handle.
 *
 * out_launched (nullable) receives whether the download + ShellExecuteW
 * actually succeeded, so an in-browser caller can show a differentiated
 * status line. Pass NULL if you do not care. */
BOOL rwb_external_handler_try(const char *url, BOOL *out_launched);

#endif /* WEB_MODULE_H */
