# Cheap vision model + fewer checks (live in Base44 since 2026-11-21)

Code changes (all in Base44 code editor, base44/shared/):
- analyzeFrameHandler.ts: classifyImage(base44, signed_url, bytes)   (passes the raw JPEG)
- camService.ts: classifyWithOwnKey(bytes) -> Gemini generateContent with the inline image;
  classifyImage tries it first and falls back to InvokeLLM on ANY problem (no key, HTTP error,
  timeout 8 s, bad JSON, unknown label). checkInterval() adds optional steady-game stretch.

Base44 Secrets (app Dashboard > Secrets). All optional; nothing set = exactly the old behavior:
- VISION_API_KEY            Google AI Studio key. Setting it turns the cheap path on.
- VISION_MODEL              default gemini-3.5-flash-lite
- VISION_PROVIDER           set to "off" to force the built-in AI again (instant rollback)
- STEADY_INTERVAL_SECONDS   e.g. 8: while a game has run 3+ checks with no ad pending, units wait this
                            long (firmware 1.4.0+ honors next_in_seconds, clamped 2-12 s; burst still fires)

Rollbacks: delete VISION_API_KEY or set VISION_PROVIDER=off (server); see ../ROLLBACK.md (firmware).
Estimated cost: ~500 input tokens/check at ~$0.30 per 1M input => roughly $0.0002/check, ~$0.70 per
3-hour game at 3 s; ~$0.25 at 8 s. Estimate only: confirm from Google's usage page after a real game.
