import RealityKit
import SwiftUI
import UIKit

@MainActor
final class RealityTabletopReconciler {
    let root = Entity()
    private var sceneState = WarcraftRenderSceneState()
    private var chunks: [WarcraftTerrainChunkID: Entity] = [:]
    private var fog: ModelEntity?
    private var entities: [UInt64: Entity] = [:]
    private var latestEntities: [UInt64: TabletopRenderEntity] = [:]
    private var positionOverrides: [UInt64: SIMD3<Float>] = [:]
    private var selectionOverrides: [UInt64: Bool] = [:]
    private var meshCache: [String: MeshResource] = [:]
    private var meshOrder: [String] = []
    private var textureCache: [String: TextureResource] = [:]
    private var textureOrder: [String] = []
    private var diskCache: WarcraftDiskCache?
    private var logOnce = WarcraftLogOnce()
    private var generation: UInt64?
    private var sessionID: UInt64?
    private var opaqueProgram: UnlitMaterial.Program?
    private var alphaProgram: UnlitMaterial.Program?
    private var additiveProgram: UnlitMaterial.Program?
    private static let resourceLimit = 256
    private static let overlayNames: Set<String> = [
        "selection", "health-background", "health-fill", "mana-background", "mana-fill",
    ]

    init() {
        root.name = "open-realm-warcraft-descriptor-scene"
        root.position = [0, 1.0, -1.1]
        do {
            diskCache = try WarcraftDiskCache(
                applicationSupport: WarcraftCacheRoot.applicationSupport(), byteLimit: 64 * 1_024 * 1_024)
        } catch {
            FileHandle.standardError.write(Data(
                "OpenRealmTabletopRenderer: cache initialization failed: \(error)\n".utf8))
        }
    }

    func prepare() async {
        guard opaqueProgram == nil || alphaProgram == nil || additiveProgram == nil else { return }
        let opaque = UnlitMaterial.Program.Descriptor()
        var alpha = UnlitMaterial.Program.Descriptor()
        alpha.blendMode = .alpha
        var additive = UnlitMaterial.Program.Descriptor()
        additive.blendMode = .add
        opaqueProgram = await UnlitMaterial.Program(descriptor: opaque)
        alphaProgram = await UnlitMaterial.Program(descriptor: alpha)
        additiveProgram = await UnlitMaterial.Program(descriptor: additive)
    }

    func reset() {
        sceneState.reset()
        chunks.removeAll()
        fog = nil
        entities.removeAll()
        latestEntities.removeAll()
        positionOverrides.removeAll()
        selectionOverrides.removeAll()
        generation = nil
        sessionID = nil
        root.children.removeAll()
    }

    func apply(_ snapshot: TabletopRenderSnapshot) {
        if let sessionID, sessionID != snapshot.sessionID { reset() }
        sessionID = snapshot.sessionID
        guard let warcraft = snapshot.warcraft else {
            diagnose("RealityKit renderer received no descriptor snapshot; scene remains empty.")
            return
        }
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
        for diagnostic in warcraft.diagnostics { diagnose(diagnostic) }
        let plan = sceneState.reconcile(warcraft)
        for id in plan.removedChunkIDs { chunks.removeValue(forKey: id)?.removeFromParent() }
        for id in plan.removedEntityIDs {
            entities.removeValue(forKey: id)?.removeFromParent()
            positionOverrides.removeValue(forKey: id)
            selectionOverrides.removeValue(forKey: id)
        }
        for chunk in plan.upsertedChunks { upsert(chunk) }
        if plan.fogRemoved { fog?.removeFromParent(); fog = nil }
        if let fog = plan.fog { upsert(fog) }
        for update in plan.entityUpdates { upsert(update) }
        for id in expiredOverrides {
            if let entity = warcraft.entities.first(where: { $0.descriptor.id == id }) {
                upsert(WarcraftEntityUpdate(entity: entity, geometryChanged: false,
                                            materialChanged: false, transformChanged: true,
                                            scaleChanged: false, stateChanged: true))
            }
        }
    }

    func fixtureID(for entity: Entity) -> UInt64? {
        var candidate: Entity? = entity
        while let current = candidate {
            if let id = entities.first(where: { $0.value === current })?.key { return id }
            candidate = current.parent
        }
        return nil
    }

    func setSelected(_ id: UInt64) {
        guard entities[id] != nil else { return }
        selectionOverrides = Dictionary(uniqueKeysWithValues: entities.keys.map { ($0, $0 == id) })
        for (entityID, entity) in entities {
            entity.findEntity(named: "selection")?.isEnabled = entityID == id
        }
    }

