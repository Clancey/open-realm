import RealityKit
import SwiftUI

@MainActor
final class RealityTabletopReconciler {
    let root = Entity()
    private var sceneState = TabletopSceneState()
    private var tiles: [Int: ModelEntity] = [:]
    private var entities: [UInt64: ModelEntity] = [:]
    private var latestEntities: [UInt64: TabletopRenderEntity] = [:]
    private var positionOverrides: [UInt64: SIMD3<Float>] = [:]
    private var selectionOverrides: [UInt64: Bool] = [:]
    private var generation: UInt64?

    init() {
        root.name = "open-realm-fixture-board"
        root.position = [0, 1.0, -1.1]
    }

    func apply(_ snapshot: TabletopRenderSnapshot) {
        var expiredOverrides = Set<UInt64>()
        if snapshot.authoritative, generation != nil, generation != snapshot.generation {
            expiredOverrides.formUnion(positionOverrides.keys)
            expiredOverrides.formUnion(selectionOverrides.keys)
            positionOverrides.removeAll()
            selectionOverrides.removeAll()
        }
        generation = snapshot.generation
        latestEntities.removeAll(keepingCapacity: true)
        for entity in snapshot.entities { latestEntities[entity.id] = entity }
        let plan = sceneState.reconcile(snapshot)
        for id in plan.removedTileIDs { tiles.removeValue(forKey: id)?.removeFromParent() }
        for id in plan.removedEntityIDs {
            entities.removeValue(forKey: id)?.removeFromParent()
            positionOverrides.removeValue(forKey: id)
            selectionOverrides.removeValue(forKey: id)
        }
        for tile in plan.upsertedTiles { upsert(tile, cellSize: snapshot.cellSize) }
        for entity in plan.upsertedEntities { upsert(entity, cellSize: snapshot.cellSize) }
        for id in expiredOverrides {
            if let entity = latestEntities[id] { upsert(entity, cellSize: snapshot.cellSize) }
        }
    }

    func fixtureID(for entity: Entity) -> UInt64? {
        entities.first { $0.value === entity }.map(\.key)
    }

    func setSelected(_ id: UInt64) {
        guard entities[id] != nil else { return }
        selectionOverrides = Dictionary(uniqueKeysWithValues: entities.keys.map { ($0, $0 == id) })
        for (entityID, entity) in entities {
            entity.scale = entityID == id ? SIMD3(repeating: 1.18) : .one
        }
    }

    func drag(_ id: UInt64, to position: SIMD3<Float>) {
        guard let entity = entities[id] else { return }
        entity.position = [position.x, entity.position.y, position.z]
        positionOverrides[id] = entity.position
    }

    private func upsert(_ tile: TabletopRenderTile, cellSize: Float) {
        let height: Float = 0.025
        let entity = tiles[tile.id] ?? ModelEntity()
        entity.name = "fixture-terrain-\(tile.id)"
        entity.model = ModelComponent(mesh: .generateBox(size: [cellSize * 0.94, height, cellSize * 0.94]),
                                      materials: [material(for: tile.kind)])
        entity.position = [tile.position.x, tile.position.y - height * 0.5, tile.position.z]
        if entity.parent == nil { root.addChild(entity) }
        tiles[tile.id] = entity
    }

    private func upsert(_ item: TabletopRenderEntity, cellSize: Float) {
        let size: SIMD3<Float>
        switch item.kind {
        case .worker: size = [cellSize * 0.30, cellSize * 0.45, cellSize * 0.30]
        case .soldier: size = [cellSize * 0.34, cellSize * 0.55, cellSize * 0.34]
        case .building: size = [cellSize * 0.72, cellSize * 0.62, cellSize * 0.72]
        case .unit: size = [cellSize * 0.34, cellSize * 0.48, cellSize * 0.34]
        }
        let entity = entities[item.id] ?? ModelEntity()
        entity.name = "fixture-entity-\(item.id)"
        entity.model = ModelComponent(mesh: .generateBox(size: size), materials: [material(for: item.kind)])
        entity.position = positionOverrides[item.id] ??
            SIMD3(item.position.x, item.position.y + size.y * 0.5, item.position.z)
        entity.orientation = simd_quatf(angle: item.heading, axis: [0, 1, 0])
        entity.scale = (selectionOverrides[item.id] ?? item.selected) ? SIMD3(repeating: 1.18) : .one
        entity.components.set(CollisionComponent(shapes: [.generateBox(size: size)]))
        entity.components.set(InputTargetComponent())
        if entity.parent == nil { root.addChild(entity) }
        entities[item.id] = entity
    }

    private func material(for kind: TabletopTerrainKind) -> SimpleMaterial {
        switch kind {
        case .grass: return SimpleMaterial(color: .green, isMetallic: false)
        case .dirt: return SimpleMaterial(color: .brown, isMetallic: false)
        case .water: return SimpleMaterial(color: .blue, isMetallic: true)
        }
    }

    private func material(for kind: TabletopEntityKind) -> SimpleMaterial {
        switch kind {
        case .worker: return SimpleMaterial(color: .orange, isMetallic: false)
        case .soldier: return SimpleMaterial(color: .cyan, isMetallic: true)
        case .building: return SimpleMaterial(color: .gray, isMetallic: false)
        case .unit: return SimpleMaterial(color: .white, isMetallic: false)
        }
    }
}

struct TabletopImmersiveView: View {
    @ObservedObject var model: TabletopSessionModel
    @State private var reconciler = RealityTabletopReconciler()
    @State private var dragState = TabletopDragState()
    @GestureState private var dragActive = false

    var body: some View {
        RealityView { content in
            content.add(reconciler.root)
            reconciler.apply(model.renderSnapshot)
        } update: { _ in
            reconciler.apply(model.renderSnapshot)
        }
        .simultaneousGesture(SpatialTapGesture().targetedToAnyEntity().onEnded { value in
            if let id = reconciler.fixtureID(for: value.entity) {
                reconciler.setSelected(id)
                model.select(entityID: id)
            }
        })
        .simultaneousGesture(DragGesture(minimumDistance: 8).targetedToAnyEntity()
            .updating($dragActive) { _, active, _ in active = true }
            .onChanged { value in
                guard let id = reconciler.fixtureID(for: value.entity) else { return }
                if dragState.entityID == nil { _ = dragState.begin(entityID: id) }
                guard dragState.change(entityID: id) else { return }
                reconciler.drag(id, to: value.convert(value.location3D, from: .local, to: reconciler.root))
            }
            .onEnded { value in
                guard let id = reconciler.fixtureID(for: value.entity), dragState.end(entityID: id) else { return }
                reconciler.drag(id, to: value.convert(value.location3D, from: .local, to: reconciler.root))
            })
        .onChange(of: dragActive) { _, active in
            guard !active, let id = dragState.entityID else { return }
            _ = dragState.end(entityID: id, cancelled: true)
        }
        .task { await model.start() }
        .onDisappear { model.stop() }
        .overlay(alignment: .top) {
            if let error = model.errorMessage {
                Text(error).padding().glassBackgroundEffect()
            }
        }
    }
}
