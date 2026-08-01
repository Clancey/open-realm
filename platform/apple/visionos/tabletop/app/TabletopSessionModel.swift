import Foundation
import SwiftUI

@MainActor
final class TabletopSessionModel: ObservableObject {
    private static let currentTransportGeneration: UInt64 = 0
    @Published private(set) var renderSnapshot = TabletopRenderSnapshot.empty
    @Published private(set) var errorMessage: String?
    @Published private(set) var snapshotDiagnostic: String?
    @Published private(set) var productState: TabletopProductState
    @Published private(set) var selectedEdition: TabletopEdition
    @Published private(set) var selectedMapID: String?

    private let transport: any TabletopSnapshotTransport
    private let productTransport: (any TabletopProductTransport)?
    private let commandTransport: (any TabletopCommandTransport)?
    private let renderPipeline: WarcraftRenderPipeline
    private let catalogs: [TabletopEdition: [TabletopMapRecord]]
    private let progressStore: TabletopProgressStore?
    private let snapshotMailbox = TabletopSnapshotMailbox()
    private let preparedMailbox = TabletopPreparedSnapshotMailbox()
    private var pollingTask: Task<Void, Never>?
    private var processingTask: Task<Void, Never>?
    private var commandEpoch: UInt64 = 0
    private var phase = TabletopLifecycleState.idle
    private var pauseReasons = Set<TabletopPauseReason>()
    let modeName: String

    init(modeName: String, transport: any TabletopSnapshotTransport,
         renderProvider: any WarcraftRenderDescriptorProvider,
         commands: (any TabletopCommandTransport)? = nil,
         catalogs: [TabletopEdition: [TabletopMapRecord]] = [:],
         initialEdition: TabletopEdition = .roc,
         progressStore: TabletopProgressStore? = nil,
         unavailableReason: String? = nil) {
        self.modeName = modeName
        self.transport = transport
        productTransport = transport as? any TabletopProductTransport
        renderPipeline = WarcraftRenderPipeline(provider: renderProvider)
        self.commandTransport = commands
        self.catalogs = catalogs
        self.progressStore = progressStore
        selectedEdition = initialEdition
        let maps = catalogs[initialEdition] ?? []
        var savedMap: String?
        if let progressStore {
            do { savedMap = try progressStore.load(initialEdition).selectedMap }
            catch {
                FileHandle.standardError.write(Data(
                    "OpenRealmTabletopPersistence: \(error)\n".utf8))
            }
        }
        selectedMapID = maps.first(where: { $0.mapPath == savedMap })?.id ?? maps.first?.id
        if let unavailableReason { productState = .missingData(unavailableReason) }
        else if maps.isEmpty { productState = .missingData("No playable maps were discovered.") }
        else { productState = .menu }
    }

    var availableMaps: [TabletopMapRecord] { catalogs[selectedEdition] ?? [] }
    var selectedMap: TabletopMapRecord? { availableMaps.first { $0.id == selectedMapID } }
    var acceptsGameplayInput: Bool {
        if case .playing = productState { return true }
        return false
    }

    func selectEdition(_ edition: TabletopEdition) {
        guard phase == .idle else { return }
        selectedEdition = edition
        let maps = catalogs[edition] ?? []
        var savedMap: String?
        if let progressStore {
            do { savedMap = try progressStore.load(edition).selectedMap }
            catch {
                productState = .failed("Stored \(edition.title) progress is corrupt: \(error)")
                return
            }
        }
        selectedMapID = maps.first(where: { $0.mapPath == savedMap })?.id ?? maps.first?.id
        productState = maps.isEmpty ? .missingData("No \(edition.title) maps were discovered.") : .menu
    }

    func selectMap(_ id: String) {
        guard phase == .idle, availableMaps.contains(where: { $0.id == id }) else { return }
        selectedMapID = id
    }

    func prepare() async throws {
        guard let map = selectedMap else {
            throw TabletopTransportError.configuration("Choose a playable map before starting")
        }
        try await prepare(map)
    }

