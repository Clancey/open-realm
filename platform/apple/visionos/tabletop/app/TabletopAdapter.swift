enum TabletopRuntimeMode: Equatable, Sendable {
    case fixture
    case live(dataPath: String, map: String?, connect: String?, tft: Bool)
}

enum TabletopProduct {
    static let bundleIdentifier = "org.openrealm.visionos.tabletop"
    static let executable = "OpenRealmTabletop"
    static let bundledDataRelativePath = "Resources/Warcraft III"
    static let defaultMap = "Human02"
}

enum TabletopRuntimeModeResolver {
    static func resolve(environment: [String: String], bundlePath: String?) throws -> TabletopRuntimeMode {
        switch environment["BZ_TABLETOP_MODE"] ?? "live" {
        case "fixture": return .fixture
        case "live":
            guard let dataPath = nonEmpty(environment["BZ_TABLETOP_DATA_PATH"]) ??
                    bundlePath.map({ $0 + "/" + TabletopProduct.bundledDataRelativePath }) else {
                throw TabletopTransportError.configuration(
                    "Live mode requires bundled Warcraft III data or BZ_TABLETOP_DATA_PATH")
            }
            let connect = nonEmpty(environment["BZ_TABLETOP_CONNECT"])
            let map = nonEmpty(environment["BZ_TABLETOP_MAP"]) ?? (connect == nil ? TabletopProduct.defaultMap : nil)
            let tft: Bool
            switch environment["BZ_TABLETOP_TFT"] {
            case nil, "0": tft = false
            case "1": tft = true
            case let value?: throw TabletopTransportError.configuration(
                "BZ_TABLETOP_TFT must be 0 or 1, got '\(value)'")
            }
            return .live(dataPath: dataPath, map: map, connect: connect, tft: tft)
        case let mode: throw TabletopTransportError.configuration("Unknown BZ_TABLETOP_MODE '\(mode)'")
        }
    }

    private static func nonEmpty(_ value: String?) -> String? {
        guard let value, !value.isEmpty else { return nil }
        return value
    }
}

enum TabletopConfigStringCopy {
    static func copy(count: UInt32, valueAt: (UInt32) -> String?) -> [UInt32: String] {
        var values: [UInt32: String] = [:]
        values.reserveCapacity(Int(count))
        for index in 0..<count { values[index] = valueAt(index) ?? "" }
        return values
    }

}

enum TabletopCoordinateConversion {
    /* Swapping engine Y/Z changes handedness, so positive engine yaw becomes negative RealityKit yaw. */
    static func heading(_ value: Float) -> Float { -value }
}

struct TabletopDataEntry: Equatable, Sendable {
    var relativePath: String
    var isRegularFile: Bool
}

enum TabletopDataPreflight {
    static func isUsable(entries: [TabletopDataEntry], localMapRequired: Bool) -> Bool {
        let paths = entries.filter(\.isRegularFile).map {
            String($0.relativePath.lowercased().map { $0 == "\\" ? "/" : $0 })
        }
        if paths.contains(where: { $0.hasSuffix(".mpq") }) { return true }
        let hasCommonJ = paths.contains("scripts/common.j")
        let hasLooseMap = paths.contains(where: { $0.hasSuffix(".w3m") || $0.hasSuffix(".w3x") })
        return hasCommonJ && (!localMapRequired || hasLooseMap)
    }
}

enum TabletopSnapshotValueValidator {
    static func fog(width: UInt32, height: UInt32, visible: [UInt8],
                    explored: [UInt8]) throws -> TabletopFogSnapshot {
        guard height == 0 || width <= UInt32.max / height,
              let count = Int(exactly: UInt64(width) * UInt64(height)) else {
            throw TabletopTransportError.malformedSnapshot("fog dimensions overflow")
        }
        guard visible.count == count, explored.count == count else {
            throw TabletopTransportError.malformedSnapshot("fog plane length does not match dimensions")
        }
        return TabletopFogSnapshot(width: width, height: height, visible: visible, explored: explored)
    }

    static func layoutCounts(buttons: UInt8, inventory: UInt8, queue: UInt8) throws {
        guard buttons <= 12, inventory <= 6, queue <= 7 else {
            throw TabletopTransportError.malformedSnapshot("unit layout counts exceed ABI bounds")
        }
    }
}

