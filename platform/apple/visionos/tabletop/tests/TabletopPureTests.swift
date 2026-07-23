@main
enum TabletopPureTests {
    private static var failures = 0

    static func main() async {
        testLauncherReduction()
        testGenerationDeduplication()
        testPlacement()
        testFixtureConversionAndReconciliation()
        testWorldConversion()
        testGestureTerminalSuppression()
        await testFixtureCommands()
        await testUnavailableTransport()
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
}