    func drag(_ id: UInt64, to position: SIMD3<Float>) {
        guard let entity = entities[id] else { return }
        entity.position = [position.x, entity.position.y, position.z]
        positionOverrides[id] = entity.position
    }

    private func upsert(_ chunk: WarcraftTerrainChunkDescriptor) {
        let container = chunks[chunk.id] ?? Entity()
        container.name = "terrain-\(chunk.id.x)-\(chunk.id.z)"
        container.children.removeAll()
        do { try addModel(chunk.mesh, key: chunk.contentKey, teamColor: .white, tint: .white, to: container) }
        catch { diagnose("Terrain chunk \(chunk.id.x),\(chunk.id.z) failed: \(error)") }
        if container.parent == nil { root.addChild(container) }
        chunks[chunk.id] = container
    }

    private func upsert(_ item: WarcraftFogRenderDescriptor) {
        do {
            let entity = fog ?? ModelEntity()
            entity.name = "fog-texture"
            let part = WarcraftFogBuilder.mesh(item)
            let fogMaterial = WarcraftMaterialDescriptor(
                name: "fog", color: .white, texture: item.image,
                blendMode: .alpha, role: .fog, unlit: true)
            entity.model = ModelComponent(mesh: try mesh(part, key: "fog-\(item.contentKey)"),
                                          materials: [try material(fogMaterial, teamColor: .white, tint: .white)])
            if entity.parent == nil { root.addChild(entity) }
            fog = entity
        } catch { diagnose("Fog texture failed: \(error)") }
    }

    private func upsert(_ update: WarcraftEntityUpdate) {
        let item = update.entity, id = item.descriptor.id
        let entity = entities[id] ?? Entity()
        entity.name = "object-\(id)"
        let rebuilt = update.geometryChanged || update.materialChanged || entities[id] == nil
        if rebuilt {
            entity.children.removeAll()
            do {
                try addModel(item.model, key: item.geometryKey, teamColor: item.teamColor,
                             tint: item.descriptor.teamTint, scale: item.scale, to: entity)
                try addOverlays(item, to: entity)
            } catch {
                diagnose("Object '\(item.descriptor.assetName)' failed: \(error)")
                entity.children.removeAll()
                do {
                    try addModel(WarcraftPlaceholder.model, key: "placeholder",
                                 teamColor: .placeholder, tint: .white, scale: item.scale, to: entity)
                } catch { diagnose("Explicit placeholder construction failed: \(error)") }
            }
        }
        if update.transformChanged || entities[id] == nil {
            entity.position = positionOverrides[id] ?? simd(item.position)
            entity.orientation = simd_quatf(angle: item.descriptor.heading, axis: [0, 1, 0])
            entity.scale = .one
        }
        if rebuilt || update.transformChanged || entities[id] == nil { updateOverlayTransforms(item, in: entity) }
        if update.scaleChanged && !rebuilt {
            for child in entity.children where !Self.overlayNames.contains(child.name) {
                child.scale = simd(item.scale)
            }
        }
        if rebuilt || update.stateChanged {
            entity.findEntity(named: "selection")?.isEnabled =
                selectionOverrides[id] ?? item.descriptor.selected
            updateBar(entity.findEntity(named: "health-background"), value: item.descriptor.health)
            updateBar(entity.findEntity(named: "mana-background"), value: item.descriptor.mana)
            if let animation = item.animation {
                entity.name = "object-\(id)-\(animation.sequence)-\(animation.frame)"
            }
        }
        if update.geometryChanged || update.scaleChanged || entities[id] == nil,
           let bounds = WarcraftMeshMath.bounds(item.model) {
            let size = SIMD3(
                max(bounds.size.x * item.scale.x, 0.01), max(bounds.size.y * item.scale.y, 0.01),
                max(bounds.size.z * item.scale.z, 0.01))
            let center = SIMD3(
                bounds.center.x * item.scale.x, bounds.center.y * item.scale.y,
                bounds.center.z * item.scale.z)
            entity.components.set(CollisionComponent(shapes: [
                ShapeResource.generateBox(size: size).offsetBy(translation: center),
            ]))
        }
        entity.components.set(InputTargetComponent())
        if entity.parent == nil { root.addChild(entity) }
        entities[id] = entity
    }

