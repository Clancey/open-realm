# visionOS tabletop gameplay controls

Layer 6 adds native spatial gameplay input while preserving the Quake 2
authority boundary. RealityKit reads immutable copied snapshots and posts typed
commands; Warcraft rules, selection callbacks, target validation, cooldowns,
mana checks, and command-card construction remain server-owned.

## Gesture mapping

| Input | Surface | Result |
|---|---|---|
| Spatial tap | Entity | Replace selection, or submit the entity while authoritative targeting accepts entities |
| Add selection toggle + spatial tap | Entity | Merge the copied selected IDs and submit one replacement selection |
| Long press | Entity | Smart-entity order |
| Spatial tap | Board | Smart-point order, or `point` while authoritative targeting accepts points |
| Drag | Blue left board rail | Translate the tabletop; never posts a game command |
| Two-hand magnify | Board | Bounded tabletop scale; never posts a game command |
| Two-hand rotate | Board | Rotate the tabletop; never posts a game command |
| Command-panel button | Native SwiftUI panel | Submit its copied semantic button token |
| Cancel | Native SwiftUI panel | Submit typed cancel (`button CmdCancel`) |

Single-hand translation, two-hand scale/rotation, selection, smart orders, and
target submission have exclusive enum-owned gesture state. Board manipulation
cannot begin while a gameplay gesture or authoritative target mode owns input.
The transform persists across snapshot generations. Cancellation restores the
gesture baseline, and immersive lifecycle teardown restores the default
placement and clears all ownership.

## Command and snapshot flow

```text
Warcraft server/game
  -> playerState_t target mode + svc_layout command bar
  -> client copied snapshot
  -> bzTTActionLayout_t / stable entity number + generation
  -> copied Swift values
  -> RealityKit hit or native command panel
  -> typed BZ_TT_Post* command
  -> server acknowledgement in a later snapshot
```

`BZ_TABLETOP_ABI_VERSION` 2 adds copied target mode, semantic action visibility,
disabled state, cooldown, target kind, and bounded action tokens. Point
targeting uses the distinct `point` command; ordinary ground orders continue to
use `smartpoint`. Cancel never uses the cinematic `cancel` console command.
Unsupported `onclick` syntax remains visible but disabled and is logged once.

Every RealityKit entity receives a copied entity number, transport generation,
and lifecycle session ID. A hit is rejected unless all three match the currently
presented snapshot and that entity still exists. Swift never reads edicts,
infers commands from model or texture names, or moves a gameplay entity locally.
After this presentation-level stale-hit check, command posting uses the
transport's documented generation-zero current-snapshot mode: the engine
publishes faster than RealityKit presentation, so requiring the copied
presentation generation again would reject valid input between display polls.
The lifecycle session ID remains mandatory, and direct nonzero-generation
transport callers still receive explicit stale-generation rejection.

The native panel is rendered only when the copied command-bar layer is present,
valid, and visible after `client_ui_state` and `uiflags` filtering. Hidden
buttons are omitted; disabled and unsupported buttons remain non-interactive;
positive cooldown values are displayed. Pressing a target-bearing action does
not optimistically enter target mode: the panel and gesture state wait for the
server-authored target enum in a later snapshot.

## Simulator acceptance

Use fresh disposable Apple Vision Pro simulators and never the protected demo
device. Run both archive modes with locally owned data:

```sh
BZ_WC3_DATA_DIR="/Users/clancey/Downloads/Warcraft III" \
  make visionos-tabletop-simulator-acceptance
BZ_WC3_DATA_DIR="/Users/clancey/Downloads/Warcraft III" \
  OPENREALM_TABLETOP_TFT=1 make visionos-tabletop-simulator-acceptance
```

For each Human02 run, verify entity replacement/additive selection, smart entity
and ground orders, every visible command-panel state, entity/point targeting,
cancel acknowledgement, stale-hit rejection after a new generation, left-rail
translation, two-hand rotation and scale bounds, and lifecycle placement reset.
All screenshot or pixel inspection must run in a delegated session and return a
text-only report.
