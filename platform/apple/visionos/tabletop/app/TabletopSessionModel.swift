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

    init(transport: any TabletopSnapshotTransport, commands: (any TabletopCommandTransport)? = nil) {
        self.transport = transport
        self.commandTransport = commands
    }

    func start() async {
        if let current = pollingTask {
            guard current.isCancelled else { return }
            await current.value
        }
        commandEpoch &+= 1
        renderSnapshot = .empty
        errorMessage = nil
        deduplicator = TabletopGenerationDeduplicator()
        pollingTask = Task { [weak self] in
            guard let self else { return }
            do {
                try await transport.start()
                while !Task.isCancelled {
                    if let snapshot = try await transport.poll(), let fresh = deduplicator.accept(snapshot) {
                        // Conversion copies transport values before RealityKit observes the published snapshot.
                        renderSnapshot = TabletopSnapshotConverter.convert(fresh)
                        if fresh.entitiesOverflowCount > 0 || fresh.duplicateEntityCount > 0 {
                            errorMessage = "Snapshot capped \(fresh.entitiesOverflowCount) entities and ignored " +
                                "\(fresh.duplicateEntityCount) duplicate IDs."
                        }
                    }
                    try await Task.sleep(for: .milliseconds(33))
                }
            } catch is CancellationError {
                // Cancellation is normal when the immersive scene closes.
            } catch {
                errorMessage = "Tabletop transport failed: \(error)"
            }
            await transport.stop()
            pollingTask = nil
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

    func stop() {
        commandEpoch &+= 1
        renderSnapshot = .empty
        pollingTask?.cancel()
    }
}
