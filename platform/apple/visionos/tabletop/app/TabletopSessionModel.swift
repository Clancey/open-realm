import Foundation
import SwiftUI

@MainActor
final class TabletopSessionModel: ObservableObject {
    @Published private(set) var renderSnapshot = TabletopRenderSnapshot.empty
    @Published private(set) var errorMessage: String?
    @Published private(set) var snapshotDiagnostic: String?

    private let transport: any TabletopSnapshotTransport
    private let commandTransport: (any TabletopCommandTransport)?
    private let renderPipeline: WarcraftRenderPipeline
    private var pollingTask: Task<Void, Never>?
    private var commandEpoch: UInt64 = 0
    private var phase = TabletopLifecycleState.idle
    let modeName: String

    init(modeName: String, transport: any TabletopSnapshotTransport,
         renderProvider: any WarcraftRenderDescriptorProvider,
         commands: (any TabletopCommandTransport)? = nil) {
        self.modeName = modeName
        self.transport = transport
        renderPipeline = WarcraftRenderPipeline(provider: renderProvider)
        self.commandTransport = commands
    }

    func prepare() async throws {
        if phase == .running { return }
        if let current = pollingTask {
            guard current.isCancelled else {
                throw TabletopTransportError.runtime("Tabletop polling is already active")
            }
            await current.value
        }
        guard phase == .idle else { throw TabletopTransportError.runtime("Tabletop startup is already in progress") }
        phase = .starting
        commandEpoch &+= 1
        renderSnapshot = .empty
        errorMessage = nil
        snapshotDiagnostic = nil
        await renderPipeline.reset()
        do {
            try await transport.start()
            let first = try await TabletopPolling.firstSnapshot(
                transport: transport, attempts: 90, sleep: { try await Task.sleep(for: .milliseconds(33)) })
            guard let prepared = try await renderPipeline.prepare(first) else {
                throw TabletopTransportError.runtime("First renderer generation was unexpectedly deduplicated")
            }
            consume(prepared)
            FileHandle.standardError.write(Data(
                "OpenRealmTabletop: first snapshot generation \(first.generation), map \(first.mapName ?? "<none>")\n".utf8))
            phase = .running
        } catch {
            await transport.stop()
            phase = .idle
            throw error
        }
        let pipeline = renderPipeline
        pollingTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await TabletopPolling.run(
                    transport: transport, sleep: { try await Task.sleep(for: .milliseconds(33)) },
                    receive: { [weak self] snapshot in
                        do {
                            if let prepared = try await pipeline.prepare(snapshot) {
                                await self?.consume(prepared)
                            }
                        } catch { await self?.rendererFailed(error) }
                    })
            } catch is CancellationError {
                // Cancellation is normal when the immersive scene closes.
            } catch {
                errorMessage = "Tabletop transport failed: \(error)"
            }
            await transport.stop()
            phase = .idle
            pollingTask = nil
        }
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
    }

    private func rendererFailed(_ error: Error) { errorMessage = "Tabletop renderer failed: \(error)" }

    func select(entityID: UInt64) {
        guard let commandTransport, let id = UInt32(exactly: entityID), renderSnapshot.sessionID != 0 else { return }
        let epoch = commandEpoch, sessionID = renderSnapshot.sessionID
        let command = TabletopCommand.select(entityIDs: [id], observedGeneration: 0,
                                              sessionID: sessionID)
        Task {
            guard commandEpoch == epoch else { return }
            do { try await commandTransport.post(command) }
            catch where commandEpoch == epoch { errorMessage = "Tabletop command failed: \(error)" }
        }
    }

    func stop() async {
        commandEpoch &+= 1
        renderSnapshot = .empty
        snapshotDiagnostic = nil
        guard let pollingTask else {
            await transport.stop()
            phase = .idle
            return
        }
        phase = .stopped
        pollingTask.cancel()
        await pollingTask.value
        phase = .idle
    }
}