enum TabletopSnapshotDiagnostics {
    static func message(overflow: UInt32, duplicates: UInt32) -> String? {
        guard overflow > 0 || duplicates > 0 else { return nil }
        return "Snapshot capped \(overflow) entities and ignored \(duplicates) duplicate IDs."
    }
}

enum TabletopLifecycleState: Int, Equatable, Sendable {
    case idle = 0
    case starting
    case running
    case failed
    case suspended
    case stopped
}

enum TabletopLoweredCommand: Equatable, Sendable {
    case select([UInt32], UInt64)
    case smartEntity(UInt32, UInt64)
    case smartPoint(Float, Float, UInt64)
    case button([Int8], UInt64)
    case cancel(UInt64)
}

enum TabletopCommandLowering {
    static let maxEntityID: UInt32 = 16_384
    static let maxSelectionCount = 64
    static let maxWorldCoordinate: Float = 1_000_000

    static func lower(_ command: TabletopCommand, currentSessionID: UInt64) throws -> TabletopLoweredCommand {
        guard command.sessionID == currentSessionID else { throw TabletopTransportError.staleSession }
        switch command {
        case .select(let ids, let generation, _):
            guard !ids.isEmpty, ids.count <= maxSelectionCount, ids.allSatisfy({ $0 < maxEntityID }) else {
                throw TabletopTransportError.invalidCommand("Selection must contain 1...\(maxSelectionCount) valid IDs")
            }
            return .select(ids, generation)
        case .smartEntity(let id, let generation, _):
            guard id < maxEntityID else { throw TabletopTransportError.invalidCommand("Invalid smart-target ID") }
            return .smartEntity(id, generation)
        case .smartPoint(let x, let y, let generation, _):
            guard x.isFinite, y.isFinite, abs(x) <= maxWorldCoordinate, abs(y) <= maxWorldCoordinate else {
                throw TabletopTransportError.invalidCommand("Invalid smart-target point")
            }
            return .smartPoint(x, y, generation)
        case .button(let code, let generation, _):
            let bytes = code.utf8.map { Int8(bitPattern: $0) }
            guard bytes.count == 4 else {
                throw TabletopTransportError.invalidCommand("Button code must contain exactly four UTF-8 bytes")
            }
            return .button(bytes, generation)
        case .cancel(let generation, _): return .cancel(generation)
        }
    }
}

enum TabletopCommandResult: UInt32 {
    case ok = 0
    case notInitialized
    case terminal
    case abiVersion
    case queueFull
    case invalidArgument
    case staleGeneration

    var error: TabletopTransportError? {
        switch self {
        case .ok: return nil
        case .notInitialized: return .notInitialized
        case .terminal: return .terminal
        case .abiVersion: return .abiVersionRejected
        case .queueFull: return .commandQueueFull
        case .invalidArgument: return .invalidCommand("Layer-2 transport rejected the command payload")
        case .staleGeneration: return .staleGeneration
        }
    }
}

enum TabletopPolling {
    static func firstSnapshot(transport: any TabletopSnapshotTransport, attempts: Int,
                              sleep: @Sendable () async throws -> Void) async throws -> TabletopSnapshot {
        for _ in 0..<attempts {
            try Task.checkCancellation()
            if let snapshot = try await transport.poll() { return snapshot }
            try await sleep()
        }
        throw TabletopTransportError.startupTimedOut
    }

    static func run(transport: any TabletopSnapshotTransport,
                    sleep: @Sendable () async throws -> Void,
                    receive: @Sendable (TabletopSnapshot) async -> Void) async throws {
        while true {
            try Task.checkCancellation()
            if let snapshot = try await transport.poll() { await receive(snapshot) }
            try await sleep()
        }
    }
}

protocol TabletopSnapshotLease {
    associatedtype Value
    func copyValue() throws -> Value
    func release()
}

enum TabletopSnapshotLeaseConsumer {
    static func consume<L: TabletopSnapshotLease>(_ lease: L) throws -> L.Value {
        defer { lease.release() }
        return try lease.copyValue()
    }
}
