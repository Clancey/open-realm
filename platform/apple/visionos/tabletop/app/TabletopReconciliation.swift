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
    var cellSize: Float
    var terrain: [TabletopRenderTile]
    var entities: [TabletopRenderEntity]

    static let empty = TabletopRenderSnapshot(generation: 0, cellSize: 0.18, terrain: [], entities: [])
}

enum TabletopSnapshotConverter {
    static let layout = TabletopBoardLayout(columns: FixtureSnapshotSource.side, rows: FixtureSnapshotSource.side,
                                             cellSize: 0.18)

    static func convert(_ snapshot: TabletopSnapshot) -> TabletopRenderSnapshot {
        let terrain = snapshot.terrain.map {
            let position = TabletopPlacement.worldPosition(
                TabletopVector3(x: Float($0.x), y: $0.elevation, z: Float($0.z)), in: layout)
            return TabletopRenderTile(id: $0.id, kind: $0.kind, position: position)
        }
        let entities = snapshot.entities.map {
            TabletopRenderEntity(id: $0.id, kind: $0.kind,
                                 position: TabletopPlacement.worldPosition($0.position, in: layout),
                                 heading: $0.heading, selected: $0.selected)
        }
        return TabletopRenderSnapshot(generation: snapshot.generation, cellSize: layout.cellSize,
                                      terrain: terrain, entities: entities)
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
        let nextTiles = Dictionary(uniqueKeysWithValues: snapshot.terrain.map { ($0.id, $0) })
        let nextEntities = Dictionary(uniqueKeysWithValues: snapshot.entities.map { ($0.id, $0) })
        let removedTileIDs = tiles.keys.filter { nextTiles[$0] == nil }.sorted()
        let removedEntityIDs = entities.keys.filter { nextEntities[$0] == nil }.sorted()
        let upsertedTiles = snapshot.terrain.filter { tiles[$0.id] != $0 }
        let upsertedEntities = snapshot.entities.filter { entities[$0.id] != $0 }
        tiles = nextTiles
        entities = nextEntities
        return TabletopReconciliationPlan(removedTileIDs: removedTileIDs, upsertedTiles: upsertedTiles,
                                           removedEntityIDs: removedEntityIDs,
                                           upsertedEntities: upsertedEntities)
    }
}