    private func addModel(_ model: WarcraftModelDescriptor, key: String,
                          teamColor: WarcraftColor, tint: WarcraftColor,
                          scale: WarcraftVector3 = WarcraftVector3(x: 1, y: 1, z: 1),
                          to parent: Entity) throws {
        for (index, part) in model.geosets.enumerated() {
            guard model.materials.indices.contains(part.materialIndex) else {
                throw WarcraftDescriptorError.invalidMesh("geoset material index is out of bounds")
            }
            let descriptor = model.materials[part.materialIndex]
            let mapped = try WarcraftAtlasMapper.map(part, region: descriptor.atlasRegion)
            let atlasKey = descriptor.atlasRegion.map {
                "-atlas-\($0.x)-\($0.y)-\($0.width)-\($0.height)-\($0.atlasWidth)-\($0.atlasHeight)"
            } ?? ""
            let child = ModelEntity(
                mesh: try mesh(mapped, key: "\(key)-\(index)\(atlasKey)"),
                materials: [try material(descriptor, teamColor: teamColor, tint: tint)])
            child.name = part.name
            child.scale = simd(scale)
            parent.addChild(child)
        }
    }

    private func addOverlays(_ item: WarcraftRenderEntityDescriptor, to parent: Entity) throws {
        let ringPart = try WarcraftOverlayMesh.selectionRing()
        let selection = ModelEntity(
            mesh: try mesh(ringPart, key: "selection-ring"),
            materials: [SimpleMaterial(color: .yellow, isMetallic: false)])
        selection.name = "selection"
        selection.isEnabled = item.descriptor.selected
        parent.addChild(selection)
        addBar(name: "health", color: .green, y: 1.12, to: parent)
        addBar(name: "mana", color: .blue, y: 1.02, to: parent)
    }

    private func updateOverlayTransforms(_ item: WarcraftRenderEntityDescriptor, in parent: Entity) {
        if let selection = parent.findEntity(named: "selection") {
            selection.position.y = 0.001
            selection.scale = [item.overlayScale.x, 1, item.overlayScale.z]
        }
        let top = WarcraftMeshMath.bounds(item.model).map {
            ($0.center.y + $0.size.y * 0.5) * item.scale.y
        } ?? item.overlayScale.y
        for (name, offset) in [("health-background", Float(0.008)), ("mana-background", Float(0.004))] {
            guard let bar = parent.findEntity(named: name) else { continue }
            bar.position = [0, top + offset, 0]
            bar.scale.x = max(item.overlayScale.x / 0.8, 0.01)
        }
    }

    private func addBar(name: String, color: UIColor, y: Float, to parent: Entity) {
        let background = ModelEntity(mesh: .generateBox(size: [0.8, 0.06, 0.04]),
                                     materials: [SimpleMaterial(color: .black, isMetallic: false)])
        background.name = "\(name)-background"
        background.position = [0, y, 0]
        let fill = ModelEntity(mesh: .generateBox(size: [0.76, 0.04, 0.045]),
                               materials: [SimpleMaterial(color: color, isMetallic: false)])
        fill.name = "\(name)-fill"
        background.addChild(fill)
        parent.addChild(background)
    }

    private func updateBar(_ entity: Entity?, value: Float?) {
        guard let entity else { return }
        let state = WarcraftOverlayReducer.bar(value)
        entity.isEnabled = state.enabled
        guard let fill = entity.children.first else { return }
        fill.scale.x = state.scale
        fill.position.x = state.offset
    }

    private func mesh(_ part: WarcraftMeshPartDescriptor, key: String) throws -> MeshResource {
        if let cached = meshCache[key] { return cached }
        guard part.positions.count == part.normals.count,
              part.positions.count == part.textureCoordinates.count,
              part.indices.allSatisfy({ Int($0) < part.positions.count }) else {
            throw WarcraftDescriptorError.invalidMesh("mesh buffers have inconsistent counts")
        }
        var descriptor = MeshDescriptor(name: part.name)
        descriptor.positions = MeshBuffers.Positions(part.positions.map(simd))
        descriptor.normals = MeshBuffers.Normals(part.normals.map(simd))
        descriptor.textureCoordinates = MeshBuffers.TextureCoordinates(
            part.textureCoordinates.map { SIMD2($0.x, $0.y) })
        descriptor.primitives = .triangles(part.indices)
        let resource = try MeshResource.generate(from: [descriptor])
        cache(resource, key: key, values: &meshCache, order: &meshOrder)
        return resource
    }

