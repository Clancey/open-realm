# Wings of Liberty retail extraction

The engine consumes locally extracted retail data. The ISO, installer payloads,
keys, and extracted archives are proprietary inputs and must never be committed.

## Proven source

The verified US retail source is the hybrid HFS image:

```text
/Users/clancey/Downloads/Starcraft II/StarCraft II - Wings of Liberty (USA).iso
```

Mount the HFS volume read-only. Do not modify the ISO or extract into the
repository. `Installer Tome 1.MPQE` is encrypted and must be opened with a
StormLib-based extractor using the Wings of Liberty locale key; ordinary MPQ
readers cannot open it directly.

## Repack rules

Installer members are split between loose files and archive payloads:

1. Preserve loose `fileset.base` members at their relative target paths.
2. Select only members below `Repack-MPQ\fileset.base#`.
3. Remove that prefix.
4. Translate `#` separators in the target component to `/`.
5. Repack each resulting container as MPQ format version 2.
6. Exclude IX86, XMAC, and installer filesets. They are not engine data.

Do not silently skip a member that does not fit these rules. Record its source
name and stop so the fileset can be classified.

## Exact extracted tree

The proven output root is:

```text
/Users/clancey/Downloads/Starcraft II/StarCraft2
```

It contains exactly 11 MPQ containers and one loose locale index:

```text
Battle.net/Battle.net.MPQ
Campaigns/Liberty.SC2Campaign/Base.SC2Data
Campaigns/Liberty.SC2Campaign/Base.SC2Maps
Campaigns/Liberty.SC2Campaign/base.SC2Assets
Campaigns/LibertyStory.SC2Campaign/Base.SC2Data
Mods/Challenges.SC2Mod
Mods/Core.SC2Mod/Base.SC2Data
Mods/Core.SC2Mod/Index.SC2Locale
Mods/Core.SC2Mod/base.SC2Assets
Mods/Liberty.SC2Mod/Base.SC2Data
Mods/Liberty.SC2Mod/base.SC2Assets
Mods/LibertyMulti.SC2Mod/Base.SC2Data
```

Case is significant in the staging manifest. Validate without changing the
source:

```sh
BZ_SC2_DATA_DIR="$HOME/Downloads/Starcraft II/StarCraft2" \
  make visionos-verify-sc2-source
```

## Content proof

Build the repository diagnostic first:

```sh
make mpqtool
```

Then prove representative base, model, and campaign data:

```sh
data="$HOME/Downloads/Starcraft II/StarCraft2"
build/bin/mpqtool -mpq "$data/Mods/Core.SC2Mod/Base.SC2Data" \
  cat GameData/Assets.txt
build/bin/mpqtool -mpq "$data/Mods/Liberty.SC2Mod/Base.SC2Data" \
  ls GameData
build/bin/mpqtool -mpq "$data/Campaigns/Liberty.SC2Campaign/Base.SC2Maps" \
  ls Maps/Challenge/ChallengeCommand
```

The verified dataset contains Core `Assets.txt`, Liberty unit/model catalogs
including the Marine M3 identity, and the Liberty campaign TRaynor01
`MapInfo`/`t3Terrain.xml`. Runtime proof:

```sh
SC2DATA="$data" make run-sc2 ARGS="+com_frame_limit 100"
SC2DATA="$data" make test-sc2-live
```

`sc2_data.sh` copies only the exact manifest into a staging root. It rejects
missing, empty, symlinked, or unexpected bundle entries and atomically restores
the previous staged tree if replacement is interrupted.