    private func prepare(_ map: TabletopMapRecord) async throws {
        if phase == .running { return }
        if let current = pollingTask {
            guard current.isCancelled else {
                throw TabletopTransportError.runtime("Tabletop polling is already active")
            }
            await current.value
        }
        if let current = processingTask {
            current.cancel()
            await current.value
            processingTask = nil
        }
        guard phase == .idle else { throw TabletopTransportError.runtime("Tabletop startup is already in progress") }
        phase = .starting
        productState = TabletopProductReducer.reduce(.menu, .launch(map))
        commandEpoch &+= 1
        renderSnapshot = .empty
        errorMessage = nil
        snapshotDiagnostic = nil
        await renderPipeline.reset()
        await snapshotMailbox.reset()
        await preparedMailbox.reset()
        do {
            try await productTransport?.configure(edition: map.edition)
            try await transport.start()
            try await productTransport?.submitMap(map.mapPath)
            let first = try await firstActiveSnapshot(attempts: 240)
            guard let prepared = try await renderPipeline.prepare(first) else {
                throw TabletopTransportError.runtime("First renderer generation was unexpectedly deduplicated")
            }
            consume(prepared)
            FileHandle.standardError.write(Data(
                "OpenRealmTabletop: first snapshot generation \(first.generation), map \(first.mapName ?? "<none>")\n".utf8))
            phase = .running
            productState = TabletopProductReducer.reduce(productState, .loaded)
            try saveSelection(map)
        } catch {
            await transport.stop()
            await renderPipeline.reset()
            phase = .idle
            productState = .failed("Loading \(map.title) failed: \(error)")
            throw error
        }
        let pipeline = renderPipeline, transport = transport
        let snapshots = snapshotMailbox, prepared = preparedMailbox
        let epoch = commandEpoch
        pollingTask = Task.detached { [weak self] in
            var terminalMessage: String?
            do {
                try await TabletopPolling.run(
                    transport: transport, sleep: { try await Task.sleep(for: .milliseconds(33)) },
                    receive: { [weak self] snapshot in
                        guard await snapshots.submit(snapshot) else { return }
                        await self?.startProcessing(
                            epoch: epoch, pipeline: pipeline, snapshots: snapshots, prepared: prepared)
                    })
            } catch is CancellationError {
                // Cancellation is normal when the immersive scene closes.
            } catch {
                terminalMessage = "Tabletop transport failed: \(error)"
            }

            await transport.stop()
            await self?.pollingFinished(terminalMessage, epoch: epoch)
        }
    }

    private func firstActiveSnapshot(attempts: Int) async throws -> TabletopSnapshot {
        for _ in 0..<attempts {
            try Task.checkCancellation()
            if let snapshot = try await transport.poll(), snapshot.connectionState == .active {
                return snapshot
            }
            try await Task.sleep(for: .milliseconds(33))
        }
        throw TabletopTransportError.startupTimedOut
    }

    /* The worker is owned by the session so stop/restart cannot leak an old generation into a new pipeline. */
    private func startProcessing(epoch: UInt64, pipeline: WarcraftRenderPipeline,
                                 snapshots: TabletopSnapshotMailbox,
                                 prepared: TabletopPreparedSnapshotMailbox) {
        guard commandEpoch == epoch, phase == .running else { return }
        processingTask = Task.detached { [weak self] in
            repeat {
                if Task.isCancelled { break }
                guard let snapshot = await snapshots.take() else { break }
                do {
                    if let output = try await pipeline.prepare(snapshot) {
                        try Task.checkCancellation()
                        _ = await prepared.submit(output)
                        await self?.consumePrepared(epoch: epoch, prepared: prepared)
                    }
                } catch is CancellationError {
                    break
                } catch {
                    await self?.rendererFailed(error, epoch: epoch)
                }
            } while await snapshots.finish()
        }
    }

