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
        testSnapshotValueValidation()
        testCommandLowering()
        testGestureTerminalSuppression()
        await testFixtureCommands()
        await testUnavailableTransport()
        await testPollingCancellation()
        await testFirstSnapshotTimeout()
        testSnapshotLeaseRelease()
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
            let fixture = try TabletopRuntimeModeResolver.resolve(environment: [:], resourcePath: "/resources")
            let live = try TabletopRuntimeModeResolver.resolve(
                environment: ["BZ_TABLETOP_MODE": "live", "BZ_TABLETOP_DATA_PATH": "/data",
                              "BZ_TABLETOP_MAP": "map.w3m"], resourcePath: "/resources")
            expect(fixture == .fixture, "fixture mode is the explicit default")
            expect(live == .live(dataPath: "/data", map: "map.w3m", connect: nil),
                "live mode lowers environment settings without fixture fallback")
            do {
                _ = try TabletopRuntimeModeResolver.resolve(
                    environment: ["BZ_TABLETOP_MODE": "invalid"], resourcePath: "/resources")
                expect(false, "invalid runtime mode was silently accepted")
            } catch TabletopTransportError.configuration {
                expect(true, "invalid runtime mode is actionable")
            }
            do {
                _ = try TabletopRuntimeModeResolver.resolve(
                    environment: ["BZ_TABLETOP_MODE": "live"], resourcePath: "/resources")
                expect(false, "targetless live mode was accepted")
            } catch TabletopTransportError.configuration {
                expect(true, "live mode requires an actionable map or connect target")
            }
        } catch {
            expect(false, "runtime mode selection unexpectedly failed: \(error)")
        }
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
