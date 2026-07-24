struct WarcraftEntityUpdate: Equatable {
    var entity: WarcraftRenderEntityDescriptor
    var geometryChanged: Bool
    var materialChanged: Bool
    var transformChanged: Bool
    var scaleChanged: Bool
    var stateChanged: Bool
}

struct WarcraftRenderReconciliationPlan: Equatable {
    var removedChunkIDs: [WarcraftTerrainChunkID]
    var upsertedChunks: [WarcraftTerrainChunkDescriptor]
    var fog: WarcraftFogRenderDescriptor?
    var fogRemoved: Bool
    var removedEntityIDs: [UInt64]
    var entityUpdates: [WarcraftEntityUpdate]

    static let empty = WarcraftRenderReconciliationPlan(
        removedChunkIDs: [], upsertedChunks: [], fog: nil, fogRemoved: false,
        removedEntityIDs: [], entityUpdates: [])
}

struct WarcraftRenderSceneState {
    private var generation: UInt64?
    private var chunks: [WarcraftTerrainChunkID: WarcraftTerrainChunkDescriptor] = [:]
    private var fog: WarcraftFogRenderDescriptor?
    private var entities: [UInt64: WarcraftRenderEntityDescriptor] = [:]

    mutating func reset() {
        generation = nil
        chunks.removeAll()
        fog = nil
        entities.removeAll()
    }

    mutating func reconcile(_ snapshot: WarcraftRenderSnapshot) -> WarcraftRenderReconciliationPlan {
        if generation == snapshot.generation { return .empty }
        generation = snapshot.generation
        let nextChunks = Dictionary(uniqueKeysWithValues: snapshot.terrainChunks.map { ($0.id, $0) })
        let nextEntities = Dictionary(snapshot.entities.map { ($0.descriptor.id, $0) },
                                      uniquingKeysWith: { _, replacement in replacement })
        let removedChunks = chunks.keys.filter { nextChunks[$0] == nil }.sorted()
        let upsertedChunks = nextChunks.values.filter { chunks[$0.id]?.contentKey != $0.contentKey }
            .sorted { $0.id < $1.id }
        let fogChanged = fog?.contentKey != snapshot.fog?.contentKey
        let removedEntities = entities.keys.filter { nextEntities[$0] == nil }.sorted()
        let updates = nextEntities.values.compactMap { next -> WarcraftEntityUpdate? in
            guard let old = entities[next.descriptor.id] else {
                return WarcraftEntityUpdate(entity: next, geometryChanged: true, materialChanged: true,
                                            transformChanged: true, scaleChanged: true, stateChanged: true)
            }
            let geometry = old.geometryKey != next.geometryKey
            let material = old.materialKey != next.materialKey ||
                old.teamColor != next.teamColor || old.descriptor.teamTint != next.descriptor.teamTint
            let scale = old.scale != next.scale
            let transform = old.position != next.position || old.scale != next.scale ||
                old.overlayScale != next.overlayScale ||
                old.descriptor.heading != next.descriptor.heading
            let state = old.stateKey != next.stateKey
            guard geometry || material || transform || state else { return nil }
            return WarcraftEntityUpdate(entity: next, geometryChanged: geometry,
                                        materialChanged: material, transformChanged: transform, scaleChanged: scale,
                                        stateChanged: state)
        }.sorted { $0.entity.descriptor.id < $1.entity.descriptor.id }
        chunks = nextChunks
        fog = snapshot.fog
        entities = nextEntities
        return WarcraftRenderReconciliationPlan(
            removedChunkIDs: removedChunks, upsertedChunks: upsertedChunks,
            fog: fogChanged ? snapshot.fog : nil, fogRemoved: fogChanged && snapshot.fog == nil,
            removedEntityIDs: removedEntities, entityUpdates: updates)
    }
}
