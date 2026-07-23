import SwiftUI

@MainActor
final class TabletopSessionModel: ObservableObject {
    @Published private(set) var renderSnapshot = TabletopRenderSnapshot.empty
    @Published private(set) var errorMessage: String?

    private let transport: any TabletopSnapshotTransport
    private var deduplicator = TabletopGenerationDeduplicator()
    private var pollingTask: Task<Void, Never>?

    init(transport: any TabletopSnapshotTransport) {
        self.transport = transport
    }

    func start() {
        guard pollingTask == nil else { return }
        pollingTask = Task { [weak self] in
            while let self, !Task.isCancelled {
                do {
                    let snapshot = try await transport.poll()
                    if let fresh = deduplicator.accept(snapshot) {
                        // Conversion copies transport values before RealityKit observes the published snapshot.
                        renderSnapshot = TabletopSnapshotConverter.convert(fresh)
                    }
                    try await Task.sleep(for: .milliseconds(33))
                } catch is CancellationError {
                    return
                } catch {
                    errorMessage = "Fixture transport failed: \(error)"
                    return
                }
            }
        }
    }

    func stop() {
        pollingTask?.cancel()
        pollingTask = nil
    }
}
