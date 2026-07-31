import Foundation

@main
enum TabletopPureTests {
    private static var failures = 0

    static func main() async {
        testLauncherReduction()
        testProductFlowReduction()
        testProductMapSelection()
        testProductTransitionEffects()
        testAudioLifetime()
        testProgressPersistence()
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
        testEntityClassIdentity()
        testCommandLowering()
        testSpatialControls()
        testSemanticActions()
        testGestureTerminalSuppression()
        await testFixtureCommands()
        await testUnavailableTransport()
        await testPollingCancellation()
        await testFirstSnapshotTimeout()
        await testPreparedSnapshotMailbox()
        await testSnapshotMailbox()
        testSnapshotLeaseRelease()
        testRenderProviderMode()
        testTerrainChunkingAndTopology()
        testFogAndImageOrientation()
        testDescriptorContentKeys()
        testMaterialsTeamsScalingAndAnimation()
        testWarcraftItemPublicationClassList()
        testAssetModelAdapter()
        testAssetTerrainAdapter()
        testAssetAdapterErrorPaths()
        testExportedAssetCache()
        testOverlayReduction()
        testDescriptorSceneAndPlaceholder()
        testDescriptorReconciliation()
        testMemoryCache()
        testDiskCache()
        testDescriptorErrorPaths()
        await testRenderPipeline()
        await testSessionModelLifecycle()
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

    private static func testProductFlowReduction() {
        let map = TabletopMapRecord(
            edition: .roc, source: .campaign, campaignIndex: 1, missionIndex: 2,
            campaign: "Campaign", title: "Mission", subtitle: "", mapPath: "Maps/Test.w3m")
        var state = TabletopProductReducer.reduce(.menu, .launch(map))
        expect(state == .loading(map), "map selection enters explicit loading")
        state = TabletopProductReducer.reduce(state, .loaded)
        expect(state == .playing(map), "loaded map enters playing")
        state = TabletopProductReducer.reduce(state, .pause)
        expect(state == .paused(map), "pause leaves an explicit resumable state")
        expect(TabletopProductReducer.reduce(state, .result(.victory)) == state,
               "terminal results cannot bypass authoritative simulation while paused")
        state = TabletopProductReducer.reduce(state, .resume)
        state = TabletopProductReducer.reduce(state, .result(.victory))
        expect(state == .terminal(map, .victory), "authoritative victory reaches terminal state")
        expect(TabletopProductReducer.reduce(state, .retry) == .loading(map),
               "retry creates a new loading generation")
        expect(TabletopProductReducer.reduce(state, .returnToMenu) == .menu,
               "terminal state can return to product menu")
        expect(TabletopProductReducer.reduce(.menu, .resume) == .menu,
               "invalid inverse transitions are ignored")
    }

    private static func testProductMapSelection() {
        let map = TabletopMapRecord(
            edition: .roc, source: .campaign, campaignIndex: 1, missionIndex: 2,
            campaign: "Campaign", title: "Mission", subtitle: "",
            mapPath: "Maps\\Campaign\\Human02.w3m")
        expect(map.matches("Human02"), "bare acceptance map name matches a Warcraft backslash path")
        expect(map.matches("maps\\campaign\\human02.w3m"), "full map matching is case-insensitive")
        expect(!map.matches("Prologue01"), "a different bare map name does not match")
    }

    private static func testProductTransitionEffects() {
        let map = TabletopMapRecord(
            edition: .roc, source: .campaign, campaignIndex: 1, missionIndex: 2,
            campaign: "Campaign", title: "Mission", subtitle: "", mapPath: "Maps/Test.w3m")
        let playing = TabletopProductState.playing(map)
        let victory = TabletopProductState.terminal(map, .victory)
        expect(TabletopProductReducer.recordsCompletion(from: playing, to: victory),
               "first authoritative victory records completion")
        expect(!TabletopProductReducer.recordsCompletion(from: victory, to: victory),
               "duplicate victory snapshots do not rewrite progress")
        expect(!TabletopProductReducer.recordsCompletion(
            from: playing, to: .terminal(map, .defeat)), "defeat does not record completion")
    }

    private static func testAudioLifetime() {
        expect(TabletopAudioLifetime.shouldRetain(isPlaying: true, intentionallyPaused: false),
               "playing native audio is retained")
        expect(TabletopAudioLifetime.shouldRetain(isPlaying: false, intentionallyPaused: true),
               "intentionally paused native audio survives polling")
        expect(!TabletopAudioLifetime.shouldRetain(isPlaying: false, intentionallyPaused: false),
               "completed native audio is released")
    }

    private static func testProgressPersistence() {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("openrealm-progress-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let store = TabletopProgressStore(applicationSupport: root)
        do {
            var progress = TabletopProgress(selectedMap: "Maps/One.w3m")
            progress.completedMaps.insert("Maps/Zero.w3m")
            try store.save(progress, edition: .roc)
            let loadedROC = try store.load(.roc), loadedTFT = try store.load(.tft)
            expect(loadedROC == progress, "versioned progress round trips atomically")
            expect(loadedTFT == TabletopProgress(), "ROC and TFT progress remain isolated")
            let corrupt = root.appendingPathComponent("OpenRealm/WarcraftTabletop/v1/roc/progress.json")
            try Data("not-json".utf8).write(to: corrupt, options: [.atomic])
            do {
                _ = try store.load(.roc)
                expect(false, "corrupt progress was silently accepted")
            } catch {
                expect(true, "corrupt progress is surfaced")
            }
        } catch {
            expect(false, "progress persistence failed: \(error)")
        }
    }

    private static func testGenerationDeduplication() {
        var deduplicator = TabletopGenerationDeduplicator()
        let first = FixtureSnapshotSource.snapshot(generation: 7)
        expect(deduplicator.accept(first) == first, "first generation is accepted")
        expect(deduplicator.accept(first) == nil, "same generation is suppressed")
        expect(deduplicator.accept(FixtureSnapshotSource.snapshot(generation: 8)) != nil,
               "next generation is accepted")
    }

    private static func testPreparedSnapshotMailbox() async {
        let mailbox = TabletopPreparedSnapshotMailbox()
        let pipeline = WarcraftRenderPipeline(provider: FixtureWarcraftRenderProvider())
        do {
            let first = try await pipeline.prepare(FixtureSnapshotSource.snapshot(generation: 1))!
            let second = try await pipeline.prepare(FixtureSnapshotSource.snapshot(generation: 2))!
            let scheduledFirst = await mailbox.submit(first)
            let scheduledSecond = await mailbox.submit(second)
            let latest = await mailbox.take()
            expect(scheduledFirst, "first prepared snapshot schedules a main-actor handoff")
            expect(!scheduledSecond, "newer prepared snapshots coalesce behind one handoff")
            expect(latest?.snapshot.generation == 2,
                   "prepared snapshot mailbox hands off only the latest generation")
            let scheduledAfterTake = await mailbox.submit(first)
            expect(scheduledAfterTake, "taking a prepared snapshot permits the next handoff")
            await mailbox.reset()
            let resetValue = await mailbox.take()
            expect(resetValue == nil, "prepared snapshot mailbox reset clears pending work")
        } catch {
            expect(false, "prepared snapshot mailbox test failed: \(error)")
        }
    }

    private static func testDescriptorContentKeys() {
        let source = exportedModel()
        do {
            guard let first = try WarcraftAssetDescriptorAdapter.modelTemplate(source).model,
                  let second = try WarcraftAssetDescriptorAdapter.modelTemplate(source).model else {
                expect(false, "model templates unexpectedly produced placeholders")
                return
            }
            expect(first.geometryKey == second.geometryKey && first.materialKey == second.materialKey,
                   "model content keys are deterministic and precomputed once per immutable template")
            var changed = source
            changed.textures[0].image?.rgba8[0] ^= 0xff
            guard let textureChanged = try WarcraftAssetDescriptorAdapter.modelTemplate(changed).model else {
                expect(false, "changed model template unexpectedly produced a placeholder")
                return
            }
            expect(first.geometryKey == textureChanged.geometryKey &&
                   first.materialKey != textureChanged.materialKey,
                   "asymmetric texture changes affect material keys without rebuilding geometry keys")
            var colored = first
            colored.geosets[0].vertexColors = [
                WarcraftColor(red: 1, green: 1, blue: 1, alpha: 0.1),
                WarcraftColor(red: 1, green: 1, blue: 1, alpha: 0.2),
                WarcraftColor(red: 1, green: 1, blue: 1, alpha: 0.3),
            ]
            expect(WarcraftDescriptorContentKey.geometry(first) !=
                WarcraftDescriptorContentKey.geometry(colored),
                "per-vertex water opacity participates in geometry cache keys")
            expect(first.geometryKey?.hasPrefix("v2-") == true &&
                   first.materialKey?.hasPrefix("v2-") == true,
                   "descriptor content keys carry an explicit format version")
            let pixels = [UInt8](repeating: 0x7f, count: 8)
            let row = WarcraftImageDescriptor(width: 2, height: 1, rgba8: pixels, orientation: .topLeft)
            let column = WarcraftImageDescriptor(width: 1, height: 2, rgba8: pixels, orientation: .topLeft)
            expect(WarcraftDescriptorContentKey.image(row) != WarcraftDescriptorContentKey.image(column),
                   "image cache keys include dimensions rather than hashing pixels alone")
        } catch {
            expect(false, "descriptor content-key test failed: \(error)")
        }
    }

    private static func testSnapshotMailbox() async {
        let mailbox = TabletopSnapshotMailbox()
        let first = FixtureSnapshotSource.snapshot(generation: 1)
        let second = FixtureSnapshotSource.snapshot(generation: 2)
        let scheduledFirst = await mailbox.submit(first)
        let scheduledSecond = await mailbox.submit(second)
        let latest = await mailbox.take()
        let finished = await mailbox.finish()
        expect(scheduledFirst && !scheduledSecond,
               "raw snapshot mailbox schedules only one provider conversion worker")
        expect(latest?.generation == 2 && !finished,
               "raw snapshot mailbox coalesces to the latest generation and finishes when empty")
        let restarted = await mailbox.submit(first)
        let retained = await mailbox.submit(second)
        let hasPending = await mailbox.finish()
        let pending = await mailbox.take()
        expect(restarted, "finished raw snapshot mailbox can schedule another worker")
        expect(!retained, "busy raw snapshot mailbox retains newer work")
        expect(hasPending, "raw snapshot mailbox keeps its worker while newer work is pending")
        expect(pending?.generation == 2, "raw snapshot worker receives the pending generation")
        await mailbox.reset()
        let resetValue = await mailbox.take()
        expect(resetValue == nil, "raw snapshot mailbox reset clears pending work")
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
            let tft = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_TFT": "1"], bundlePath: "/app")
            expect(liveDefault == .live(
                dataPath: "/app/Resources/Warcraft III", map: nil, connect: nil, tft: false),
                "production defaults to bundled live data without hardcoding a product map")
            expect(fixture == .fixture, "fixture mode requires explicit selection")
            expect(live == .live(dataPath: "/data", map: "map.w3m", connect: nil, tft: false),
                "live mode lowers environment settings without fixture fallback")
            expect(TabletopCoordinateConversion.heading(.pi / 2) == -.pi / 2,
                   "engine yaw is negated when the Y/Z axis swap changes handedness")
            expect(tft == .live(
                dataPath: "/app/Resources/Warcraft III", map: nil, connect: nil, tft: true),
                "TFT mode is an explicit live-engine argument")
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
            do {
                _ = try TabletopRuntimeModeResolver.resolve(
                    environment: ["BZ_TABLETOP_TFT": "yes"], bundlePath: "/app")
                expect(false, "invalid TFT toggle was silently accepted")
            } catch TabletopTransportError.configuration {
                expect(true, "invalid TFT toggle is actionable")
            }
            let remote = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_CONNECT": "127.0.0.1"], bundlePath: "/app")
            expect(remote == .live(
                dataPath: "/app/Resources/Warcraft III", map: nil, connect: "127.0.0.1", tft: false),
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
        expect(TabletopDataPreflight.supportsProductCatalog(
            entries: [TabletopDataEntry(relativePath: "War3.mpq", isRegularFile: true)]),
            "root-level MPQ archives support native catalog discovery")
        expect(!TabletopDataPreflight.supportsProductCatalog(
            entries: [TabletopDataEntry(relativePath: "nested/War3.mpq", isRegularFile: true)]),
            "nested archives are rejected before catalog discovery")
        expect(!TabletopDataPreflight.supportsProductCatalog(
            entries: [TabletopDataEntry(relativePath: "Maps/Test.w3m", isRegularFile: true)]),
            "loose maps are rejected before catalog discovery")
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

    private static func testEntityClassIdentity() {
        expect(!TabletopEntityMetadata().hasClassIdentity,
               "model-only effects do not request object-data metadata")
        expect(TabletopEntityMetadata(classID: fourCC("hfoo")).hasClassIdentity,
               "gameplay classes request authoritative object-data metadata")
    }

    private static func testCommandLowering() {
        do {
            let select = try TabletopCommandLowering.lower(
                .select(entityIDs: [7], observedGeneration: 0, sessionID: 4), currentSessionID: 4)
            let button = try TabletopCommandLowering.lower(
                .button(code: "CmdMove", observedGeneration: 8, sessionID: 4), currentSessionID: 4)
            let entity = try TabletopCommandLowering.lower(
                .smartEntity(entityID: 11, observedGeneration: 9, sessionID: 4), currentSessionID: 4)
            let point = try TabletopCommandLowering.lower(
                .smartPoint(x: 1, y: -2, observedGeneration: 10, sessionID: 4), currentSessionID: 4)
            let target = try TabletopCommandLowering.lower(
                .targetPoint(x: 3, y: 4, observedGeneration: 11, sessionID: 4), currentSessionID: 4)
            let cancel = try TabletopCommandLowering.lower(
                .cancel(observedGeneration: 12, sessionID: 4), currentSessionID: 4)
            expect(select == .select([7], 0), "selection lowers to the typed ABI operation")
            expect(button == .button([67, 109, 100, 77, 111, 118, 101], 8),
                   "semantic button token lowers exactly")
            expect(entity == .smartEntity(11, 9), "smart entity lowers exactly")
            expect(point == .smartPoint(1, -2, 10), "smart point lowers exactly")
            expect(target == .targetPoint(3, 4, 11), "active point target remains distinct")
            expect(cancel == .cancel(12), "cancel lowers exactly")
            do {
                _ = try TabletopCommandLowering.lower(
                    .button(code: "Cmd Move", observedGeneration: 0, sessionID: 4), currentSessionID: 4)
                expect(false, "unsafe button token was accepted")
            } catch TabletopTransportError.invalidCommand {
                expect(true, "invalid command payload is explicit")
            }
            for command in [
                TabletopCommand.select(entityIDs: [], observedGeneration: 0, sessionID: 4),
                .smartEntity(entityID: TabletopCommandLowering.maxEntityID,
                             observedGeneration: 0, sessionID: 4),
                .smartPoint(x: .nan, y: 0, observedGeneration: 0, sessionID: 4),
                .targetPoint(x: 0, y: .infinity, observedGeneration: 0, sessionID: 4),
                .button(code: "", observedGeneration: 0, sessionID: 4),
            ] {
                do {
                    _ = try TabletopCommandLowering.lower(command, currentSessionID: 4)
                    expect(false, "invalid inverse command was accepted: \(command)")
                } catch TabletopTransportError.invalidCommand {
                    expect(true, "invalid inverse command is explicit")
                } catch {
                    expect(false, "invalid inverse command returned the wrong error: \(error)")
                }
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

    private static func testSpatialControls() {
        var source = FixtureSnapshotSource.snapshot(generation: 7)
        source.sessionID = 3
        source.selectedEntityIDs = [2]
        let snapshot = TabletopSnapshotConverter.convert(source)
        let current = TabletopEntityHit(entityID: 1, generation: 7, sessionID: 3)
        do {
            let id = try TabletopHitValidation.entityID(current, in: snapshot)
            let replacement = try TabletopHitValidation.selection(current, mode: .replacement, in: snapshot)
            let additive = try TabletopHitValidation.selection(current, mode: .additive, in: snapshot)
            expect(id == 1, "current entity hit resolves to its stable id")
            expect(replacement == [1], "replacement selection contains only the hit")
            expect(additive == [2, 1],
                   "additive selection merges the copied authoritative selection")
        } catch {
            expect(false, "current hit unexpectedly failed: \(error)")
        }
        for stale in [
            TabletopEntityHit(entityID: 1, generation: 6, sessionID: 3),
            TabletopEntityHit(entityID: 1, generation: 7, sessionID: 2),
            TabletopEntityHit(entityID: 99, generation: 7, sessionID: 3),
        ] {
            do {
                _ = try TabletopHitValidation.entityID(stale, in: snapshot)
                expect(false, "stale or missing entity hit was accepted")
            } catch TabletopTransportError.staleEntityHit {
                expect(true, "stale entity hit is explicit")
            } catch {
                expect(false, "stale entity hit returned the wrong error: \(error)")
            }
        }

        var interaction = TabletopInteractionState()
        expect(interaction.beginSelection(), "idle state accepts selection ownership")
        expect(!interaction.beginBoardManipulation(.leftHand),
               "selection and board manipulation cannot own one gesture simultaneously")
        interaction.finishTransient()
        expect(interaction.beginSmartPoint(), "idle state accepts a smart-point gesture")
        expect(!interaction.beginSelection(), "smart-point ownership blocks selection")
        interaction.cancelGesture()
        interaction.reconcile(authoritativeTarget: .entityOrPoint)
        expect(interaction.mode == .abilityTarget(.entityOrPoint),
               "authoritative snapshot enters ability target mode")
        expect(interaction.requestCancel() && interaction.mode == .cancelling,
               "cancel enters an explicit pending state")
        interaction.reconcile(authoritativeTarget: .none)
        expect(interaction.mode == .idle, "authoritative acknowledgement ends cancellation")
        expect(!interaction.requestCancel(), "idle cancellation is rejected")
        expect(interaction.beginBoardManipulation(.twoHand), "idle state accepts two-hand board ownership")
        interaction.reconcile(authoritativeTarget: .point)
        expect(interaction.mode == .boardManipulation(.twoHand),
               "active board manipulation is not stolen by snapshot reconciliation")
        interaction.finishTransient()
        interaction.reconcile(authoritativeTarget: .point)
        expect(interaction.mode == .abilityTarget(.point), "target state resumes after board ownership ends")
        interaction.reset()
        expect(interaction.mode == .idle, "lifecycle reset clears interaction ownership")

        var board = TabletopBoardManipulationState()
        expect(board.begin(.leftHand), "left hand acquires board translation")
        expect(!board.begin(.twoHand), "two-hand manipulation cannot steal left-hand ownership")
        expect(board.update(.leftHand, translation: TabletopVector3(x: 9, y: -9, z: 9)),
               "owning hand updates translation")
        expect(board.transform.translation == TabletopVector3(x: 1.5, y: 0.45, z: -0.35),
               "board translation is bounded on every axis")
        expect(board.cancel(.leftHand), "cancel releases board ownership")
        expect(board.transform == TabletopBoardTransform(), "cancel restores the manipulation baseline")
        expect(board.begin(.twoHand), "two hands acquire scale and rotation")
        expect(board.update(.twoHand, yaw: 0.75, magnification: 99), "two hands update scale and rotation")
        expect(board.transform.scale == 1.8 && board.transform.yaw == 0.75,
               "board scale is bounded while rotation is preserved")
        expect(board.end(.twoHand), "matching owner commits manipulation")
        expect(!board.update(.twoHand, magnification: 1.2), "released owner cannot mutate the board")
        board.reset()
        expect(board.transform == TabletopBoardTransform() && board.owner == nil,
               "lifecycle reset restores tabletop placement")

        do {
            let point = try TabletopWorldMapping.enginePoint(
                TabletopVector3(x: 0, y: 0, z: 0),
                bounds: TabletopBounds2(minX: 10, minZ: 20, maxX: 30, maxZ: 60))
            expect(point.0 == 20 && point.1 == 40, "board center maps to authoritative world center")
        } catch {
            expect(false, "valid board point mapping failed: \(error)")
        }
        do {
            _ = try TabletopWorldMapping.enginePoint(TabletopVector3(x: 0, y: 0, z: 0), bounds: nil)
            expect(false, "point mapping silently accepted missing bounds")
        } catch TabletopTransportError.invalidInteractionState {
            expect(true, "missing point mapping state is explicit")
        } catch {
            expect(false, "missing point mapping state returned the wrong error: \(error)")
        }
    }

    private static func testSemanticActions() {
        let move = TabletopActionButtonSnapshot(
            imageIndex: 12, tooltip: "Move", actionCode: "CmdMove", hotkey: 77,
            gridX: 0, gridY: 0, hidden: false, disabled: false, cooldown: 0,
            target: .point, semantic: .button)
        let cancel = TabletopActionButtonSnapshot(
            imageIndex: 0, tooltip: "Cancel", actionCode: "", hotkey: 0,
            gridX: 1, gridY: 0, hidden: false, disabled: false, cooldown: 0,
            target: .none, semantic: .cancel)
        let layout = TabletopActionLayoutSnapshot(
            present: true, visible: true, valid: true, currentTarget: .none, buttons: [move, cancel])
        do {
            let moveCommand = try TabletopActionValidation.command(
                move, layout: layout, generation: 8, sessionID: 2)
            let cancelCommand = try TabletopActionValidation.command(
                cancel, layout: layout, generation: 8, sessionID: 2)
            expect(moveCommand == .button(code: "CmdMove", observedGeneration: 8, sessionID: 2),
                "semantic button emits only its copied authoritative token")
            expect(cancelCommand == .cancel(observedGeneration: 8, sessionID: 2),
                "cancel semantic emits the typed cancel command")
        } catch {
            expect(false, "valid semantic action unexpectedly failed: \(error)")
        }
        for unavailable in [
            TabletopActionButtonSnapshot(
                imageIndex: 0, tooltip: "", actionCode: "Hidden", hotkey: 0,
                gridX: 2, gridY: 0, hidden: true, disabled: false, cooldown: 0,
                target: .none, semantic: .button),
            TabletopActionButtonSnapshot(
                imageIndex: 0, tooltip: "", actionCode: "Disabled", hotkey: 0,
                gridX: 3, gridY: 0, hidden: false, disabled: true, cooldown: 1,
                target: .none, semantic: .button),
        ] {
            var unavailableLayout = layout
            unavailableLayout.buttons.append(unavailable)
            do {
                _ = try TabletopActionValidation.command(
                    unavailable, layout: unavailableLayout, generation: 0, sessionID: 2)
                expect(false, "hidden or disabled action was accepted")
            } catch TabletopTransportError.invalidInteractionState {
                expect(true, "hidden and disabled actions are explicit")
            } catch {
                expect(false, "unavailable action returned the wrong error: \(error)")
            }
        }
        var unsupported = move
        unsupported.semantic = .unsupported
        var unsupportedLayout = layout
        unsupportedLayout.buttons = [unsupported]
        do {
            _ = try TabletopActionValidation.command(
                unsupported, layout: unsupportedLayout, generation: 0, sessionID: 2)
            expect(false, "missing semantic action was accepted")
        } catch TabletopTransportError.missingSemanticAction {
            expect(true, "missing semantic action is explicit")
        } catch {
            expect(false, "missing semantic action returned the wrong error: \(error)")
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
                environment: [:], runtimeMode: .live(dataPath: "/data", map: nil, connect: nil, tft: false))
            let explicit = try WarcraftRenderProviderModeResolver.resolve(
                environment: ["BZ_TABLETOP_RENDER_PROVIDER": "fixture"],
                runtimeMode: .live(dataPath: "/data", map: nil, connect: nil, tft: false))
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
            let reshaped = try WarcraftFogBuilder.build(
                WarcraftFogDescriptor(width: 1, height: 3, states: [.hidden, .explored, .visible]),
                terrain: nil)
            expect(fog.contentKey != reshaped.contentKey,
                   "fog keys include image and plane dimensions even when RGBA bytes match")
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
        var unlitOpaque = fixtureModel.materials[0]
        unlitOpaque.unlit = true
        expect(WarcraftMaterialMapping.kind(unlitOpaque) == .unlitOpaque,
               "unshaded opaque layers retain opaque depth-writing semantics")
        expect(WarcraftMaterialMapping.kind(fixtureModel.materials[1]) == .litModulate,
               "team-color modulate maps distinctly")
        expect(WarcraftMaterialMapping.kind(
            FixtureWarcraftRenderProvider.model(.resource).materials[1]) == .unlitAdditive,
            "team glow additive material maps distinctly")
        expect(WarcraftTeamPalette.color(0) != WarcraftTeamPalette.color(1) &&
               WarcraftTeamPalette.color(12) == WarcraftTeamPalette.color(0),
               "team colors are deterministic and wrap safely")
        let authoredTeam = WarcraftMaterialDescriptor(
            name: "authored-team", color: .white,
            texture: WarcraftImageDescriptor(
                width: 1, height: 1, rgba8: [12, 34, 56, 255], orientation: .topLeft),
            blendMode: .additive, role: .teamGlow, unlit: true)
        let fixtureTeam = WarcraftMaterialDescriptor(
            name: "fixture-team", color: .white, texture: nil,
            blendMode: .additive, role: .teamGlow, unlit: true)
        expect(WarcraftMaterialTint.roleColor(
            authoredTeam, teamColor: WarcraftTeamPalette.color(3)) == .white &&
            WarcraftMaterialTint.roleColor(
                fixtureTeam, teamColor: WarcraftTeamPalette.color(3)) == WarcraftTeamPalette.color(3),
            "authored team images are not second-tinted while textureless fixtures retain palette color")
        var noDepthTeam = authoredTeam
        noDepthTeam.writesDepth = false
        expect(WarcraftDescriptorContentKey.materials([authoredTeam]) !=
               WarcraftDescriptorContentKey.materials([noDepthTeam]),
               "render-significant MDX depth state participates in material reconciliation identity")
        let unit = WarcraftCategoryScale.scale(
            category: .unit, footprint: WarcraftFootprint(width: 1, depth: 1))
        let building = WarcraftCategoryScale.scale(
            category: .building, footprint: WarcraftFootprint(width: 4, depth: 3))
        let item = WarcraftCategoryScale.scale(
            category: .item, footprint: WarcraftFootprint(width: 0, depth: 0))
        expect(building.x > unit.x && building.z > unit.z,
               "building footprints scale beyond unit footprints")
        expect(item.x > 0 && item.z > 0 && item.x < unit.x && item.z < unit.z,
               "zero-footprint items retain their distinct small overlay scale")
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

    private static func testWarcraftItemPublicationClassList() {
        let metadata = [
            WarcraftAssetMetadata(category: .item, classID: 0x66746172, teamColor: 0,
                                  tint: WarcraftColor(red: 1, green: 1, blue: 1, alpha: 1),
                                  footprint: WarcraftFootprint(width: 0, depth: 0)),
            WarcraftAssetMetadata(category: .unit, classID: 0x12345678, teamColor: 0,
                                  tint: WarcraftColor(red: 1, green: 1, blue: 1, alpha: 1),
                                  footprint: WarcraftFootprint(width: 1, depth: 1)),
            WarcraftAssetMetadata(category: .item, classID: 0x34656472, teamColor: 0,
                                  tint: WarcraftColor(red: 1, green: 1, blue: 1, alpha: 1),
                                  footprint: WarcraftFootprint(width: 0, depth: 0)),
            WarcraftAssetMetadata(category: .item, classID: 0x66746172, teamColor: 0,
                                  tint: WarcraftColor(red: 1, green: 1, blue: 1, alpha: 1),
                                  footprint: WarcraftFootprint(width: 0, depth: 0)),
        ]
        expect(WarcraftItemPublication.classList(WarcraftItemPublication.classIDs(metadata)) ==
               "34656472,66746172",
               "item publication classes are filtered, unique, sorted, and byte-exact")
    }

    private static func testAssetModelAdapter() {
        do {
            let source = exportedModel()
            let asset = try WarcraftAssetDescriptorAdapter.model(source)
            guard let model = asset.model else {
                expect(false, "valid MDX 800 fixture became a placeholder")
                return
            }

            expect(asset.identity == "Units/Human/Footman/Footman.mdx" &&
                   asset.bounds == source.bounds, "model identity and exported bounds survive value copying")
            expect(model.geosets.count == 2 && model.materials.count == 2,
                   "each flattened MDX material layer creates explicit RealityKit geometry/material state")
            expect(model.geosets[0].indices == [0, 2, 1] &&
                   WarcraftMeshMath.facesMatchNormals(model.geosets[0]),
                   "MDX z-up conversion reverses winding while preserving declared normals")
            expect(model.geosets[0].textureCoordinates == source.geosets[0].textureCoordinates,
                   "MDX axis conversion and mirrored winding preserve authored UV coordinates exactly")
            expect(model.materials[0].texture?.orientation == .topLeft &&
                   model.materials[0].texture?.rgba8.prefix(4) == [255, 0, 0, 255],
                   "exported top-left RGBA8 texture remains upright through descriptor conversion")
            expect(model.materials[1].role == .teamColor &&
                   model.materials[1].name == source.teamColorImage?.identity &&
                   model.materials[1].texture?.rgba8 == source.teamColorImage?.rgba8 &&
                   WarcraftMaterialMapping.kind(model.materials[1]) == .unlitAdditive,
                   "C-authored team textures preserve additive unshaded MDX layer semantics")
            var glow = source
            glow.textures[1].replaceableID = 2
            glow.teamColorImage = nil
            glow.teamGlowImage = WarcraftExportedImage(
                identity: "TeamGlow01.blp", placeholder: false, status: 0,
                width: 2, height: 1, rowBytes: 8,
                rgba8: [0, 0, 0, 255, 64, 96, 128, 255], orientation: .topLeft)
            let glowMaterial = try WarcraftAssetDescriptorAdapter.model(glow).model?.materials[1]
            expect(glowMaterial?.role == .teamGlow &&
                   glowMaterial?.name == glow.teamGlowImage?.identity &&
                   glowMaterial?.texture?.rgba8 == glow.teamGlowImage?.rgba8 &&
                   glowMaterial?.sourceBlendMode == 3 && glowMaterial?.sourceFlags == 1 &&
                   glowMaterial?.writesDepth == false && glowMaterial?.readsDepth == true &&
                   glowMaterial.map(WarcraftMaterialMapping.kind) == .unlitAdditive,
                   "nonuniform team glow preserves MDX additive, flags, alpha, and depth state")
            let copiedGlow = glowMaterial?.texture?.rgba8
            glow.teamGlowImage?.rgba8[4] = 255
            expect(glowMaterial?.texture?.rgba8 == copiedGlow,
                   "adapted team glow owns copied pixels after the retained C image is released")
            var absentTeam = source
            absentTeam.teamColorImage = nil
            let absentTeamAsset = try WarcraftAssetDescriptorAdapter.model(absentTeam)
            expect(absentTeamAsset.model?.materials[1] == WarcraftPlaceholder.material &&
                   absentTeamAsset.diagnostics.contains { $0.contains("authoritative team image") },
                   "missing team registration becomes an explicit placeholder instead of a solid plane")
            absentTeam.teamColorImage = replacementImage(
                "MissingTeamColor.blp", value: 255, placeholder: true)
            let missingTeamAsset = try WarcraftAssetDescriptorAdapter.model(absentTeam)
            expect(missingTeamAsset.model?.materials[1] == WarcraftPlaceholder.material &&
                   missingTeamAsset.diagnostics.contains { $0.contains("status 6") },
                   "missing C-authored team images preserve status-bearing placeholder accounting")
            expect(model.sequences == [
                WarcraftSequenceDescriptor(name: "Stand", firstFrame: 0, lastFrame: 999, looping: true),
            ], "MDX millisecond sequences remain available to frame selection")
            var noUV = source
            noUV.geosets[0].textureCoordinates = []
            let noUVModel = try WarcraftAssetDescriptorAdapter.model(noUV).model
            expect(noUVModel?.geosets[0].textureCoordinates ==
                Array(repeating: WarcraftVector2(x: 0, y: 0), count: source.geosets[0].positions.count),
                "valid MDX geosets without UVBS receive deterministic zero coordinates for RealityKit")
            var modulate = source
            modulate.layers[0].blendMode = 5
            modulate.textures[0].image = WarcraftExportedImage(
                identity: "Textures/Smoke.blp", placeholder: false, status: 0,
                width: 2, height: 1, rowBytes: 8, rgba8: [
                    255, 254, 255, 255, 64, 65, 63, 255,
                ], orientation: .topLeft)
            let modulateMaterial = try WarcraftAssetDescriptorAdapter.model(modulate).model?.materials[0]
            expect(modulateMaterial?.blendMode == .alpha && modulateMaterial?.unlit == true &&
                   modulateMaterial?.texture?.rgba8 == [0, 0, 0, 0, 0, 0, 0, 191],
                   "grayscale MDX modulate maps exactly to black alpha composition")
            modulate.layers[0].blendMode = 6
            let doubledMaterial = try WarcraftAssetDescriptorAdapter.model(modulate).model?.materials[0]
            expect(doubledMaterial?.texture?.rgba8.suffix(4) == [0, 0, 0, 127],
                   "grayscale MDX modulate-2x doubles luminance before alpha conversion")
            modulate.textures[0].image?.rgba8[0...3] = [255, 0, 0, 255]
            let coloredModulate = try WarcraftAssetDescriptorAdapter.model(modulate)
            expect(coloredModulate.model?.materials[0] == WarcraftPlaceholder.material &&
                   coloredModulate.diagnostics.contains { $0.contains("colored modulate") },
                   "colored destination multiply remains an explicit unsupported material")
            let missing = try WarcraftAssetDescriptorAdapter.model(exportedModel(placeholder: true))
            expect(missing.model == nil && missing.diagnostics.count == 1,
                   "status-bearing C placeholders become repository geometry/material placeholders")
            var metadataError = source
            metadataError.metadataStatus = 6
            let unresolvedClass = try WarcraftAssetDescriptorAdapter.model(metadataError)
            expect(unresolvedClass.model == nil && unresolvedClass.diagnostics.contains {
                $0.contains("metadata status 6")
            }, "metadata resolution failures remain explicit and never invent class or footprint values")
            var missingTexture = source
            missingTexture.textures[0].image?.placeholder = true
            let textureAsset = try WarcraftAssetDescriptorAdapter.model(missingTexture)
            expect(textureAsset.model?.materials[0] == WarcraftPlaceholder.material &&
                   textureAsset.diagnostics.contains { $0.contains("texture 'Textures/Footman.blp'") },
                   "missing visible model textures use the explicit placeholder material")
            var tree = source
            tree.identity = "Doodads/Terrain/LordaeronTree/LordaeronTree0.mdx"
            tree.textures[0] = WarcraftExportedTexture(
                identity: "<replaceable:31>", replaceableID: 31, image: nil)
            tree.overrideImage = replacementImage(
                "ReplaceableTextures/LordaeronTree/LordaeronSummerTree.blp", value: 72)
            let treeAsset = try WarcraftAssetDescriptorAdapter.model(tree)
            expect(treeAsset.model?.materials[0].texture?.rgba8.first == 72 &&
                   treeAsset.model?.materials[0].name == tree.overrideImage?.identity,
                   "non-team replaceables consume the authoritative per-entity image")
            expect(treeAsset.model?.materials[1].role == .teamColor &&
                   treeAsset.model?.materials[1].name == tree.teamColorImage?.identity,
                   "entity skin overrides never replace team-color or team-glow layers")
            var otherTree = tree
            otherTree.overrideImage = replacementImage("OtherTree.blp", value: 39)
            let otherTreeAsset = try WarcraftAssetDescriptorAdapter.model(otherTree)
            expect(treeAsset.model?.materialKey != otherTreeAsset.model?.materialKey,
                   "one shared MDX model with different entity images has distinct material keys")
            var retainedTree = tree
            let retainedAsset = try WarcraftAssetDescriptorAdapter.model(retainedTree)
            retainedTree.overrideImage?.rgba8[0] = 0
            expect(retainedAsset.model?.materials[0].texture?.rgba8.first == 72,
                   "adapted entity images remain copied values after the retained snapshot is released")
            tree.overrideImage = nil
            let absentTree = try WarcraftAssetDescriptorAdapter.model(tree)
            expect(absentTree.model?.materials[0] == WarcraftPlaceholder.material &&
                   absentTree.diagnostics.contains { $0.contains("no authoritative entity image") },
                   "an absent entity image leaves a non-team replaceable explicitly unresolved")
            tree.overrideImage = replacementImage("MissingTree.blp", value: 255, placeholder: true)
            let missingTree = try WarcraftAssetDescriptorAdapter.model(tree)
            expect(missingTree.model?.materials[0] == WarcraftPlaceholder.material &&
                   missingTree.diagnostics.contains { $0.contains("status 6") },
                   "a missing entity image preserves its status-bearing explicit placeholder")
            let placeholderCounts = WarcraftProductionAssets(
                abiVersion: 1, terrain: nil, worldTransform: nil,
                entities: [1: missing, 2: missingTree],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 1),
                terrainTextureCount: 0, terrainNoCliffCount: 0, diagnostics: [])
            expect(placeholderCounts.placeholderModelCount == 1 &&
                   placeholderCounts.placeholderMaterialCount == 1 &&
                   placeholderCounts.placeholderCount == 2,
                   "acceptance counters include missing models and explicit placeholder-role materials")
        } catch {
            expect(false, "valid exported model fixture failed: \(error)")
        }
    }

    private static func testAssetTerrainAdapter() {
        let side = 129, ground = fourCC("Lgrs"), dirt = fourCC("Ldrt")
        let cliff = fourCC("CLdi"), noCliff = fourCC("CLno")
        var corners = Array(repeating: WarcraftExportedTerrainCorner(
            height: 0, waterHeight: 16, groundID: ground, cliffID: cliff,
            groundVariation: 0, cliffVariation: 0,
            cliffLevel: 0, flags: 0), count: side * side)
        corners[0].groundVariation = 2
        corners[0].flags = 1 << 3
        corners[1].flags = 1 << 3
        corners[side].flags = 1 << 3
        corners[side + 1].flags = 1 << 3
        corners[1].waterHeight = 20
        corners[2].flags = 1 << 1
        corners[3].flags = 1 << 1
        corners[side + 2].flags = 1 << 1
        corners[side + 3].flags = 1 << 1
        corners[4].cliffLevel = 1
        corners[6].groundID = dirt
        corners[7].groundID = dirt
        corners[side + 7].groundID = dirt
        for index in [8, 9, side + 8, side + 9] {
            corners[index].cliffID = 0
            corners[index].flags = 1 << 5
        }
        corners[8].cliffLevel = 2
        let terrain = WarcraftExportedTerrain(
            cornerWidth: side, cornerHeight: side, tileWidth: 128, tileHeight: 128,
            chunkTiles: 32, chunkCountX: 4, chunkCountZ: 4,
            bounds: TabletopBounds2(minX: 0, minZ: 0, maxX: 16_384, maxZ: 16_384),
            groundTypes: [ground, dirt], cliffTypes: [cliff, noCliff],
            groundTextures: [
                WarcraftExportedTerrainTexture(
                    typeIndex: 0, typeID: ground, cornerCount: side * side - 3,
                    image: terrainImage(
                        "TerrainArt/LordaeronSummer/Lords_Grass.blp", width: 512, height: 256)),
                WarcraftExportedTerrainTexture(
                    typeIndex: 1, typeID: dirt, cornerCount: 3,
                    image: terrainImage(
                        "TerrainArt/LordaeronSummer/Lords_Dirt.blp", width: 256, height: 256)),
            ],
            cliffTextures: [WarcraftExportedTerrainTexture(
                typeIndex: 0, typeID: cliff, cornerCount: side * side - 4,
                image: terrainImage("ReplaceableTextures/Cliff/Cliff0.blp", width: 256, height: 256))],
            waterTexture: WarcraftExportedTerrainTexture(
                typeIndex: 0, typeID: 0, cornerCount: 4,
                image: terrainImage("authoritative-water", width: 4, height: 4)),
            corners: corners)
        do {
            let assets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: terrain, models: [7: exportedModel()],
                counters: WarcraftAssetCacheCounters(hits: 4, misses: 2, placeholderLogs: 0))
            guard let descriptor = assets.terrain else {
                expect(false, "valid published terrain was omitted")
                return
            }
            let chunks = try WarcraftTerrainChunkBuilder.build(descriptor)
            let chunksAreBounded = chunks.allSatisfy {
                $0.cellBounds.maxX - $0.cellBounds.minX <= 32 &&
                    $0.cellBounds.maxZ - $0.cellBounds.minZ <= 32
            }
            expect(chunks.count == 16 && chunksAreBounded,
                   "authoritative 128x128 terrain remains bounded to sixteen 32x32 entities")
            expect(descriptor.cells[0].waterCornerHeights?.count == 4 &&
                   descriptor.cells[1].features.contains(WarcraftTerrainFeature.ramp) &&
                   descriptor.cells[4].features.contains(WarcraftTerrainFeature.cliff),
                   "published water corners, ramps, and cliff levels survive terrain lowering")
            let waterUV = descriptor.cells[0].waterTextureCoordinates ?? []
            let waterOpacity = descriptor.cells[0].waterCornerOpacities ?? []
            let waterPart = chunks[0].mesh.geosets.first {
                $0.materialIndex == descriptor.waterMaterialIndex
            }
            expect(waterUV == [
                WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1.0 / 3, y: 0),
                WarcraftVector2(x: 1.0 / 3, y: 1.0 / 3), WarcraftVector2(x: 0, y: 1.0 / 3),
            ] && waterOpacity.count == 4 &&
                abs(waterOpacity[0] - 0.32) < 0.00001 &&
                abs(waterOpacity[1] - 0.4) < 0.00001,
                "water uses desktop three-tile UVs and depth-clamped per-corner opacity")
            expect(waterPart?.textureCoordinates.prefix(4).elementsEqual(waterUV) == true &&
                waterPart?.vertexColors.prefix(4).map(\.alpha).elementsEqual(waterOpacity) == true,
                "water mesh preserves authoritative UV and opacity values as vertex colors")
            expect(descriptor.cells[6].surfaceLayers.count == 2 &&
                   descriptor.cells[6].surfaceLayers[1].materialIndex == 1 &&
                   !descriptor.cells[8].features.contains(WarcraftTerrainFeature.cliff),
                   "ground bitmask layers survive while the no-cliff sentinel suppresses cliff geometry")
            let baseUV = descriptor.cells[0].surfaceLayers[0].textureCoordinates
            expect(abs(baseUV[0].x - 0.753125) < 0.00001 && abs(baseUV[2].x - 0.871875) < 0.00001,
                   "base terrain variation selects the extended atlas half with a five-percent inset")
            let transformedHeight = assets.worldTransform?.point(
                TabletopVector3(x: 8_192, y: 128, z: 8_192)).y ?? 0
            expect(abs(transformedHeight - 0.0084375) < 0.000001,
                "terrain bounds provide one shared world-to-tabletop transform")
            expect(assets.counters == WarcraftAssetCacheCounters(
                hits: 4, misses: 2, placeholderLogs: 0),
                "C cache hit/miss/placeholder counters are copied into the production snapshot")
            expect(assets.terrainTextureCount == 4 && assets.terrainNoCliffCount == 4,
                   "terrain image and no-cliff acceptance counters survive pure descriptor lowering")
            expect(descriptor.materials.allSatisfy {
                $0.texture != nil && $0.role != .placeholder
            } && descriptor.materials[descriptor.waterMaterialIndex].role == .water &&
                assets.diagnostics.contains { $0.contains("authoritative asset ABI") },
            "production terrain consumes authoritative ground, cliff, and water images")
            var edgeWater = terrain
            edgeWater.corners[0].flags |= 1 << 0
            let edgeAssets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: edgeWater, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(edgeAssets.terrain?.cells[0].waterLevel == nil,
                   "desktop IsTileWater map-edge suppression prevents edge water geometry")
            var suppressedWater = terrain
            suppressedWater.corners = suppressedWater.corners.map {
                var corner = $0
                if corner.flags & (1 << 3) != 0 { corner.flags |= 1 << 0 }
                return corner
            }
            suppressedWater.waterTexture = nil
            let suppressedAssets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: suppressedWater, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(suppressedAssets.terrainTextureCount == 3 &&
                suppressedAssets.terrain?.cells.allSatisfy { $0.waterLevel == nil } == true,
                "entirely map-edge-suppressed water needs no texture reference or geometry")
            do {
                suppressedWater.waterTexture = terrain.waterTexture
                _ = try WarcraftAssetDescriptorAdapter.production(
                    abiVersion: 1, terrain: suppressedWater, models: [:],
                    counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
                expect(false, "an unexpected edge-only water reference was accepted")
            } catch WarcraftDescriptorError.invalidTerrain {
                expect(true, "edge-only water rejects an exporter reference")
            }
            var missingWater = terrain
            missingWater.waterTexture?.image = replacementImage(
                "missing-water", value: 255, placeholder: true)
            let missingWaterAssets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: missingWater, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 1, placeholderLogs: 1))
            expect(missingWaterAssets.placeholderMaterialCount == 1 &&
                missingWaterAssets.terrain?.materials[
                   missingWaterAssets.terrain!.waterMaterialIndex].role == .placeholder,
                "missing exported water remains an explicit counted placeholder material")
            var dry = terrain
            dry.corners = dry.corners.map {
                var corner = $0
                corner.flags &= ~(1 << 3)
                return corner
            }
            dry.waterTexture = nil
            let dryAssets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: dry, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(dryAssets.terrain?.cells.allSatisfy { $0.waterLevel == nil } == true &&
                dryAssets.terrainTextureCount == 3,
                "a no-water map needs no water texture or water geometry")
            do {
                var missingReference = terrain
                missingReference.waterTexture = nil
                _ = try WarcraftAssetDescriptorAdapter.production(
                   abiVersion: 1, terrain: missingReference, models: [:],
                   counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
                expect(false, "wet terrain without its C-authored water reference was accepted")
            } catch WarcraftDescriptorError.invalidTerrain {
                expect(true, "wet terrain requires its exact C-authored water reference")
            }
            let rock = fourCC("Lrok")
            var sparse = terrain
            sparse.groundTypes = [ground, dirt, rock]
            sparse.groundTextures = [
            terrain.groundTextures[0],
            WarcraftExportedTerrainTexture(
                typeIndex: 2, typeID: rock, cornerCount: 3,
                image: terrainImage(
                    "TerrainArt/LordaeronSummer/Lords_Rock.blp", width: 256, height: 256)),
            ]
            sparse.corners = terrain.corners.map {
            var corner = $0
            if corner.groundID == dirt { corner.groundID = rock }
            return corner
            }
            let sparseAssets = try WarcraftAssetDescriptorAdapter.production(
            abiVersion: 1, terrain: sparse, models: [:],
            counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(sparseAssets.terrain?.cells[6].surfaceLayers.count == 2 &&
               sparseAssets.terrain?.cells[6].surfaceLayers[1].materialIndex == 1,
               "sparse type tables use only C-published referenced textures without inferring index 1")
            var noZero = sparse
            noZero.groundTextures = [sparse.groundTextures[1]]
            noZero.corners = sparse.corners.map {
                var corner = $0
                corner.groundID = rock
                return corner
            }
            let noZeroAssets = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: noZero, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(noZeroAssets.terrain?.materials.first?.blendMode == .opaque &&
               noZeroAssets.terrain?.cells.first?.surfaceLayers.first?.materialIndex == 0,
               "the lowest C-published layer becomes the base when type-table index zero is unused")
            var snapshot = TabletopSnapshot(
                generation: 4, terrain: [], entities: [TabletopEntitySnapshot(
                    id: 7, kind: .unit, position: TabletopVector3(x: 8_192, y: 0, z: 8_192),
                    heading: 0, selected: false,
                    metadata: TabletopEntityMetadata(
                        classID: fourCC("hfoo"), scale: 1, radius: 32, player: 1, model: 1))],
                coordinateSpace: .world(terrain.bounds))
            snapshot.warcraftAssets = assets
            let scene = try WarcraftSceneBuilder.build(
                ProductionWarcraftRenderProvider().scene(for: snapshot))
            expect(scene.entities.count == 1 && !scene.entities[0].usedPlaceholder &&
                   scene.entities[0].position == WarcraftVector3(x: 0, y: 0, z: 0) &&
                   scene.entities[0].descriptor.teamColor == 1,
                   "production provider consumes copied model/team/transform values without a fixture path")
        } catch {
            expect(false, "valid exported terrain fixture failed: \(error)")
        }
    }

    private static func testAssetAdapterErrorPaths() {
        do {
            var malformed = exportedModel()
            malformed.geosets[0].indices = [0, 1, 9]
            _ = try WarcraftAssetDescriptorAdapter.model(malformed)
            expect(false, "out-of-range exported MDX index was accepted")
        } catch WarcraftDescriptorError.invalidMesh {
            expect(true, "malformed exported geoset fails explicitly")
        } catch {
            expect(false, "malformed exported geoset returned wrong error: \(error)")
        }
        do {
            var malformed = exportedModel()
            malformed.geosets[0].textureCoordinates.removeLast()
            _ = try WarcraftAssetDescriptorAdapter.model(malformed)
            expect(false, "partial exported MDX UV buffer was accepted")
        } catch WarcraftDescriptorError.invalidMesh {
            expect(true, "nonzero MDX UV count must still match the vertex count")
        } catch {
            expect(false, "partial exported UV buffer returned wrong error: \(error)")
        }
        do {
            var malformed = exportedModel()
            malformed.layers[0].blendMode = 99
            _ = try WarcraftAssetDescriptorAdapter.model(malformed)
            expect(false, "unknown exported blend mode was accepted")
        } catch WarcraftDescriptorError.invalidMesh {
            expect(true, "unknown exported blend mode fails explicitly")
        } catch {
            expect(false, "unknown blend mode returned wrong error: \(error)")
        }
        do {
            let terrain = WarcraftExportedTerrain(
                cornerWidth: 2, cornerHeight: 2, tileWidth: 1, tileHeight: 1,
                chunkTiles: 16, chunkCountX: 1, chunkCountZ: 1,
                bounds: TabletopBounds2(minX: 0, minZ: 0, maxX: 128, maxZ: 128),
                groundTypes: [fourCC("Lgrs")], cliffTypes: [],
                groundTextures: [], cliffTextures: [], corners: [])
            _ = try WarcraftAssetDescriptorAdapter.production(
                abiVersion: 1, terrain: terrain, models: [:],
                counters: WarcraftAssetCacheCounters(hits: 0, misses: 0, placeholderLogs: 0))
            expect(false, "non-authoritative terrain chunk size was accepted")
        } catch WarcraftDescriptorError.invalidTerrain {
            expect(true, "terrain ABI/chunk mismatch fails explicitly")
        } catch {
            expect(false, "terrain ABI/chunk mismatch returned wrong error: \(error)")
        }
    }

    private static func testExportedAssetCache() {
        var cache = WarcraftExportedAssetCache(modelLimit: 2)
        let first = exportedModel()
        var second = exportedModel(), third = exportedModel()
        second.identity = "second.mdx"
        third.identity = "third.mdx"
        let firstKey = WarcraftExportedModelCacheKey(
            configStringIndex: 1, identity: first.identity, status: 0, placeholder: false)
        let secondKey = WarcraftExportedModelCacheKey(
            configStringIndex: 2, identity: second.identity, status: 0, placeholder: false)
        let thirdKey = WarcraftExportedModelCacheKey(
            configStringIndex: 3, identity: third.identity, status: 0, placeholder: false)
        expect(cache.model(for: firstKey) == nil, "copied asset cache records model misses")
        cache.insert(first, for: firstKey)
        expect(cache.model(for: firstKey) == first, "copied asset cache reuses immutable model buffers")
        do {
            let adapted = try WarcraftAssetDescriptorAdapter.modelTemplate(first)
            cache.insertAdapted(adapted, for: firstKey)
            expect(cache.adaptedModel(for: firstKey) == adapted,
                   "copied asset cache reuses the immutable adapted model template")
        } catch {
            expect(false, "adapted model cache setup failed: \(error)")
        }
        cache.insert(second, for: secondKey)
        cache.insert(third, for: thirdKey)
        expect(cache.model(for: firstKey) == nil && cache.adaptedModel(for: firstKey) == nil &&
               cache.counters.evictions == 1,
               "copied asset cache evicts raw and adapted model values together at its fixed bound")
        do {
            let firstSkin = replacementImage("TreeA.blp", value: 24)
            let secondSkin = replacementImage("TreeB.blp", value: 48)
            let firstSkinKey = WarcraftExportedModelCacheKey(
               configStringIndex: 4, identity: first.identity, status: 0, placeholder: false,
               overrideIdentity: firstSkin.identity,
               overrideContentKey: try WarcraftAssetDescriptorAdapter.imageContentKey(firstSkin))
            let secondSkinKey = WarcraftExportedModelCacheKey(
               configStringIndex: 4, identity: first.identity, status: 0, placeholder: false,
               overrideIdentity: secondSkin.identity,
               overrideContentKey: try WarcraftAssetDescriptorAdapter.imageContentKey(secondSkin))
            var firstSkinned = first, secondSkinned = first
            firstSkinned.overrideImage = firstSkin; secondSkinned.overrideImage = secondSkin
            cache.reset()
            expect(cache.model(for: firstSkinKey) == nil && cache.model(for: secondSkinKey) == nil,
                  "different entity skins begin as independent model-cache misses")
            cache.insert(firstSkinned, for: firstSkinKey)
            cache.insert(secondSkinned, for: secondSkinKey)
            expect(cache.model(for: firstSkinKey) == firstSkinned &&
                  cache.model(for: secondSkinKey) == secondSkinned &&
                  firstSkinKey != secondSkinKey && cache.counters.hits == 2,
                  "shared model geometry reuses only the exact identity-and-content skin key")
            let firstTeam = replacementImage("TeamColor01.blp", value: 64)
            let secondTeam = replacementImage("TeamColor01.blp", value: 64)
            var firstTeamKey = firstKey, secondTeamKey = firstKey
            firstTeamKey.teamColorIndex = 1
            firstTeamKey.teamColorContentKey = try WarcraftAssetDescriptorAdapter.imageContentKey(firstTeam)
            secondTeamKey.teamColorIndex = 2
            secondTeamKey.teamColorContentKey = try WarcraftAssetDescriptorAdapter.imageContentKey(secondTeam)
            var firstTeamed = first, secondTeamed = first
            firstTeamed.teamColorImage = firstTeam
            secondTeamed.teamColorImage = secondTeam
            cache.insert(firstTeamed, for: firstTeamKey)
            cache.insert(secondTeamed, for: secondTeamKey)
            expect(firstTeamKey != secondTeamKey &&
                  cache.model(for: firstTeamKey)?.teamColorImage == firstTeam &&
                  cache.model(for: secondTeamKey)?.teamColorImage == secondTeam,
                  "same model and image bytes remain separated by authoritative team index")
            expect(WarcraftTeamTextureRequest.resolve(
                kind: 1, teamColor: 15, count: 16, required: true) ==
                   .valid(UInt64(1) << 32 | 15) &&
                WarcraftTeamTextureRequest.resolve(
                   kind: 2, teamColor: 16, count: 16, required: true) == .invalid &&
                WarcraftTeamTextureRequest.resolve(
                   kind: 3, teamColor: 0, count: 16, required: true) == .invalid &&
                WarcraftTeamTextureRequest.resolve(
                   kind: 2, teamColor: 0, count: 0, required: true) == .invalid &&
                WarcraftTeamTextureRequest.resolve(
                   kind: 99, teamColor: 99, count: 0, required: false) == .absent,
                "team texture reducer covers both kinds, count bounds, invalid inputs, and absent roles")
        } catch {
            expect(false, "entity skin cache-key setup failed: \(error)")
        }
        let terrain = WarcraftExportedTerrain(
            cornerWidth: 2, cornerHeight: 2, tileWidth: 1, tileHeight: 1,
            chunkTiles: 32, chunkCountX: 1, chunkCountZ: 1,
            bounds: TabletopBounds2(minX: 0, minZ: 0, maxX: 128, maxZ: 128),
            groundTypes: [], cliffTypes: [], groundTextures: [], cliffTextures: [], corners: [])
        expect(cache.terrain(for: "map:a") == nil, "copied asset cache records terrain misses")
        cache.insert(terrain, terrainKey: "map:a")
        expect(cache.terrain(for: "map:a") == terrain && cache.terrain(for: "map:b") == nil,
               "copied terrain reloads only for the exact map/source key")
        cache.reset()
        expect(cache.model(for: thirdKey) == nil && cache.terrain(for: "map:a") == nil,
               "copied asset cache resets across transport lifecycles")
    }

    private static func exportedModel(placeholder: Bool = false) -> WarcraftExportedModel {
        let image = WarcraftExportedImage(
            identity: "Textures/Footman.blp", placeholder: false, status: 0,
            width: 2, height: 2, rowBytes: 8, rgba8: [
                255, 0, 0, 255, 0, 255, 0, 255,
                0, 0, 255, 255, 255, 255, 0, 255,
            ], orientation: .topLeft)
        return WarcraftExportedModel(
            identity: "Units/Human/Footman/Footman.mdx", placeholder: placeholder,
            status: placeholder ? 6 : 0, metadataStatus: 0, version: 800,
            metadata: WarcraftAssetMetadata(
                category: .unit, classID: fourCC("hfoo"), teamColor: 1,
                tint: WarcraftColor(red: 0.8, green: 0.9, blue: 1, alpha: 1),
                footprint: WarcraftFootprint(width: 32, depth: 32)),
            bounds: WarcraftExportedBounds(
                min: WarcraftVector3(x: 0, y: 0, z: 0),
                max: WarcraftVector3(x: 1, y: 1, z: 1), radius: 1),
            geosets: [WarcraftExportedGeoset(
                positions: [
                    WarcraftVector3(x: 0, y: 0, z: 0),
                    WarcraftVector3(x: 1, y: 0, z: 0),
                    WarcraftVector3(x: 0, y: 1, z: 0),
                ], normals: Array(repeating: WarcraftVector3(x: 0, y: 0, z: 1), count: 3),
                textureCoordinates: [
                    WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                    WarcraftVector2(x: 0, y: 1),
                ], indices: [0, 1, 2], materialIndex: 0)],
            materials: [WarcraftExportedMaterial(firstLayer: 0, layerCount: 2)],
            layers: [
                WarcraftExportedLayer(blendMode: 0, flags: 0, textureIndex: 0, alpha: 1),
                WarcraftExportedLayer(blendMode: 3, flags: 1, textureIndex: 1, alpha: 0.75),
            ],
            textures: [
                WarcraftExportedTexture(identity: image.identity, replaceableID: 0, image: image),
                WarcraftExportedTexture(identity: "<replaceable:1>", replaceableID: 1, image: nil),
            ],
            teamColorImage: replacementImage("TeamColor01.blp", value: 48),
            sequences: [
                WarcraftExportedSequence(
                    name: "Stand", startMilliseconds: 0, endMilliseconds: 999, flags: 0),
            ],
            nodes: [WarcraftExportedNode(
                name: "Bone_Root", objectID: 0, parentID: UInt32.max, flags: 0,
                pivot: WarcraftVector3(x: 0, y: 0, z: 0),
                initialTranslation: WarcraftVector3(x: 0, y: 0, z: 0),
                initialRotation: (0, 0, 0, 1),
                initialScale: WarcraftVector3(x: 1, y: 1, z: 1))])
    }

    private static func terrainImage(_ identity: String, width: Int, height: Int) -> WarcraftExportedImage {
        WarcraftExportedImage(
            identity: identity, placeholder: false, status: 0, width: width, height: height,
            rowBytes: width * 4, rgba8: [UInt8](repeating: 255, count: width * height * 4),
            orientation: .topLeft)
    }

    private static func replacementImage(_ identity: String, value: UInt8,
                                         placeholder: Bool = false) -> WarcraftExportedImage {
        WarcraftExportedImage(
            identity: identity, placeholder: placeholder, status: placeholder ? 6 : 0,
            width: 1, height: 1, rowBytes: 4, rgba8: [value, value, value, 255], orientation: .topLeft)
    }

    private static func fourCC(_ text: String) -> UInt32 {
        text.utf8.enumerated().reduce(0) {
            $0 | UInt32($1.element) << UInt32($1.offset * 8)
        }
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
            expect(Set(fixture.entities.map(\.descriptor.category)) ==
                Set([.unit, .building, .resource, .doodad, .destructable]),
                   "fixture descriptors cover units, buildings, resources, doodads, and destructables")
            expect(fixture.fog != nil, "fixture scene creates exactly one fog descriptor")
            expect(fixture.entities.allSatisfy { !$0.usedPlaceholder },
                   "fixture test geometry resolves without production placeholders")
            var otherTeamScene = try FixtureWarcraftRenderProvider().scene(for: snapshot)
            otherTeamScene.entities[0].teamColor &+= 1
            let otherTeam = try WarcraftSceneBuilder.build(otherTeamScene)
            expect(fixture.entities[0].geometryKey == otherTeam.entities[0].geometryKey &&
                   fixture.entities[0].materialKey != otherTeam.entities[0].materialKey,
                   "same model geometry with a different team index rebuilds only material state")
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
                $0.contains("no copied asset descriptors")
            }, "missing live adapter values remain visible")
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
            var skinOnly = first
            skinOnly.generation = 4
            skinOnly.entities[0].materialKey = "entity-skin-content-b"
            var skinState = WarcraftRenderSceneState()
            _ = skinState.reconcile(first)
            let skinUpdate = skinState.reconcile(skinOnly)
            expect(skinUpdate.entityUpdates.first {
                $0.entity.descriptor.id == 1
            }.map { $0.materialChanged && !$0.geometryChanged } == true,
            "a new per-entity skin generation rebuilds materials without rebuilding shared geometry")
            var scaleOnly = first
            scaleOnly.generation = 5
            scaleOnly.entities[0].scale = WarcraftVector3(x: 2, y: 3, z: 4)
            var scaleState = WarcraftRenderSceneState()
            _ = scaleState.reconcile(first)
            let scaleUpdate = scaleState.reconcile(scaleOnly)
            expect(scaleUpdate.entityUpdates.first {
            $0.entity.descriptor.id == 1
            }.map { $0.scaleChanged && $0.transformChanged && !$0.geometryChanged } == true,
            "scale-only updates resize model children and collisions without rebuilding geometry")
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
            expect(key.fileName.hasPrefix("v2-texture-"), "disk keys are versioned and content hashed")
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

    @MainActor
    private static func testSessionModelLifecycle() async {
        let first = TabletopMapRecord(
            edition: .roc, source: .campaign, campaignIndex: 1, missionIndex: 1,
            campaign: "Campaign", title: "First", subtitle: "", mapPath: "Maps/First.w3m")
        let second = TabletopMapRecord(
            edition: .roc, source: .campaign, campaignIndex: 1, missionIndex: 2,
            campaign: "Campaign", title: "Second", subtitle: "", mapPath: "Maps/Second.w3m")
        let transport = SessionTestTransport()
        let model = TabletopSessionModel(
            modeName: "test", transport: transport,
            renderProvider: FixtureWarcraftRenderProvider(), commands: transport,
            catalogs: [.roc: [first, second]])
        do {
            try await model.prepare()
            expect(model.productState == .playing(first) && model.renderSnapshot.sessionID == 1,
                   "session preparation configures, starts, submits, and publishes the first active snapshot")
            model.selectMap(second.id)
            expect(model.selectedMapID == first.id, "running sessions reject map selection changes")
            await model.setPaused(true, reason: .background)
            await model.setPaused(true, reason: .user)
            await model.setPaused(false, reason: .background)
            expect(model.productState == .paused(first), "one remaining pause owner keeps simulation suspended")
            await model.setPaused(false, reason: .user)
            expect(model.productState == .playing(first), "last pause owner resumes simulation")
            model.smartPoint(x: 1, y: 2)
            let posted = await withTaskGroup(of: Bool.self) { group in
                group.addTask { await transport.waitForCommand() }
                group.addTask {
                    try? await Task.sleep(for: .seconds(1))
                    return false
                }
                let result = await group.next() ?? false
                group.cancelAll()
                return result
            }
            let running = await transport.state()
            expect(posted && running.starts == 1 && running.configured == [.roc] &&
                   running.maps == [first.mapPath] && running.suspends == 1 &&
                   running.resumes == 1 && running.commands == 1,
                   "session lifecycle owns product transport and typed command calls exactly once")
            await model.stop()
            let stopped = await transport.state()
            expect(stopped.stops == 1 && model.renderSnapshot == .empty,
                   "session stop cancels workers, stops transport once, and clears copied render state")
            model.selectMap(second.id)
            expect(model.selectedMapID == second.id, "stopped sessions accept a new map generation")
        } catch {
            expect(false, "session model lifecycle failed: \(error)")
            await model.stop()
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

private struct SessionTransportState: Sendable {
    var starts = 0
    var stops = 0
    var suspends = 0
    var resumes = 0
    var configured: [TabletopEdition] = []
    var maps: [String] = []
    var commands = 0
}

private actor SessionTestTransport: TabletopProductTransport, TabletopCommandTransport {
    private var value = SessionTransportState()
    private var active = false
    private var sessionID: UInt64 = 0
    private var generation: UInt64 = 0
    private let commandEvents: AsyncStream<Void>
    private let commandContinuation: AsyncStream<Void>.Continuation

    init() {
        (commandEvents, commandContinuation) = AsyncStream.makeStream(bufferingPolicy: .bufferingNewest(1))
    }

    func configure(edition: TabletopEdition) async throws { value.configured.append(edition) }
    func start() async throws {
        value.starts += 1; active = true; sessionID &+= 1; generation = 0
    }
    func submitMap(_ map: String) async throws { value.maps.append(map) }
    func poll() async throws -> TabletopSnapshot? {
        guard active else { return nil }
        generation &+= 1
        var snapshot = FixtureSnapshotSource.snapshot(generation: generation)
        snapshot.sessionID = sessionID
        snapshot.mapName = value.maps.last
        return snapshot
    }
    func suspend() async { value.suspends += 1 }
    func resume() async { value.resumes += 1 }
    func stop() async {
        guard active else { return }
        active = false; value.stops += 1
    }
    func post(_ command: TabletopCommand) async throws {
        guard active else { throw TabletopTransportError.terminal }
        guard command.sessionID == sessionID else { throw TabletopTransportError.staleSession }
        value.commands += 1
        commandContinuation.yield()
    }
    func waitForCommand() async -> Bool {
        var iterator = commandEvents.makeAsyncIterator()
        return await iterator.next() != nil
    }
    func state() -> SessionTransportState { value }
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
