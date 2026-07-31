# StarCraft II visionOS foundation

The first SC2 visionOS layer is deliberately snapshot-only. It proves the real
SC2 game/client/server can run on the generic visionOS lifecycle and immutable
transport without linking SDL, OpenGL, or Warcraft's asset ABI. It does not
present a SwiftUI or RealityKit product yet.

## Architecture

```text
SC2 server/game
  -> normal player/entity/configstring network state
  -> real headless client parser
  -> bz_tabletop_transport ABI v3 immutable snapshots
  -> typed command queue
  -> normal client command packet
  -> SC2 server/game
```

`platform/bridge/bz_tabletop_game.h` is the selected-game policy seam. Warcraft
continues to initialize and publish its asset ABI. SC2 publishes its distinct
terrain/DDS ABI documented in [visionos-assets.md](visionos-assets.md); it does
not fake SC2 terrain through the Warcraft W3E descriptor.

Selection is copied from server-authored `RF_SELECTED`, not the desktop
client's speculative `centity_t.selected` state. Typed commands therefore become
visible only after the server accepts them and a later frame crosses the normal
network path.

## Data

See [installer-extraction.md](installer-extraction.md) for provenance and the
exact 12-entry manifest. The default source is:

```text
$HOME/Downloads/Starcraft II/StarCraft2
```

Override it with `BZ_SC2_DATA_DIR`. Staging owns only
`<root>/Resources/StarCraft II`:

```sh
make visionos-verify-sc2-source
make test-visionos-sc2-data
BZ_XR_APP_STAGE_DIR=build/sc2-stage make visionos-stage-sc2-data
BZ_XR_APP_STAGE_DIR=build/sc2-stage make visionos-verify-sc2-data
```

## Build products

```sh
make visionos-sc2
```

Outputs:

```text
build/lib/visionos/sc2/xrsimulator/libopensc2-engine.a
build/lib/visionos/sc2/xrsimulator/libopensc2-bridge.a
build/lib/visionos/sc2/xros/libopensc2-engine.a
build/lib/visionos/sc2/xros/libopensc2-bridge.a
```

Both arm64 archives are link-checked against their SDK. The smoke binaries must
have no SDL, OpenGL, Homebrew, developer-path, or `BZ_TTA_*` dependency.
libxml2 headers and libraries come from the selected visionOS SDK sysroot.

## Tests

```sh
make test-visionos-sc2-data
make test-bz-tabletop-lifecycle test-bz-tabletop-transport
make test-sc2 test-sc2-tabletop-runtime
make visionos-sc2
SC2DATA="$HOME/Downloads/Starcraft II/StarCraft2" make test-sc2-live test-sc2-tabletop-live
```

`test-sc2-tabletop-runtime` starts the real headless listen server/client on the
generated Tiny fixture, waits for an active snapshot, checks map bounds,
entities, retained terrain, and DDS cache behavior, posts select and point
commands, observes authoritative selection, rejects a stale generation, and
verifies terminal shutdown.
`test-sc2-tabletop-live` verifies the retail-data manifest and repeats the
lifecycle, active snapshot, exact terrain/DDS inventory, cache, retained
lifetime, entity, and shutdown proof on TRaynor01.
Typed command acknowledgement stays fixture-owned because direct-map TRaynor01
starts the local client as player 0 while its authored objects belong to players
3 and 5.

No simulator is launched in this layer. The later native-shell layer must use a
fresh disposable Apple Vision Pro simulator and must never address
`0FDBB103-A3CE-461F-AF41-30CDBEC010FE`.

## Next layer boundary

SC2 M3 geometry and material export remains game-owned Layer 2B. The OpenGL
desktop renderer is not a visionOS asset provider, and renderer-private M3
pointers or buffers must not cross the native asset ABI.
