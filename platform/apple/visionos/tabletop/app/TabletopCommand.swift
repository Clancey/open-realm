enum TabletopCommand: Equatable, Sendable {
    case select(entityIDs: [UInt32], observedGeneration: UInt64, sessionID: UInt64)
    case smartEntity(entityID: UInt32, observedGeneration: UInt64, sessionID: UInt64)
    case smartPoint(x: Float, y: Float, observedGeneration: UInt64, sessionID: UInt64)
    case targetPoint(x: Float, y: Float, observedGeneration: UInt64, sessionID: UInt64)
    case button(code: String, observedGeneration: UInt64, sessionID: UInt64)
    case cancel(observedGeneration: UInt64, sessionID: UInt64)

    var sessionID: UInt64 {
        switch self {
        case .select(_, _, let sessionID), .smartEntity(_, _, let sessionID),
             .smartPoint(_, _, _, let sessionID), .targetPoint(_, _, _, let sessionID),
             .button(_, _, let sessionID),
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
    case notInitialized
    case terminal
    case abiVersionRejected
    case staleGeneration
    case invalidCommand(String)
    case startupTimedOut
    case malformedSnapshot(String)
    case invalidSnapshotEntity(UInt32)
    case staleEntityHit(UInt64)
    case missingSemanticAction(String)
    case invalidInteractionState(String)

    var description: String {
        switch self {
        case .configuration(let message), .runtime(let message): return message
        case .abiVersion(let expected, let actual):
            return "Tabletop transport ABI mismatch: expected \(expected), received \(actual)"
        case .commandRejected(let result): return "Tabletop command rejected with result \(result)"
        case .commandQueueFull: return "Tabletop command queue is full"
        case .staleSession: return "Tabletop command belongs to a stopped session"
        case .notInitialized: return "Tabletop transport is not initialized"
        case .terminal: return "Tabletop transport has stopped"
        case .abiVersionRejected: return "Tabletop transport rejected this ABI version"
        case .staleGeneration: return "Tabletop command was based on a stale snapshot"
        case .invalidCommand(let message): return message
        case .startupTimedOut: return "Timed out waiting for the first live tabletop snapshot"
        case .malformedSnapshot(let message): return "Malformed tabletop snapshot: \(message)"
        case .invalidSnapshotEntity(let index): return "Tabletop snapshot entity \(index) could not be copied"
        case .staleEntityHit(let id): return "Tabletop entity hit \(id) belongs to a stale snapshot"
        case .missingSemanticAction(let code): return "Tabletop action '\(code)' has no supported semantic command"
        case .invalidInteractionState(let message): return message
        }
    }
}

actor UnavailableTabletopTransport: TabletopSnapshotTransport {
    private let message: String

    init(_ message: String) { self.message = message }
    func start() async throws { throw TabletopTransportError.configuration(message) }
    func poll() async throws -> TabletopSnapshot? { nil }
}
