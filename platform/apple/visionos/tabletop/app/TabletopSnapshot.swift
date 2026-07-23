struct TabletopVector3: Equatable, Sendable {
    var x: Float
    var y: Float
    var z: Float
}

enum TabletopTerrainKind: UInt8, Equatable, Sendable {
    case grass
    case dirt
    case water
}

struct TabletopTerrainTile: Equatable, Sendable {
    var id: Int
    var x: Int
    var z: Int
    var elevation: Float
    var kind: TabletopTerrainKind
}

enum TabletopEntityKind: UInt8, Equatable, Sendable {
    case worker
    case soldier
    case building
}

struct TabletopEntitySnapshot: Equatable, Sendable {
    var id: UInt64
    var kind: TabletopEntityKind
    var position: TabletopVector3
    var heading: Float
    var selected: Bool
}

struct TabletopSnapshot: Equatable, Sendable {
    var generation: UInt64
    var terrain: [TabletopTerrainTile]
    var entities: [TabletopEntitySnapshot]
}

protocol TabletopSnapshotTransport: Sendable {
    func poll() async throws -> TabletopSnapshot
}

struct TabletopGenerationDeduplicator {
    private(set) var generation: UInt64?

    mutating func accept(_ snapshot: TabletopSnapshot) -> TabletopSnapshot? {
        guard generation != snapshot.generation else { return nil }
        generation = snapshot.generation
        return snapshot
    }
}
