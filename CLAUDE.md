# Heapwatch Connector (ESP-IDF)

Shared ESP-IDF component: enrollment, MQTT telemetry, logs, metrics, remote commands, OTA, coredumps.

Integration guide: [docs.heapwatch.com/integration](https://docs.heapwatch.com/integration/getting-started/overview)

Workspace context: `heapwatch-firmware/CLAUDE.md` (Lysos `ws`/`rws`, build rules, task naming).

## Changelog (mandatory)

[`CHANGELOG.md`](CHANGELOG.md) — [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) + SemVer.

When you ship any **notable** connector change (new/changed MQTT commands, config, OTA behaviour, breaking API), **add a user-facing bullet under `[Unreleased]` in the same change**. Do not paste commit logs. On **git tag**, promote `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`, then add a fresh empty `[Unreleased]`. Commit to `viktorjamrich/heapwatch-connector-espidf`. Follow the `changelog` skill in the umbrella repo (`.claude/skills/changelog/SKILL.md`).
