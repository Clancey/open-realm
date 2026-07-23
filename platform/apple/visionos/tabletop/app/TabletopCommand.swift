enum TabletopCommand: Equatable, Sendable {
    case select(entityIDs: [UInt32], observedGeneration: UInt64, sessionID: UInt64)
    case smartEntity(entityID: UInt32, observedGeneration: UInt64, sessionID: UInt64)
    case smartPoint(x: Float, y: Float, observedGeneration: UInt64, sessionID: UInt64)
    case button(code: String, observedGeneration: UInt64, sessionID: UInt64)
    case cancel(observedGeneration: UInt64, sessionID: UInt64)

    var sessionID: UInt64 {
        switch self {
        case .select(_, _, let sessionID), .smartEntity(_, _, let sessionID),
             .smartPoint(_, _, _, let sessionID), .button(_, _, let sessionID),
             .cancel(_, let sessionID): return sessionID
        }
    }
}

protocol TabletopCommandTransport: Sendable {
    func post(_ command: TabletopCommand) async throws
}

enum TabletopTransportError: Error, Equatable, CustomStringConvertible {
    case configuration(String)
    case runtime(String)
    case abiVersion(expected: UInt32, actual: UInt32)
    case commandRejected(UInt32)
    case commandQueueFull
    case staleSession
    case invalidSnapshotEntity(UInt32)

    var description: String {
        switch self {
        case .configuration(let message), .runtime(let message): return message
        case .abiVersion(let expected, let actual):
            return "Tabletop transport ABI mismatch: expected \(expected), received \(actual)"
        case .commandRejected(let result): return "Tabletop command rejected with result \(result)"
        case .commandQueueFull: return "Fixture command queue is full"
        case .staleSession: return "Tabletop command belongs to a stopped session"
        case .invalidSnapshotEntity(let index): return "Tabletop snapshot entity \(index) could not be copied"
        }
    }
}

actor UnavailableTabletopTransport: TabletopSnapshotTransport {
    private let message: String

    init(_ message: String) { self.message = message }
    func start() async throws { throw TabletopTransportError.configuration(message) }
    func poll() async throws -> TabletopSnapshot? { nil }
}
