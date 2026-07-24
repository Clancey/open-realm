import Foundation

@main
enum TabletopPureTests {
    private static var failures = 0

    static func main() async {
        testLauncherReduction()
        testGenerationDeduplication()
        testPlacement()
        testFixtureConversionAndReconciliation()
        testWorldConversion()
        testLifecycleStateMapping()
        testRuntimeModeSelection()
        testDataPreflight()
        testConfigStringCopy()
        testSnapshotDiagnostics()
        testSnapshotValueValidation()
        testCommandLowering()
        testGestureTerminalSuppression()
        await testFixtureCommands()
        await testUnavailableTransport()
        await testPollingCancellation()
        await testFirstSnapshotTimeout()
        testSnapshotLeaseRelease()
        testRenderProviderMode()
        testTerrainChunkingAndTopology()
        testFogAndImageOrientation()
        testMaterialsTeamsScalingAndAnimation()
        testOverlayReduction()
        testDescriptorSceneAndPlaceholder()
        testDescriptorReconciliation()
        testMemoryCache()
        testDiskCache()
        testDescriptorErrorPaths()
        await testRenderPipeline()
        guard failures == 0 else {
            print("TabletopPureTests: \(failures) failure(s)")
            fatalError("TabletopPureTests failed")
        }
        print("TabletopPureTests: all tests passed")
    }