    private func consumePrepared(epoch: UInt64, prepared: TabletopPreparedSnapshotMailbox) async {
        guard let output = await prepared.take(),
              commandEpoch == epoch, phase == .running else { return }
        consume(output)
    }

    private func consume(_ prepared: WarcraftPreparedSnapshot) {
        // The provider actor has copied and converted all descriptor values before this main-actor handoff.
        renderSnapshot = TabletopSnapshotConverter.convert(prepared)
        let transportMessage = TabletopSnapshotDiagnostics.message(
            overflow: prepared.snapshot.entitiesOverflowCount,
            duplicates: prepared.snapshot.duplicateEntityCount)
        snapshotDiagnostic = ([transportMessage].compactMap { $0 } +
            (renderSnapshot.warcraft?.diagnostics ?? [])).joined(separator: "\n")
        if snapshotDiagnostic?.isEmpty == true { snapshotDiagnostic = nil }
        if let result = prepared.snapshot.player?.gameResult, result != .none {
            let previous = productState
            productState = TabletopProductReducer.reduce(previous, .result(result))
            if TabletopProductReducer.recordsCompletion(from: previous, to: productState) { recordCompletion() }
        }
    }

    private func rendererFailed(_ error: Error, epoch: UInt64) {
        guard commandEpoch == epoch, phase == .running else { return }
        errorMessage = "Tabletop renderer failed: \(error)"
    }

    private func pollingFinished(_ terminalMessage: String?, epoch: UInt64) async {
        guard commandEpoch == epoch else { return }
        let processing = processingTask
        processing?.cancel()
        await processing?.value
        processingTask = nil
        await snapshotMailbox.reset()
        await preparedMailbox.reset()
        await renderPipeline.reset()
        if let terminalMessage {
            errorMessage = terminalMessage
            FileHandle.standardError.write(Data("OpenRealmTabletop: \(terminalMessage)\n".utf8))
        }
        phase = .idle
        pollingTask = nil
    }

    func select(_ hit: TabletopEntityHit, mode: TabletopSelectionMode) {
        do {
            let ids = try TabletopHitValidation.selection(hit, mode: mode, in: renderSnapshot)
            submit(.select(entityIDs: ids, observedGeneration: Self.currentTransportGeneration,
                           sessionID: hit.sessionID))
        } catch { commandFailed(error) }
    }

    func smartEntity(_ hit: TabletopEntityHit) {
        do {
            let id = try TabletopHitValidation.entityID(hit, in: renderSnapshot)
            submit(.smartEntity(entityID: id, observedGeneration: Self.currentTransportGeneration,
                                sessionID: hit.sessionID))
        } catch { commandFailed(error) }
    }

    func targetEntity(_ hit: TabletopEntityHit) {
        do {
            guard renderSnapshot.actionLayout.currentTarget.acceptsEntity else {
                throw TabletopTransportError.invalidInteractionState("The server is not accepting an entity target")
            }
            let id = try TabletopHitValidation.entityID(hit, in: renderSnapshot)
            submit(.select(entityIDs: [id], observedGeneration: Self.currentTransportGeneration,
                           sessionID: hit.sessionID))
        } catch { commandFailed(error) }
    }

    func smartPoint(x: Float, y: Float) {
        submit(.smartPoint(x: x, y: y, observedGeneration: Self.currentTransportGeneration,
                           sessionID: renderSnapshot.sessionID))
    }

    func targetPoint(x: Float, y: Float) {
        guard renderSnapshot.actionLayout.currentTarget.acceptsPoint else {
            commandFailed(TabletopTransportError.invalidInteractionState(
                "The server is not accepting a point target"))
            return
        }
        submit(.targetPoint(x: x, y: y, observedGeneration: Self.currentTransportGeneration,
                            sessionID: renderSnapshot.sessionID))
    }

    func activate(_ button: TabletopActionButtonSnapshot) {
        do {
            submit(try TabletopActionValidation.command(
                button, layout: renderSnapshot.actionLayout, generation: Self.currentTransportGeneration,
                sessionID: renderSnapshot.sessionID))
        } catch { commandFailed(error) }
    }

