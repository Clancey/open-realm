import SwiftUI

@MainActor
final class TabletopSessionModel: ObservableObject {
    @Published private(set) var renderSnapshot = TabletopRenderSnapshot.empty
    @Published private(set) var errorMessage: String?

    private let transport: any TabletopSnapshotTransport
    private let commandTransport: (any TabletopCommandTransport)?
    private var deduplicator = TabletopGenerationDeduplicator()
    private var pollingTask: Task<Void, Never>?
    private var commandEpoch: UInt64 = 0
    private var phase = TabletopLifecycleState.idle
    let modeName: String

    init(modeName: String, transport: any TabletopSnapshotTransport,
         commands: (any TabletopCommandTransport)? = nil) {
        self.modeName = modeName
        self.transport = transport
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
        deduplicator = TabletopGenerationDeduplicator()
        do {
            try await transport.start()
            let first = try await TabletopPolling.firstSnapshot(
                transport: transport, attempts: 90, sleep: { try await Task.sleep(for: .milliseconds(33)) })
            consume(first)
            phase = .running
        } catch {
            await transport.stop()
            phase = .idle
            throw error
        }
        pollingTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await TabletopPolling.run(
                    transport: transport, sleep: { try await Task.sleep(for: .milliseconds(33)) },
                    receive: { [weak self] snapshot in await self?.consume(snapshot) })
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

    private func consume(_ snapshot: TabletopSnapshot) {
        guard let fresh = deduplicator.accept(snapshot) else { return }
        // The transport has already released all C storage before this main-actor conversion.
        renderSnapshot = TabletopSnapshotConverter.convert(fresh)
        if fresh.entitiesOverflowCount > 0 || fresh.duplicateEntityCount > 0 {
            errorMessage = "Snapshot capped \(fresh.entitiesOverflowCount) entities and ignored " +
                "\(fresh.duplicateEntityCount) duplicate IDs."
        }
    }

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
