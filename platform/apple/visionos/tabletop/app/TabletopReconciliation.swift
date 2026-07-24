struct TabletopRenderTile: Equatable {
    var id: Int
    var kind: TabletopTerrainKind
    var position: TabletopVector3
}

struct TabletopRenderEntity: Equatable {
    var id: UInt64
    var kind: TabletopEntityKind
    var position: TabletopVector3
    var heading: Float
    var selected: Bool
}

struct TabletopRenderSnapshot: Equatable {
    var generation: UInt64
    var sessionID: UInt64
    var cellSize: Float
    var terrain: [TabletopRenderTile]
    var entities: [TabletopRenderEntity]
    var authoritative = false

    static let empty = TabletopRenderSnapshot(generation: 0, sessionID: 0, cellSize: 0.18, terrain: [], entities: [],
                                               authoritative: false)
}

enum TabletopSnapshotConverter {
    static let layout = TabletopBoardLayout(columns: FixtureSnapshotSource.side, rows: FixtureSnapshotSource.side,
                                             cellSize: 0.18)

    static func convert(_ snapshot: TabletopSnapshot) -> TabletopRenderSnapshot {
        let terrain = snapshot.terrain.map { TabletopRenderTile(
            id: $0.id, kind: $0.kind,
            position: TabletopPlacement.worldPosition(
                TabletopVector3(x: Float($0.x), y: $0.elevation, z: Float($0.z)), in: layout))
        }
        let worldBounds = bounds(for: snapshot)
        let entities = snapshot.entities.map {
            let position: TabletopVector3
            switch snapshot.coordinateSpace {
            case .fixtureBoard: position = TabletopPlacement.worldPosition($0.position, in: layout)
            case .world: position = worldPosition($0.position, bounds: worldBounds)
            }
            return TabletopRenderEntity(id: $0.id, kind: $0.kind,
                                        position: position, heading: $0.heading, selected: $0.selected)
        }
        let authoritative: Bool
        switch snapshot.coordinateSpace {
        case .fixtureBoard: authoritative = false
        case .world: authoritative = true
        }
        return TabletopRenderSnapshot(generation: snapshot.generation, sessionID: snapshot.sessionID,
                                      cellSize: layout.cellSize,
                                      terrain: terrain, entities: entities, authoritative: authoritative)
    }

    private static func bounds(for snapshot: TabletopSnapshot) -> TabletopBounds2? {
        if case .world(let bounds) = snapshot.coordinateSpace, let bounds { return bounds }
        guard case .world = snapshot.coordinateSpace, let first = snapshot.entities.first else { return nil }
        return snapshot.entities.dropFirst().reduce(
            TabletopBounds2(minX: first.position.x, minZ: first.position.z,
                            maxX: first.position.x, maxZ: first.position.z)) {
            TabletopBounds2(minX: min($0.minX, $1.position.x), minZ: min($0.minZ, $1.position.z),
                            maxX: max($0.maxX, $1.position.x), maxZ: max($0.maxZ, $1.position.z))
        }
    }

    private static func worldPosition(_ position: TabletopVector3, bounds: TabletopBounds2?) -> TabletopVector3 {
        guard let bounds else { return TabletopVector3(x: 0, y: 0, z: 0) }
        let width = max(bounds.maxX - bounds.minX, 1), depth = max(bounds.maxZ - bounds.minZ, 1)
        let scale: Float = 1.08 / max(width, depth)
        return TabletopVector3(x: (position.x - (bounds.minX + bounds.maxX) * 0.5) * scale,
                               y: position.y * scale,
                               z: (position.z - (bounds.minZ + bounds.maxZ) * 0.5) * scale)
    }
}

struct TabletopReconciliationPlan: Equatable {
    var removedTileIDs: [Int]
    var upsertedTiles: [TabletopRenderTile]
    var removedEntityIDs: [UInt64]
    var upsertedEntities: [TabletopRenderEntity]
}

struct TabletopSceneState {
    private var tiles: [Int: TabletopRenderTile] = [:]
    private var entities: [UInt64: TabletopRenderEntity] = [:]

    mutating func reconcile(_ snapshot: TabletopRenderSnapshot) -> TabletopReconciliationPlan {
        var nextTiles: [Int: TabletopRenderTile] = [:]
        var nextEntities: [UInt64: TabletopRenderEntity] = [:]
        for tile in snapshot.terrain { nextTiles[tile.id] = tile }
        for entity in snapshot.entities { nextEntities[entity.id] = entity }
        let removedTileIDs = tiles.keys.filter { nextTiles[$0] == nil }.sorted()
        let removedEntityIDs = entities.keys.filter { nextEntities[$0] == nil }.sorted()
        let upsertedTiles = nextTiles.values.filter { tiles[$0.id] != $0 }.sorted { $0.id < $1.id }
        let upsertedEntities = nextEntities.values.filter { entities[$0.id] != $0 }.sorted { $0.id < $1.id }
        tiles = nextTiles
        entities = nextEntities
        return TabletopReconciliationPlan(removedTileIDs: removedTileIDs, upsertedTiles: upsertedTiles,
                                           removedEntityIDs: removedEntityIDs,
                                           upsertedEntities: upsertedEntities)
    }
}