    func cancelTargeting() {
        guard renderSnapshot.actionLayout.currentTarget != .none else {
            commandFailed(TabletopTransportError.invalidInteractionState(
                "There is no authoritative target mode to cancel"))
            return
        }
        submit(.cancel(observedGeneration: Self.currentTransportGeneration, sessionID: renderSnapshot.sessionID))
    }

    private func submit(_ command: TabletopCommand) {
        guard let commandTransport, phase == .running, renderSnapshot.sessionID != 0 else {
            commandFailed(TabletopTransportError.terminal)
            return
        }
        let epoch = commandEpoch
        Task {
            guard commandEpoch == epoch else { return }
            do { try await commandTransport.post(command) }
            catch where commandEpoch == epoch { errorMessage = "Tabletop command failed: \(error)" }
        }
    }

    private func commandFailed(_ error: Error) {
        let message = "Tabletop command failed: \(error)"
        errorMessage = message
        FileHandle.standardError.write(Data("OpenRealmTabletop: \(message)\n".utf8))
    }

    func setPaused(_ paused: Bool, reason: TabletopPauseReason) async {
        guard phase == .running else { return }
        if paused {
            let wasEmpty = pauseReasons.isEmpty
            pauseReasons.insert(reason)
            if wasEmpty {
                await productTransport?.suspend()
                productState = TabletopProductReducer.reduce(productState, .pause)
            }
        } else {
            pauseReasons.remove(reason)
            if pauseReasons.isEmpty {
                await productTransport?.resume()
                productState = TabletopProductReducer.reduce(productState, .resume)
            }
        }
    }

    func retry() async {
        guard case .terminal(let map, _) = productState else { return }
        await stop()
        productState = TabletopProductReducer.reduce(.terminal(map, .none), .retry)
        do { try await prepare(map) }
        catch { productState = .failed("Retry failed: \(error)") }
    }

    var nextMap: TabletopMapRecord? {
        guard case .terminal(let map, .victory) = productState, map.source == .campaign else { return nil }
        return availableMaps.first {
            $0.source == .campaign && $0.campaignIndex == map.campaignIndex &&
            $0.missionIndex == map.missionIndex + 1
        }
    }

    func startNextMap() async {
        guard let map = nextMap else { return }
        await stop()
        selectedMapID = map.id
        do { try await prepare(map) }
        catch { productState = .failed("Next map failed: \(error)") }
    }

    func returnToMenu() async {
        await stop()
        pauseReasons.removeAll()
        productState = TabletopProductReducer.reduce(productState, .returnToMenu)
    }

    private func saveSelection(_ map: TabletopMapRecord) throws {
        guard let progressStore else { return }
        var value = try progressStore.load(map.edition)
        value.selectedMap = map.mapPath
        try progressStore.save(value, edition: map.edition)
    }

    private func recordCompletion() {
        guard let progressStore, let map = selectedMap else { return }
        do {
            var value = try progressStore.load(map.edition)
            value.completedMaps.insert(map.mapPath)
            try progressStore.save(value, edition: map.edition)
        } catch {
            errorMessage = "Progress could not be saved: \(error)"
            FileHandle.standardError.write(Data("OpenRealmTabletopPersistence: \(error)\n".utf8))
        }
    }

    func stop() async {
        commandEpoch &+= 1
        renderSnapshot = .empty
        snapshotDiagnostic = nil
        guard pollingTask != nil || processingTask != nil else {
            await transport.stop()
            await renderPipeline.reset()
            phase = .idle
            return
        }
        phase = .stopped
        let polling = pollingTask, processing = processingTask
        polling?.cancel()
        processing?.cancel()
        await polling?.value
        await processing?.value
        pollingTask = nil
        processingTask = nil
        await renderPipeline.reset()
        await snapshotMailbox.reset()
        await preparedMailbox.reset()
        phase = .idle
    }
}
