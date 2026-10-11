# Roadmap

The milestones are ordered so that something plays on hardware early. Each release candidate is a hardware gate: the owner flashes it and agents never do. Task-level detail lives in [build-plan.md](../engineering/build-plan.md) (v2).

Related: [requirements](requirements.md) · [agentic dev loop](../engineering/agentic-dev.md) · [architecture](../engineering/architecture.md)

| Gate | Done when | Tasks | Requirements |
|---|---|---|---|
| **RC0**: X0X flashed by us | Unmodified X0X (BREAK stripped) builds on deltron.local and boots in fm1-emulator. The owner flashes it, checks that return-to-stock works, and sets the minimum text size on the real panel. | T1 | D1, D2 |
| **RC1**: our engine on the device | The harness and screen goldens are in place, and the one-grammar LIVE shell is working. The clip engine (roles, clip pool, Bar/Beat/End quantize, Loop/Once) and FM6+NOISE are ported. The loader rejects corrupt packages. CPU headroom with 5 roles has been measured on the device. | T2–T5 | R3–R9, R11 |
| **RC2**: first usable release | Songs persist safely (A/B, autosave, undo), there is a STUDIO clip UI (record, step, motion, settings, copy, clear), and songs can be created, opened, renamed, copied and deleted. | T6–T8 | R1, R2, R13, R15 |
| **RC3**: public release candidate | A take is recorded per song (event log, event-exact replay), the six punch FX and LIVE SETUP with key assignment work, MIDI note input, Program Change launch and clock-in work, and the cheat sheet and help cards are in. GitHub CI is on. | T9–T11 | R10, R12, R14 |

Things deferred past v1 are listed in [build-plan.md](../engineering/build-plan.md#deferred-past-v1).

Sound engines come from X0X (909, 808, 303) and Felucca (FM6, NOISE). They change alongside these gates rather than forming a milestone of their own ([ADR-0003](../decisions/0003-sound-design-scope.md)).