    private static func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        if !condition() {
            failures += 1
            print("FAIL: \(message)")
        }
    }

    private static func testLauncherReduction() {
        var state = TabletopLauncherReducer.reduce(.waiting, .launchRequested)
        expect(state == .opening, "startup begins immersive opening")
        state = TabletopLauncherReducer.reduce(state, .openCancelled)
        expect(state == .failed("Opening the immersive space was cancelled. Choose Retry when you are ready."),
               "cancelled open is actionable")
        state = TabletopLauncherReducer.reduce(state, .retryRequested)
        expect(state == .opening, "retry re-enters opening")
        state = TabletopLauncherReducer.reduce(state, .openSucceeded)
        expect(state == .open, "successful open reaches terminal open state")
        expect(TabletopLauncherReducer.reduce(.opening, .retryRequested) == .opening,
               "retry cannot authorize a second concurrent opening transition")
        expect(TabletopLauncherReducer.reduce(.open, .retryRequested) == .open,
               "retry is ignored outside a failed state")
    }

    private static func testGenerationDeduplication() {
        var deduplicator = TabletopGenerationDeduplicator()
        let first = FixtureSnapshotSource.snapshot(generation: 7)
        expect(deduplicator.accept(first) == first, "first generation is accepted")
        expect(deduplicator.accept(first) == nil, "same generation is suppressed")
        expect(deduplicator.accept(FixtureSnapshotSource.snapshot(generation: 8)) != nil,
               "next generation is accepted")
    }

    private static func testPlacement() {
        let layout = TabletopBoardLayout(columns: 7, rows: 7, cellSize: 0.2)
        expect(TabletopPlacement.worldPosition(TabletopVector3(x: 3, y: 1, z: 3), in: layout)
            == TabletopVector3(x: 0, y: 1, z: 0), "board center maps to world origin")
        expect(TabletopPlacement.clamp(TabletopVector3(x: -2, y: 0, z: 10), to: layout)
            == TabletopVector3(x: 0, y: 0, z: 6), "placement clamps to bounded board")
    }

    private static func testFixtureConversionAndReconciliation() {
        let first = TabletopSnapshotConverter.convert(FixtureSnapshotSource.snapshot(generation: 0))
        expect(first.terrain.count == 49 && first.entities.count == 3, "fixture conversion remains bounded")
        var state = TabletopSceneState()
        let initial = state.reconcile(first)
        expect(initial.upsertedTiles.count == 49 && initial.upsertedEntities.count == 3,
               "initial reconciliation inserts every fixture")
        expect(state.reconcile(first) == TabletopReconciliationPlan(
            removedTileIDs: [], upsertedTiles: [], removedEntityIDs: [], upsertedEntities: []),
            "identical render snapshot reconciles to no work")
        var next = first
        next.entities.removeLast()
        next.entities[0].position.x += 0.1
        let changed = state.reconcile(next)
        expect(changed.removedEntityIDs == [3] && changed.upsertedEntities.map(\.id) == [1],
               "reconciliation removes stale IDs and updates changed values")
    }

    private static func testGestureTerminalSuppression() {
        var gate = TabletopGestureTerminalGate()
        expect(gate.accept(.began) && gate.accept(.changed) && gate.accept(.ended),
               "active gesture accepts one terminal event")
        expect(!gate.accept(.ended) && !gate.accept(.cancelled) && !gate.accept(.changed),
               "late and duplicate terminal traffic is suppressed")
        expect(gate.accept(.began) && gate.accept(.cancelled), "new gesture resets the terminal gate")
    }

    private static func testWorldConversion() {
        let entities = [
            TabletopEntitySnapshot(id: 1, kind: .unit, position: TabletopVector3(x: 10, y: 2, z: 20),
                                   heading: 0, selected: false),
            TabletopEntitySnapshot(id: 2, kind: .unit, position: TabletopVector3(x: 110, y: 2, z: 20),
                                   heading: 0, selected: false),
        ]
        let snapshot = TabletopSnapshot(generation: 1, terrain: [], entities: entities,
                                        coordinateSpace: .world(nil))
        let converted = TabletopSnapshotConverter.convert(snapshot)
        expect(converted.entities[0].position.x == -0.54 && converted.entities[1].position.x == 0.54,
               "live world positions normalize from copied entity bounds")
        var state = TabletopSceneState()
        var duplicate = converted
        duplicate.entities.append(converted.entities[0])
        expect(state.reconcile(duplicate).upsertedEntities.count == 2,
               "duplicate transport IDs reconcile without trapping or inventing IDs")
        var overflow = snapshot
        overflow.entitiesOverflowCount = 17
        overflow.duplicateEntityCount = 3
        expect(overflow.entitiesOverflowCount == 17 && overflow.duplicateEntityCount == 3,
               "snapshot values preserve transport overflow diagnostics")
    }

    private static func testLifecycleStateMapping() {
        expect(TabletopLifecycleState(rawValue: 0) == .idle, "lifecycle maps idle")
        expect(TabletopLifecycleState(rawValue: 2) == .running, "lifecycle maps running")
        expect(TabletopLifecycleState(rawValue: 5) == .stopped, "lifecycle maps stopped")
        expect(TabletopLifecycleState(rawValue: 6) == nil, "unknown lifecycle state is rejected")
    }

    private static func testRuntimeModeSelection() {
        do {
            let liveDefault = try TabletopRuntimeModeResolver.resolve(environment: [:], bundlePath: "/app")
            let fixture = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_MODE": "fixture"], bundlePath: "/app")
            let live = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_MODE": "live", "BZ_TABLETOP_DATA_PATH": "/data",
                              "BZ_TABLETOP_MAP": "map.w3m"], bundlePath: "/app")
            expect(liveDefault == .live(
                dataPath: "/app/Resources/Warcraft III", map: "Human02", connect: nil),
                "production defaults to bundled live data and the standard acceptance map")
            expect(fixture == .fixture, "fixture mode requires explicit selection")
            expect(live == .live(dataPath: "/data", map: "map.w3m", connect: nil),
                "live mode lowers environment settings without fixture fallback")
            expect(TabletopProduct.bundleIdentifier == "org.openrealm.visionos.tabletop" &&
                   TabletopProduct.executable == "OpenRealmTabletop",
                   "framework-free product constants use the production identity")
            do {
                _ = try TabletopRuntimeModeResolver.resolve(
                    environment: ["BZ_TABLETOP_MODE": "invalid"], bundlePath: "/app")
                expect(false, "invalid runtime mode was silently accepted")
            } catch TabletopTransportError.configuration {
                expect(true, "invalid runtime mode is actionable")
            }
            let remote = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_CONNECT": "127.0.0.1"], bundlePath: "/app")
            expect(remote == .live(
                dataPath: "/app/Resources/Warcraft III", map: nil, connect: "127.0.0.1"),
                "explicit remote mode does not also start the default local map")
        } catch {
            expect(false, "runtime mode selection unexpectedly failed: \(error)")
        }
    }

    private static func testConfigStringCopy() {
        let values = TabletopConfigStringCopy.copy(count: 3) { index in
            index == 1 ? "Human02" : nil
        }
        expect(values.count == 3 && values[0] == "" && values[1] == "Human02" && values[2] == "",
               "configstring count copies every slot and preserves valid empty entries")
    }

    private static func testSnapshotDiagnostics() {
        expect(TabletopSnapshotDiagnostics.message(overflow: 0, duplicates: 0) == nil,
               "clean snapshots clear non-fatal diagnostics")
        expect(TabletopSnapshotDiagnostics.message(overflow: 7, duplicates: 2) ==
               "Snapshot capped 7 entities and ignored 2 duplicate IDs.",
               "snapshot loss remains visible without becoming a fatal transport error")
    }

    private static func testDataPreflight() {
        expect(TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "War3.mpq", isRegularFile: true)], localMapRequired: true),
               "archive-backed data passes preflight")
        expect(TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "Scripts\\common.j", isRegularFile: true),
                      TabletopDataEntry(relativePath: "Maps/Test.w3m", isRegularFile: true)],
            localMapRequired: true),
            "loose common script plus map passes local-map preflight")
        expect(TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "Scripts/common.j", isRegularFile: true)],
            localMapRequired: false),
            "loose common script passes remote-connect preflight")
        expect(!TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "random.txt", isRegularFile: true)], localMapRequired: false),
               "arbitrary nonempty directories do not fake live-data readiness")
        expect(!TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "fake.mpq", isRegularFile: false)], localMapRequired: false),
            "directories named like archives do not pass preflight")
        expect(!TabletopDataPreflight.isUsable(
            entries: [TabletopDataEntry(relativePath: "NotScripts/common.j", isRegularFile: true)],
            localMapRequired: false), "misleading script suffixes do not pass preflight")
    }

    private static func testSnapshotValueValidation() {
        do {
            let fog = try TabletopSnapshotValueValidator.fog(
                width: 2, height: 2, visible: [1, 0, 1, 0], explored: [1, 1, 1, 0])
            expect(fog.visible.count == 4 && fog.explored.count == 4, "fog planes preserve bounded values")
            do {
                _ = try TabletopSnapshotValueValidator.fog(width: UInt32.max, height: 2,
                                                           visible: [], explored: [])
                expect(false, "overflowing fog dimensions were accepted")
            } catch TabletopTransportError.malformedSnapshot {
                expect(true, "fog dimension overflow is explicit")
            }
            do {
                try TabletopSnapshotValueValidator.layoutCounts(buttons: 13, inventory: 0, queue: 0)
                expect(false, "oversized unit layout was accepted")
            } catch TabletopTransportError.malformedSnapshot {
                expect(true, "unit layout overflow is explicit")
            }
        } catch {
            expect(false, "valid snapshot value unexpectedly failed: \(error)")
        }
    }

    private static func testCommandLowering() {
        do {
            let select = try TabletopCommandLowering.lower(
                .select(entityIDs: [7], observedGeneration: 0, sessionID: 4), currentSessionID: 4)
            let button = try TabletopCommandLowering.lower(
                .button(code: "hpea", observedGeneration: 8, sessionID: 4), currentSessionID: 4)
            expect(select == .select([7], 0), "selection lowers to the typed ABI operation")
            expect(button == .button([104, 112, 101, 97], 8), "four-byte button code lowers exactly")
            do {
                _ = try TabletopCommandLowering.lower(
                    .button(code: "bad", observedGeneration: 0, sessionID: 4), currentSessionID: 4)
                expect(false, "short button code was accepted")
            } catch TabletopTransportError.invalidCommand {
                expect(true, "invalid command payload is explicit")
            }
            do {
                _ = try TabletopCommandLowering.lower(
                    .cancel(observedGeneration: 0, sessionID: 3), currentSessionID: 4)
                expect(false, "stale session command was accepted")
            } catch TabletopTransportError.staleSession {
                expect(true, "stale session command is explicit")
            }
            expect(TabletopCommandResult.queueFull.error == .commandQueueFull,
                   "bounded C queue result maps to an explicit Swift error")
        } catch {
            expect(false, "valid command lowering unexpectedly failed: \(error)")
        }
    }

    private static func testFixtureCommands() async {
        let fixture = FixtureSnapshotTransport()
        do {
            try await fixture.start()
            let snapshot = try await fixture.poll()
            let sessionID = snapshot?.sessionID ?? 0
            try await fixture.post(.select(entityIDs: [1], observedGeneration: 2, sessionID: sessionID))
            let count = await fixture.postedCommandCount()
            expect(count == 1, "fixture mode preserves typed command flow")
            for _ in 1..<256 { try await fixture.post(.cancel(observedGeneration: 2, sessionID: sessionID)) }
            do {
                try await fixture.post(.cancel(observedGeneration: 2, sessionID: sessionID))
                expect(false, "fixture command queue accepted work beyond its bound")
            } catch TabletopTransportError.commandQueueFull {
                expect(true, "fixture command queue reports its bound")
            } catch {
                expect(false, "fixture queue returned the wrong error: \(error)")
            }
            try await fixture.start()
            do {
                try await fixture.post(.cancel(observedGeneration: 0, sessionID: sessionID))
                expect(false, "fixture accepted a command from the previous session")
            } catch TabletopTransportError.staleSession {
                expect(true, "fixture rejects commands from a previous session")
            } catch {
                expect(false, "stale fixture command returned the wrong error: \(error)")
            }
        } catch {
            expect(false, "fixture command unexpectedly failed: \(error)")
        }
    }

    private static func testUnavailableTransport() async {
        let unavailable = UnavailableTabletopTransport("bad mode")
        do {
            try await unavailable.start()
            expect(false, "invalid runtime mode started silently")
        } catch TabletopTransportError.configuration("bad mode") {
            expect(true, "invalid runtime mode is surfaced")
        } catch {
            expect(false, "invalid runtime mode returned the wrong error: \(error)")
        }
    }

    private static func testPollingCancellation() async {
        let transport = PollingTestTransport(), receiver = PollingReceiver()
        do {
            try await TabletopPolling.run(
                transport: transport, sleep: { throw CancellationError() },
                receive: { _ in await receiver.receive() })
            expect(false, "polling ignored cancellation")
        } catch is CancellationError {
            let pollCount = await transport.pollCount(), received = await receiver.count()
            expect(pollCount == 1, "polling stops after cancellation")
            expect(received == 1, "polling copies one value before cancellation")
        } catch {
            expect(false, "polling cancellation returned the wrong error: \(error)")
        }
    }

    private static func testFirstSnapshotTimeout() async {
        let transport = EmptyPollingTransport()
        do {
            _ = try await TabletopPolling.firstSnapshot(transport: transport, attempts: 2, sleep: {})
            expect(false, "first-snapshot timeout returned fake success")
        } catch TabletopTransportError.startupTimedOut {
            let pollCount = await transport.pollCount()
            expect(pollCount == 2, "first-snapshot timeout remains bounded")
        } catch {
            expect(false, "first-snapshot timeout returned the wrong error: \(error)")
        }
    }

    private static func testSnapshotLeaseRelease() {
        let success = TestSnapshotLease(throwsOnCopy: false)
        do {
            let value = try TabletopSnapshotLeaseConsumer.consume(success)
            expect(value == 42, "snapshot lease returns the copied value")
            expect(success.releaseCount == 1, "snapshot lease releases after success")
        } catch {
            expect(false, "snapshot lease success unexpectedly failed: \(error)")
        }
        let failure = TestSnapshotLease(throwsOnCopy: true)
        do {
            _ = try TabletopSnapshotLeaseConsumer.consume(failure)
            expect(false, "snapshot lease copy failure was swallowed")
        } catch {
            expect(failure.releaseCount == 1, "snapshot lease releases after copy failure")
        }
    }

    private static func testRenderProviderMode() {
        do {
            let fixture = try WarcraftRenderProviderModeResolver.resolve(
                environment: [:], runtimeMode: .fixture)
            let production = try WarcraftRenderProviderModeResolver.resolve(
                environment: [:], runtimeMode: .live(dataPath: "/data", map: nil, connect: nil))
            let explicit = try WarcraftRenderProviderModeResolver.resolve(
                environment: ["BZ_TABLETOP_RENDER_PROVIDER": "fixture"],
                runtimeMode: .live(dataPath: "/data", map: nil, connect: nil))
            expect(fixture == .fixture,
                "fixture transport defaults to fixture descriptors")
            expect(production == .production,
                "live transport defaults to production descriptors")
            expect(explicit == .fixture,
                "descriptor provider can be selected explicitly")
            do {
                _ = try WarcraftRenderProviderModeResolver.resolve(
                    environment: ["BZ_TABLETOP_RENDER_PROVIDER": "procedural"], runtimeMode: .fixture)
                expect(false, "obsolete procedural renderer mode was accepted")
            } catch TabletopTransportError.configuration {
                expect(true, "unknown renderer provider is explicit")
            }
        } catch {
            expect(false, "valid renderer provider mode failed: \(error)")
        }
    }

    private static func testTerrainChunkingAndTopology() {
        do {
            let terrain = FixtureWarcraftRenderProvider.terrain(width: 128, height: 128)
            let chunks = try WarcraftTerrainChunkBuilder.build(terrain)
            expect(chunks.count == 16, "128x128 terrain is bounded to sixteen 32x32 entities")
            expect(chunks.allSatisfy {
                $0.cellBounds.maxX - $0.cellBounds.minX <= 32 &&
                    $0.cellBounds.maxZ - $0.cellBounds.minZ <= 32
            }, "terrain chunk bounds never exceed 32x32")
            expect(chunks.flatMap(\.mesh.geosets).allSatisfy { part in
                part.positions.count == part.normals.count &&
                    part.positions.count == part.textureCoordinates.count &&
                    part.indices.count.isMultiple(of: 3) &&
                    part.indices.allSatisfy { Int($0) < part.positions.count }
            }, "terrain indices and vertex buffers have valid triangle topology")
            expect(chunks.flatMap(\.mesh.geosets).allSatisfy(WarcraftMeshMath.facesMatchNormals),
                   "terrain triangle winding faces the declared surface normals")
            let materials = Set(chunks.flatMap(\.mesh.geosets).map(\.materialIndex))
            expect(materials.contains(terrain.waterMaterialIndex), "water produces dedicated geometry")
            expect(materials.contains(terrain.cliffMaterialIndex), "cliffs produce side-wall geometry")
            expect(materials.contains(terrain.rampMaterialIndex), "ramps produce sloped surface geometry")
            let elevated = chunks.flatMap(\.mesh.geosets).flatMap(\.positions).map(\.y).max() ?? 0
            expect(elevated > 0.07, "terrain mesh preserves fixture height fields")
        } catch {
            expect(false, "valid 128x128 terrain failed: \(error)")
        }
    }

    private static func testFogAndImageOrientation() {
        do {
            let normalized = try WarcraftImageNormalizer.topLeft(
                FixtureWarcraftRenderProvider.asymmetricTexture())
            expect(normalized.orientation == .topLeft, "texture orientation is normalized explicitly")
            expect(Array(normalized.rgba8[0..<8]) == [255, 0, 0, 255, 0, 255, 0, 255],
                   "asymmetric top row remains red then green rather than vertically inverted")
            expect(Array(normalized.rgba8[8..<16]) == [0, 0, 255, 255, 255, 255, 0, 255],
                   "asymmetric bottom row remains blue then yellow")
            let source = WarcraftMeshPartDescriptor(
                name: "atlas", positions: [], normals: [],
                textureCoordinates: [WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 1)],
                indices: [], materialIndex: 0)
            let mapped = try WarcraftAtlasMapper.map(source, region: WarcraftAtlasRegion(
                x: 1, y: 2, width: 2, height: 4, atlasWidth: 8, atlasHeight: 8))
            expect(mapped.textureCoordinates == [
                WarcraftVector2(x: 0.125, y: 0.25), WarcraftVector2(x: 0.375, y: 0.75),
            ], "atlas UV mapping uses normalized top-left pixel bounds")
            let fog = try WarcraftFogBuilder.build(
                WarcraftFogDescriptor(width: 3, height: 1, states: [.hidden, .explored, .visible]),
                terrain: nil)
            expect(fog.image.rgba8 == [
                0, 0, 0, 235, 18, 24, 36, 145, 255, 255, 255, 0,
            ], "three fog states map to deterministic RGBA values")
            expect(fog.width == 0.06 && fog.depth == 0.02, "fog plane uses descriptor bounds")
            expect(WarcraftMeshMath.facesMatchNormals(WarcraftFogBuilder.mesh(fog)),
                   "fog plane winding faces the declared upward normal")
        } catch {
            expect(false, "valid fog/orientation fixture failed: \(error)")
        }
    }

    private static func testMaterialsTeamsScalingAndAnimation() {
        let fixtureModel = FixtureWarcraftRenderProvider.model(.unit)
        expect(fixtureModel.geosets.count == 2 && fixtureModel.materials.count == 2,
               "fixture model exercises real geoset-to-material construction")
        expect(fixtureModel.geosets.allSatisfy {
            $0.textureCoordinates.count == $0.positions.count
        }, "fixture geosets preserve one UV per vertex")
        expect(WarcraftMaterialMapping.kind(fixtureModel.materials[0]) == .litOpaque,
               "opaque fixture texture maps to lit material")
        expect(WarcraftMaterialMapping.kind(fixtureModel.materials[1]) == .litModulate,
               "team-color modulate maps distinctly")
        expect(WarcraftMaterialMapping.kind(
            FixtureWarcraftRenderProvider.model(.resource).materials[1]) == .unlitAdditive,
            "team glow additive material maps distinctly")
        expect(WarcraftTeamPalette.color(0) != WarcraftTeamPalette.color(1) &&
               WarcraftTeamPalette.color(12) == WarcraftTeamPalette.color(0),
               "team colors are deterministic and wrap safely")
        let unit = WarcraftCategoryScale.scale(
            category: .unit, footprint: WarcraftFootprint(width: 1, depth: 1))
        let building = WarcraftCategoryScale.scale(
            category: .building, footprint: WarcraftFootprint(width: 4, depth: 3))
        expect(building.x > unit.x && building.z > unit.z,
               "building footprints scale beyond unit footprints")
        let bounds = WarcraftMeshMath.bounds(fixtureModel)
        expect(bounds?.center.y == 0.49 && bounds?.size.y == 0.98,
               "model bounds preserve the visual center and size used by hit testing")
        let looping = WarcraftAnimationSelector.resolve(
            WarcraftAnimationRequest(sequence: "walk", frame: 85), sequences: fixtureModel.sequences)
        let clamped = WarcraftAnimationSelector.resolve(
            WarcraftAnimationRequest(sequence: "Death", frame: 150), sequences: fixtureModel.sequences)
        let fallback = WarcraftAnimationSelector.resolve(
            WarcraftAnimationRequest(sequence: "missing", frame: 2), sequences: fixtureModel.sequences)
        expect(looping?.sequence == "Walk" && looping?.frame == 45,
               "looping animation wraps into its sequence range")
        expect(clamped?.frame == 99, "non-looping animation clamps to its final frame")
        expect(fallback?.sequence == "Stand", "missing animation selects Stand deterministically")
    }

    private static func testOverlayReduction() {
        expect(WarcraftOverlayReducer.bar(nil) ==
            WarcraftBarState(enabled: false, scale: 0, offset: -0.38),
            "nil overlay values hide their complete rectangular bar")
        expect(WarcraftOverlayReducer.bar(0.25) ==
            WarcraftBarState(enabled: true, scale: 0.25, offset: -0.285),
            "overlay values scale and left-anchor their fill")
        expect(WarcraftOverlayReducer.bar(2) ==
            WarcraftBarState(enabled: true, scale: 1, offset: 0),
            "overlay values clamp above one")
        expect(WarcraftOverlayReducer.bar(-1) ==
            WarcraftBarState(enabled: true, scale: 0, offset: -0.38),
            "overlay values clamp below zero without disappearing")
    }

    private static func testDescriptorSceneAndPlaceholder() {
        do {
            let snapshot = FixtureSnapshotSource.snapshot(generation: 3)
            let fixture = try WarcraftSceneBuilder.build(
                FixtureWarcraftRenderProvider().scene(for: snapshot))
            expect(fixture.terrainChunks.count == 4 && fixture.entities.count == 6,
                   "fixture scene stays bounded while covering all render categories")
            expect(Set(fixture.entities.map(\.descriptor.category)) == Set(WarcraftEntityCategory.allCases),
                   "fixture descriptors cover units, buildings, resources, doodads, and destructables")
            expect(fixture.fog != nil, "fixture scene creates exactly one fog descriptor")
            expect(fixture.entities.allSatisfy { !$0.usedPlaceholder },
                   "fixture test geometry resolves without production placeholders")
            var liveSnapshot = snapshot
            liveSnapshot.coordinateSpace = .world(TabletopBounds2(minX: 0, minZ: 0, maxX: 10, maxZ: 10))
            let production = try WarcraftSceneBuilder.build(
                ProductionWarcraftRenderProvider().scene(for: liveSnapshot))
            expect(production.entities.count == 3 && production.entities.allSatisfy(\.usedPlaceholder),
                   "missing production descriptors use explicit geometry")
            expect(production.entities.allSatisfy {
                $0.model == WarcraftPlaceholder.model &&
                    $0.model.materials == [WarcraftPlaceholder.material]
            }, "missing production assets use the repository placeholder material and geometry")
            expect(production.diagnostics.contains {
                $0.contains("Production asset descriptors await the C exporter adapter")
            }, "parallel-lane production gap is visible")
            var log = WarcraftLogOnce()
            expect(log.record("missing") && !log.record("missing") && log.record("other"),
                   "missing diagnostics log once per unique message")
        } catch {
            expect(false, "descriptor scene construction failed: \(error)")
        }
    }

    private static func testDescriptorReconciliation() {
        do {
            let provider = FixtureWarcraftRenderProvider()
            let first = try WarcraftSceneBuilder.build(
                provider.scene(for: FixtureSnapshotSource.snapshot(generation: 1)))
            var state = WarcraftRenderSceneState()
            let initial = state.reconcile(first)
            expect(initial.upsertedChunks.count == 4 && initial.entityUpdates.count == 6 &&
                   initial.fog != nil, "first descriptor generation inserts all bounded resources")
            expect(state.reconcile(first) == .empty,
                   "same generation causes no chunk, fog, entity, material, or animation work")
            var next = try WarcraftSceneBuilder.build(
                provider.scene(for: FixtureSnapshotSource.snapshot(generation: 2)))
            next.entities.removeLast()
            let update = state.reconcile(next)
            expect(update.removedEntityIDs == [6], "new generation removes absent descriptor entities")
            expect(update.upsertedChunks.isEmpty, "unchanged terrain chunks are not rebuilt")
            expect(update.entityUpdates.contains {
                $0.entity.descriptor.id == 2 && $0.transformChanged && $0.stateChanged
            }, "changed heading and animation update only object state")
            var materialOnly = first
            materialOnly.generation = 3
            materialOnly.entities[0].descriptor.teamTint =
                WarcraftColor(red: 0.5, green: 1, blue: 1, alpha: 1)
            var materialState = WarcraftRenderSceneState()
            _ = materialState.reconcile(first)
            let materialUpdate = materialState.reconcile(materialOnly)
            expect(materialUpdate.entityUpdates.first {
                $0.entity.descriptor.id == 1
            }.map { $0.materialChanged && !$0.stateChanged } == true,
            "material-only updates remain distinct so rebuilt overlays can reapply current state")
            state.reset()
            let restarted = state.reconcile(next)
            expect(restarted.upsertedChunks.count == 4 && restarted.entityUpdates.count == 5,
                   "scene reset accepts a reused generation from a new session")
        } catch {
            expect(false, "descriptor reconciliation failed: \(error)")
        }
    }

    private static func testMemoryCache() {
        do {
            var cache = WarcraftMemoryCache(byteLimit: 6)
            let a = Data([1, 2, 3, 4]), b = Data([5, 6, 7, 8])
            let keyA = try WarcraftCacheKey.content(namespace: "mesh", data: a)
            let keyB = try WarcraftCacheKey.content(namespace: "mesh", data: b)
            let miss = try cache.value(for: keyA, fingerprint: "a")
            expect(miss == nil, "memory cache records a miss")
            try cache.insert(a, for: keyA, fingerprint: "a")
            let hit = try cache.value(for: keyA, fingerprint: "a")
            expect(hit == a, "memory cache records a hit")
            try cache.insert(b, for: keyB, fingerprint: "b")
            let evicted = try cache.value(for: keyA, fingerprint: "a")
            expect(evicted == nil, "memory cache evicts least-recently-used bytes at its bound")
            expect(cache.counters.hits == 1 && cache.counters.misses == 2 &&
                   cache.counters.evictions == 1, "memory cache counters expose hits, misses, and eviction")
            do {
                try cache.insert(b, for: keyB, fingerprint: "collision")
                expect(false, "memory cache accepted a key collision")
            } catch WarcraftDescriptorError.cache {
                expect(true, "memory cache collision is explicit")
            }
        } catch {
            expect(false, "memory cache test failed: \(error)")
        }
    }

    private static func testDiskCache() {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("openrealm-render-cache-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        do {
            let cache = try WarcraftDiskCache(applicationSupport: root, byteLimit: 2_000)
            let payload = Data(repeating: 7, count: 128)
            let key = try WarcraftCacheKey.content(namespace: "texture", data: payload)
            expect(key.fileName.hasPrefix("v1-texture-"), "disk keys are versioned and content hashed")
            let miss = try cache.value(for: key, fingerprint: "fixture")
            expect(miss == nil, "disk cache records a miss")
            try cache.insert(payload, for: key, fingerprint: "fixture")
            let hit = try cache.value(for: key, fingerprint: "fixture")
            expect(hit == payload, "disk cache atomically reloads validated payload")
            let reload = try WarcraftDiskCache(applicationSupport: root, byteLimit: 2_000)
            let reloaded = try reload.value(for: key, fingerprint: "fixture")
            expect(reloaded == payload, "disk cache survives process-style reload")
            do {
                try cache.insert(payload, for: key, fingerprint: "collision")
                expect(false, "disk cache accepted a content-key collision")
            } catch WarcraftDescriptorError.cache {
                expect(true, "disk collision protection rejects mismatched fingerprints")
            }
            do {
                _ = try cache.confinedURL(for: WarcraftCacheKey(namespace: "../escape", digest: "abc"))
                expect(false, "disk cache allowed path traversal")
            } catch WarcraftDescriptorError.cache {
                expect(true, "disk cache confines paths beneath Application Support")
            }
            let small = try WarcraftDiskCache(
                applicationSupport: root.appendingPathComponent("small"), byteLimit: 500)
            for index in 0..<3 {
                let value = Data(repeating: UInt8(index), count: 220)
                try small.insert(value, for: WarcraftCacheKey(
                    namespace: "mesh", digest: String(format: "%032x", index)), fingerprint: "\(index)")
            }
            expect(small.counters.evictions > 0, "disk cache evicts files beyond its byte bound")
        } catch {
            expect(false, "disk cache test failed: \(error)")
        }
    }

    private static func testDescriptorErrorPaths() {
        do {
            _ = try WarcraftImageNormalizer.topLeft(
                WarcraftImageDescriptor(width: 2, height: 2, rgba8: [0], orientation: .topLeft))
            expect(false, "invalid image byte count was accepted")
        } catch WarcraftDescriptorError.invalidImage {
            expect(true, "invalid image byte count is explicit")
        } catch {
            expect(false, "invalid image returned wrong error: \(error)")
        }
        do {
            var terrain = FixtureWarcraftRenderProvider.terrain(width: 4, height: 4)
            terrain.heights.removeLast()
            _ = try WarcraftTerrainChunkBuilder.build(terrain)
            expect(false, "invalid terrain heights were accepted")
        } catch WarcraftDescriptorError.invalidTerrain {
            expect(true, "invalid terrain dimensions are explicit")
        } catch {
            expect(false, "invalid terrain returned wrong error: \(error)")
        }
        do {
            _ = try WarcraftFogBuilder.build(
                WarcraftFogDescriptor(width: 2, height: 2, states: [.visible]), terrain: nil)
            expect(false, "invalid fog state count was accepted")
        } catch WarcraftDescriptorError.invalidFog {
            expect(true, "invalid fog state count is explicit")
        } catch {
            expect(false, "invalid fog returned wrong error: \(error)")
        }
        do {
            _ = try WarcraftOverlayMesh.selectionRing(segments: 2)
            expect(false, "invalid selection topology was accepted")
        } catch WarcraftDescriptorError.invalidMesh {
            expect(true, "invalid selection topology is explicit")
        } catch {
            expect(false, "invalid selection topology returned wrong error: \(error)")
        }
        do {
            _ = try WarcraftAtlasMapper.map(
                WarcraftMeshPartDescriptor(name: "bad-atlas", positions: [], normals: [],
                    textureCoordinates: [], indices: [], materialIndex: 0),
                region: WarcraftAtlasRegion(
                    x: 7, y: 0, width: 2, height: 1, atlasWidth: 8, atlasHeight: 8))
            expect(false, "out-of-bounds atlas region was accepted")
        } catch WarcraftDescriptorError.invalidImage {
            expect(true, "out-of-bounds atlas region is explicit")
        } catch {
            expect(false, "invalid atlas returned wrong error: \(error)")
        }
    }

    private static func testRenderPipeline() async {
        let pipeline = WarcraftRenderPipeline(provider: FixtureWarcraftRenderProvider())
        do {
            let snapshot = FixtureSnapshotSource.snapshot(generation: 9)
            let first = try await pipeline.prepare(snapshot)
            let duplicate = try await pipeline.prepare(snapshot)
            let next = try await pipeline.prepare(FixtureSnapshotSource.snapshot(generation: 10))
            expect(first?.render.generation == 9 && duplicate == nil && next?.render.generation == 10,
                   "provider actor deduplicates generations before descriptor conversion")
            await pipeline.reset()
            let reset = try await pipeline.prepare(snapshot)
            expect(reset != nil,
                   "provider actor reset accepts the first generation of a new session")
        } catch {
            expect(false, "render pipeline actor failed: \(error)")
        }
    }
}

private actor PollingTestTransport: TabletopSnapshotTransport {
    private var polls = 0
    func poll() async throws -> TabletopSnapshot? {
        polls += 1
        return FixtureSnapshotSource.snapshot(generation: UInt64(polls))
    }
    func pollCount() -> Int { polls }
}

private actor PollingReceiver {
    private var received = 0
    func receive() { received += 1 }
    func count() -> Int { received }
}

private actor EmptyPollingTransport: TabletopSnapshotTransport {
    private var polls = 0
    func poll() async throws -> TabletopSnapshot? { polls += 1; return nil }
    func pollCount() -> Int { polls }
}

private enum TestLeaseError: Error { case failed }

private final class TestSnapshotLease: TabletopSnapshotLease {
    let throwsOnCopy: Bool
    private(set) var releaseCount = 0

    init(throwsOnCopy: Bool) { self.throwsOnCopy = throwsOnCopy }
    func copyValue() throws -> Int {
        if throwsOnCopy { throw TestLeaseError.failed }
        return 42
    }
    func release() { releaseCount += 1 }
}
