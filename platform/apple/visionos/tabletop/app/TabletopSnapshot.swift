struct TabletopVector3: Equatable, Sendable {
    var x: Float
    var y: Float
    var z: Float
}

struct TabletopBounds2: Equatable, Sendable {
    var minX: Float
    var minZ: Float
    var maxX: Float
    var maxZ: Float
}

enum TabletopCoordinateSpace: Equatable, Sendable {
    case fixtureBoard
    case world(TabletopBounds2?)
}

enum TabletopConnectionState: UInt8, Equatable, Sendable {
    case disconnected
    case connecting
    case connected
    case active
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
    case unit
}

struct TabletopEntitySnapshot: Equatable, Sendable {
    var id: UInt64
    var kind: TabletopEntityKind
    var position: TabletopVector3
    var heading: Float
    var selected: Bool
    var metadata = TabletopEntityMetadata()
}

struct TabletopSnapshot: Equatable, Sendable {
    var abiVersion: UInt32 = 0
    var generation: UInt64
    var terrain: [TabletopTerrainTile]
    var entities: [TabletopEntitySnapshot]
    var sessionID: UInt64 = 0
    var coordinateSpace = TabletopCoordinateSpace.fixtureBoard
    var connectionState = TabletopConnectionState.active
    var mapName: String?
    var player: TabletopPlayerSnapshot?
    var selectedEntityIDs: [UInt32] = []
    var fog: TabletopFogSnapshot?
    var unitLayouts: [TabletopUnitLayoutSnapshot] = []
    var actionLayout = TabletopActionLayoutSnapshot()
    var configStrings: [UInt32: String] = [:]
    var entitiesOverflowCount: UInt32 = 0
    var duplicateEntityCount: UInt32 = 0
    var warcraftAssets: WarcraftProductionAssets?
}

protocol TabletopSnapshotTransport: Sendable {
    func start() async throws
    func poll() async throws -> TabletopSnapshot?
    func stop() async
}

extension TabletopSnapshotTransport {
    func start() async throws {}
    func stop() async {}
}

struct TabletopGenerationDeduplicator {
    private(set) var generation: UInt64?

    mutating func accept(_ snapshot: TabletopSnapshot) -> TabletopSnapshot? {
        guard generation != snapshot.generation else { return nil }
        generation = snapshot.generation
        return snapshot
    }
}