    private func material(_ descriptor: WarcraftMaterialDescriptor,
                          teamColor: WarcraftColor, tint: WarcraftColor) throws -> any RealityKit.Material {
        let roleColor = descriptor.role == .teamColor || descriptor.role == .teamGlow ?
            teamColor : descriptor.color
        let color = multiply(roleColor, tint)
        let texture = try descriptor.texture.map(texture)
        let baseColor = PhysicallyBasedMaterial.BaseColor(
            tint: uiColor(color), texture: texture.map(PhysicallyBasedMaterial.Texture.init))
        switch WarcraftMaterialMapping.kind(descriptor) {
        case .litOpaque, .litModulate:
            var result = SimpleMaterial(color: uiColor(color), roughness: 0.82, isMetallic: false)
            result.color = baseColor
            return result
        case .litAlpha:
            var result = PhysicallyBasedMaterial()
            result.baseColor = baseColor
            result.roughness = 0.82
            result.blending = .transparent(opacity: .init(scale: color.alpha))
            result.writesDepth = false
            return result
        case .unlitOpaque, .unlitAlpha, .unlitAdditive:
            let program = switch WarcraftMaterialMapping.kind(descriptor) {
            case .unlitOpaque: opaqueProgram
            case .unlitAdditive: additiveProgram
            default: alphaProgram
            }
            guard let program else {
                throw WarcraftDescriptorError.invalidMesh("unlit material program was not prepared")
            }
            var result = UnlitMaterial(program: program)
            result.color = baseColor
            result.writesDepth = WarcraftMaterialMapping.kind(descriptor) == .unlitOpaque
            return result
        }
    }

    private func texture(_ image: WarcraftImageDescriptor) throws -> TextureResource {
        let normalized = try WarcraftImageNormalizer.topLeft(image)
        let data = Data(normalized.rgba8), key = WarcraftDescriptorContentKey.image(normalized)
        if let cached = textureCache[key] { return cached }
        let cacheKey = try WarcraftCacheKey.content(namespace: "texture", data: Data(key.utf8))
        let bytes: Data
        if let cached = try diskCache?.value(for: cacheKey, fingerprint: key) { bytes = cached }
        else {
            bytes = data
            try diskCache?.insert(data, for: cacheKey, fingerprint: key)
        }
        guard let provider = CGDataProvider(data: bytes as CFData),
              let cgImage = CGImage(
                width: normalized.width, height: normalized.height, bitsPerComponent: 8,
                bitsPerPixel: 32, bytesPerRow: normalized.width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                provider: provider, decode: nil, shouldInterpolate: false,
                intent: .defaultIntent) else {
            throw WarcraftDescriptorError.invalidImage("could not construct normalized texture")
        }
        let texture = try TextureResource(image: cgImage, options: .init(semantic: .color))
        cache(texture, key: key, values: &textureCache, order: &textureOrder)
        return texture
    }

    private func cache<T>(_ value: T, key: String, values: inout [String: T], order: inout [String]) {
        values[key] = value
        order.removeAll { $0 == key }
        order.append(key)
        while order.count > Self.resourceLimit { values.removeValue(forKey: order.removeFirst()) }
    }

    private func multiply(_ a: WarcraftColor, _ b: WarcraftColor) -> WarcraftColor {
        WarcraftColor(red: a.red * b.red, green: a.green * b.green,
                      blue: a.blue * b.blue, alpha: a.alpha * b.alpha)
    }

    private func uiColor(_ color: WarcraftColor) -> UIColor {
        UIColor(red: CGFloat(color.red), green: CGFloat(color.green),
                blue: CGFloat(color.blue), alpha: CGFloat(color.alpha))
    }

    private func simd(_ value: WarcraftVector3) -> SIMD3<Float> { [value.x, value.y, value.z] }

    private func diagnose(_ message: String) {
        guard logOnce.record(message) else { return }
        FileHandle.standardError.write(Data("OpenRealmTabletopRenderer: \(message)\n".utf8))
    }
}

struct TabletopImmersiveView: View {
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @ObservedObject var model: TabletopSessionModel
    @State private var reconciler = RealityTabletopReconciler()
    @State private var dragState = TabletopDragState()
    @GestureState private var dragActive = false

    var body: some View {
        RealityView { content in
            await reconciler.prepare()
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
        .onDisappear {
            reconciler.reset()
            Task { await model.stop() }
        }
        .overlay(alignment: .top) {
            if let error = model.errorMessage {
                VStack {
                    Text(error)
                    Button("Return to Launcher") {
                        Task {
                            await model.stop()
                            await dismissImmersiveSpace()
                            openWindow(id: "launcher")
                        }
                    }
                }
                .padding()
                .glassBackgroundEffect()
            } else if let diagnostic = model.snapshotDiagnostic {
                Text(diagnostic)
                    .padding()
                    .glassBackgroundEffect()
            }
        }
    }
}
