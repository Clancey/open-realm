import ImageIO
import Metal
import RealityKit
import SwiftUI
import UIKit

struct TabletopEntityHitComponent: Component {
    var id: UInt64
    var generation: UInt64
    var sessionID: UInt64
}

enum TabletopSurfaceKind: UInt8 {
    case board
    case translationHandle
}

struct TabletopSurfaceComponent: Component {
    var kind: TabletopSurfaceKind
}

@MainActor
final class RealityTabletopReconciler {
    let root = Entity()
    private var sceneState = WarcraftRenderSceneState()
    private var chunks: [WarcraftTerrainChunkID: Entity] = [:]
    private var fog: ModelEntity?
    private var entities: [UInt64: Entity] = [:]
    private var latestEntities: [UInt64: TabletopRenderEntity] = [:]
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
    private var waterMaterials: [String: ShaderGraphMaterial] = [:]
    private var waterMaterialOrder: [String] = []
    private var prepared = false
    private static let resourceLimit = 256
    private static let waterDiskByteLimit = 8 * 1_024 * 1_024
    private static let overlayNames: Set<String> = [
        "selection", "health-background", "health-fill", "mana-background", "mana-fill",
    ]
    private static let waterMaterialX = """
        <?xml version="1.0" encoding="UTF-8"?>
        <materialx version="1.38">
          <nodegraph name="OpenRealmWaterGraph">
            <output name="color" type="color3" nodename="rgb" />
            <output name="opacity" type="float" nodename="alpha" />
            <image name="image" type="color4">
              <input name="file" type="filename" value="__WATER_TEXTURE__" />
              <input name="uaddressmode" type="string" value="periodic" />
              <input name="vaddressmode" type="string" value="periodic" />
              <input name="filtertype" type="string" value="linear" />
            </image>
            <geomcolor name="vertexColor" type="color4">
              <input name="index" type="integer" value="0" />
            </geomcolor>
            <multiply name="modulated" type="color4">
              <input name="in1" type="color4" nodename="image" />
              <input name="in2" type="color4" nodename="vertexColor" />
            </multiply>
            <swizzle name="rgb" type="color3">
              <input name="in" type="color4" nodename="modulated" />
              <input name="channels" type="string" value="rgb" />
            </swizzle>
            <swizzle name="alpha" type="float">
              <input name="in" type="color4" nodename="modulated" />
              <input name="channels" type="string" value="a" />
            </swizzle>
          </nodegraph>
          <surface_unlit name="surface" type="surfaceshader">
            <input name="emission" type="float" value="1" />
            <input name="emission_color" type="color3"
                   nodegraph="OpenRealmWaterGraph" output="color" />
            <input name="transmission" type="float" value="0" />
            <input name="opacity" type="float"
                   nodegraph="OpenRealmWaterGraph" output="opacity" />
          </surface_unlit>
          <surfacematerial name="OpenRealmWater" type="material">
            <input name="surfaceshader" type="surfaceshader" nodename="surface" />
          </surfacematerial>
        </materialx>
        """

    private struct ColoredVertex {
        var position: SIMD3<Float>
        var normal: SIMD3<Float>
        var textureCoordinate: SIMD2<Float>
        var color: SIMD4<Float>
    }

    init(cacheScope: String) {
        root.name = "open-realm-warcraft-descriptor-scene"
        root.position = [0, 1.0, -1.1]
        do {
            diskCache = try WarcraftDiskCache(
                applicationSupport: WarcraftCacheRoot.applicationSupport(), byteLimit: 64 * 1_024 * 1_024,
                editionScope: cacheScope)
        } catch {
            FileHandle.standardError.write(Data(
                "OpenRealmTabletopRenderer: cache initialization failed: \(error)\n".utf8))
        }
    }

    func prepare() async {
        guard !prepared else { return }
        prepared = true
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
        generation = nil
        sessionID = nil
        root.children.removeAll()
        applyTabletopTransform(TabletopBoardTransform())
    }

    func apply(_ snapshot: TabletopRenderSnapshot) async {
        if let sessionID, sessionID != snapshot.sessionID { reset() }
        sessionID = snapshot.sessionID
        ensureInteractionSurfaces()
        guard let warcraft = snapshot.warcraft else {
            diagnose("RealityKit renderer received no descriptor snapshot; scene remains empty.")
            return
        }
        do { try await prepareWaterMaterials(warcraft) }
        catch is CancellationError {
            /* A newer snapshot cancels this task while MaterialX imports; only real import failures are diagnostic. */
            return
        }
        catch {
            diagnose("Authoritative water shader preparation failed: \(error)")
            return
        }
        guard !Task.isCancelled else { return }
        generation = snapshot.generation
        latestEntities.removeAll(keepingCapacity: true)
        for entity in snapshot.entities { latestEntities[entity.id] = entity }
        for diagnostic in warcraft.diagnostics { diagnose(diagnostic) }
        let plan = sceneState.reconcile(warcraft)
        for id in plan.removedChunkIDs { chunks.removeValue(forKey: id)?.removeFromParent() }
        for id in plan.removedEntityIDs {
            entities.removeValue(forKey: id)?.removeFromParent()
        }
        for chunk in plan.upsertedChunks { upsert(chunk) }
        if plan.fogRemoved { fog?.removeFromParent(); fog = nil }
        if let fog = plan.fog { upsert(fog) }
        for update in plan.entityUpdates { upsert(update) }
        for (id, entity) in entities {
            entity.components.set(TabletopEntityHitComponent(
                id: id, generation: snapshot.generation, sessionID: snapshot.sessionID))
        }
    }

    func entityHit(for entity: Entity) -> TabletopEntityHit? {
        var candidate: Entity? = entity
        while let current = candidate {
            if let component = current.components[TabletopEntityHitComponent.self] {
                return TabletopEntityHit(
                    entityID: component.id, generation: component.generation, sessionID: component.sessionID)
            }
            candidate = current.parent
        }
        return nil
    }

    func surfaceKind(for entity: Entity) -> TabletopSurfaceKind? {
        var candidate: Entity? = entity
        while let current = candidate {
            if let component = current.components[TabletopSurfaceComponent.self] { return component.kind }
            candidate = current.parent
        }
        return nil
    }

    func applyTabletopTransform(_ value: TabletopBoardTransform) {
        root.position = [value.translation.x, value.translation.y, value.translation.z]
        root.orientation = simd_quatf(angle: value.yaw, axis: [0, 1, 0])
        root.scale = SIMD3(repeating: value.scale)
    }

    private func ensureInteractionSurfaces() {
        guard root.findEntity(named: "board-command-surface") == nil else { return }
        let board = Entity()
        board.name = "board-command-surface"
        board.position.y = -0.025
        board.components.set(InputTargetComponent())
        board.components.set(CollisionComponent(shapes: [.generateBox(size: [1.12, 0.03, 1.12])]))
        board.components.set(TabletopSurfaceComponent(kind: .board))
        root.addChild(board)

        let handle = ModelEntity(
            mesh: .generateBox(size: [0.025, 0.025, 0.34], cornerRadius: 0.008),
            materials: [SimpleMaterial(color: UIColor.systemBlue.withAlphaComponent(0.72), isMetallic: false)])
        handle.name = "board-translation-handle"
        handle.position = [-0.59, 0.02, 0]
        handle.components.set(InputTargetComponent())
        handle.generateCollisionShapes(recursive: false)
        handle.components.set(TabletopSurfaceComponent(kind: .translationHandle))
        root.addChild(handle)
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
            entity.position = simd(item.position)
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
            entity.findEntity(named: "selection")?.isEnabled = item.descriptor.selected
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
                materials: [try material(
                    descriptor, teamColor: teamColor, tint: tint,
                    usesVertexColors: !mapped.vertexColors.isEmpty)])
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
              (part.vertexColors.isEmpty || part.positions.count == part.vertexColors.count),
              part.indices.allSatisfy({ Int($0) < part.positions.count }) else {
            throw WarcraftDescriptorError.invalidMesh("mesh buffers have inconsistent counts")
        }
        if !part.vertexColors.isEmpty {
            let resource = try coloredMesh(part)
            cache(resource, key: key, values: &meshCache, order: &meshOrder)
            return resource
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

    /* Standard RealityKit materials ignore vertex color, so water uses a color-semantic LowLevelMesh. */
    private func coloredMesh(_ part: WarcraftMeshPartDescriptor) throws -> MeshResource {
        let vertices = part.positions.indices.map { index in
            let color = part.vertexColors[index]
            return ColoredVertex(
                position: simd(part.positions[index]), normal: simd(part.normals[index]),
                textureCoordinate: SIMD2(
                    part.textureCoordinates[index].x, part.textureCoordinates[index].y),
                color: SIMD4(color.red, color.green, color.blue, color.alpha))
        }
        let stride = MemoryLayout<ColoredVertex>.stride
        let descriptor = LowLevelMesh.Descriptor(
            vertexCapacity: vertices.count,
            vertexAttributes: [
                .init(semantic: .position, format: .float3,
                      offset: MemoryLayout<ColoredVertex>.offset(of: \.position)!),
                .init(semantic: .normal, format: .float3,
                      offset: MemoryLayout<ColoredVertex>.offset(of: \.normal)!),
                .init(semantic: .uv0, format: .float2,
                      offset: MemoryLayout<ColoredVertex>.offset(of: \.textureCoordinate)!),
                .init(semantic: .color, format: .float4,
                      offset: MemoryLayout<ColoredVertex>.offset(of: \.color)!),
            ],
            vertexLayouts: [.init(bufferIndex: 0, bufferStride: stride)],
            indexCapacity: part.indices.count, indexType: .uint32)
        let mesh = try LowLevelMesh(descriptor: descriptor)
        mesh.replaceUnsafeMutableBytes(bufferIndex: 0) { destination in
            vertices.withUnsafeBytes { destination.copyBytes(from: $0) }
        }
        mesh.replaceUnsafeMutableIndices { destination in
            part.indices.withUnsafeBytes { destination.copyBytes(from: $0) }
        }
        let points = vertices.map(\.position), first = points[0]
        let bounds = points.dropFirst().reduce((min: first, max: first)) {
            (simd_min($0.min, $1), simd_max($0.max, $1))
        }
        mesh.parts.replaceAll([
            .init(indexCount: part.indices.count, topology: .triangle, materialIndex: 0,
                  bounds: BoundingBox(min: bounds.min, max: bounds.max)),
        ])
        return try MeshResource(from: mesh)
    }

    private func material(_ descriptor: WarcraftMaterialDescriptor,
                          teamColor: WarcraftColor, tint: WarcraftColor,
                          usesVertexColors: Bool = false) throws -> any RealityKit.Material {
        let color = multiply(WarcraftMaterialTint.roleColor(descriptor, teamColor: teamColor), tint)
        if usesVertexColors, let image = descriptor.texture {
            let key = WarcraftDescriptorContentKey.image(image)
            guard let result = waterMaterials[key] else {
                throw WarcraftDescriptorError.invalidMesh("authoritative water material was not prepared")
            }
            return result
        }
        let texture = try descriptor.texture.map(texture)
        let baseColor = PhysicallyBasedMaterial.BaseColor(
            tint: uiColor(color), texture: texture.map(PhysicallyBasedMaterial.Texture.init))
        switch WarcraftMaterialMapping.kind(descriptor) {
        case .litOpaque, .litModulate:
            var result = SimpleMaterial(color: uiColor(color), roughness: 0.82, isMetallic: false)
            result.color = baseColor
            result.writesDepth = descriptor.writesDepth
            result.readsDepth = descriptor.readsDepth
            if descriptor.twoSided { result.faceCulling = .none }
            return result
        case .litAlpha:
            var result = PhysicallyBasedMaterial()
            result.baseColor = baseColor
            result.roughness = 0.82
            result.blending = .transparent(opacity: .init(scale: color.alpha))
            result.writesDepth = descriptor.writesDepth
            result.readsDepth = descriptor.readsDepth
            if descriptor.twoSided { result.faceCulling = .none }
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
            result.writesDepth = descriptor.writesDepth
            result.readsDepth = descriptor.readsDepth
            if descriptor.twoSided { result.faceCulling = .none }
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

    /* MaterialX must resolve its image while importing; cache the copied image as an atomic PNG first. */
    private func prepareWaterMaterials(_ snapshot: WarcraftRenderSnapshot) async throws {
        var images: [String: WarcraftImageDescriptor] = [:]
        for chunk in snapshot.terrainChunks {
            for part in chunk.mesh.geosets where !part.vertexColors.isEmpty {
                guard chunk.mesh.materials.indices.contains(part.materialIndex) else {
                    throw WarcraftDescriptorError.invalidMesh(
                        "vertex-colored water geometry has no material")
                }
                /* Fixture water is intentionally textureless; only production water imports MaterialX. */
                guard let image = chunk.mesh.materials[part.materialIndex].texture else { continue }
                images[WarcraftDescriptorContentKey.image(image)] = image
            }
        }
        for key in images.keys.sorted() where waterMaterials[key] == nil {
            guard !Task.isCancelled, let image = images[key] else { return }
            let url = try waterImageURL(image, key: key)
            let escaped = url.path
                .replacingOccurrences(of: "&", with: "&amp;")
                .replacingOccurrences(of: "\"", with: "&quot;")
            let data = Data(Self.waterMaterialX.replacingOccurrences(
                of: "__WATER_TEXTURE__", with: escaped).utf8)
            var material = try await ShaderGraphMaterial(materialXLabel: "OpenRealmWater", data: data)
            material.writesDepth = false
            waterMaterials[key] = material
            waterMaterialOrder.append(key)
            while waterMaterialOrder.count > Self.resourceLimit {
                waterMaterials.removeValue(forKey: waterMaterialOrder.removeFirst())
            }
        }
    }

    private func waterImageURL(_ image: WarcraftImageDescriptor, key: String) throws -> URL {
        let normalized = try WarcraftImageNormalizer.topLeft(image)
        let bytes = Data(normalized.rgba8)
        guard let provider = CGDataProvider(data: bytes as CFData),
              let cgImage = CGImage(
                width: normalized.width, height: normalized.height, bitsPerComponent: 8,
                bitsPerPixel: 32, bytesPerRow: normalized.width * 4,
                space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                provider: provider, decode: nil, shouldInterpolate: false,
                intent: .defaultIntent),
              let png = CFDataCreateMutable(nil, 0),
              let destination = CGImageDestinationCreateWithData(png, "public.png" as CFString, 1, nil)
        else { throw WarcraftDescriptorError.invalidImage("could not encode authoritative water image") }
        CGImageDestinationAddImage(destination, cgImage, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw WarcraftDescriptorError.invalidImage("could not finalize authoritative water image")
        }
        let root = try WarcraftCacheRoot.applicationSupport()
            .appendingPathComponent("OpenRealm/WarcraftRenderer/v\(WarcraftCacheKey.version)/water",
                                    isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let url = root.appendingPathComponent("\(key).png")
        let data = png as Data
        if FileManager.default.fileExists(atPath: url.path) {
            guard try Data(contentsOf: url) == data else {
                throw WarcraftDescriptorError.cache("water image cache collision for \(key)")
            }
        } else {
            try data.write(to: url, options: .atomic)
        }
        try trimWaterImageCache(root, keeping: url)
        return url
    }

    /* MaterialX needs files, so retain the current image while bounding all older content revisions. */
    private func trimWaterImageCache(_ root: URL, keeping current: URL) throws {
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .fileSizeKey, .contentModificationDateKey]
        let files = try FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: Array(keys), options: [.skipsHiddenFiles])
        var entries: [(url: URL, size: Int, date: Date)] = [], total = 0
        for url in files where url.pathExtension == "png" {
            let values = try url.resourceValues(forKeys: keys)
            guard values.isRegularFile == true else { continue }
            let size = values.fileSize ?? 0
            entries.append((url, size, values.contentModificationDate ?? .distantPast))
            total += size
        }
        for entry in entries.sorted(by: { $0.date < $1.date })
            where total > Self.waterDiskByteLimit && entry.url != current {
            try FileManager.default.removeItem(at: entry.url)
            total -= entry.size
        }
        guard total <= Self.waterDiskByteLimit else {
            throw WarcraftDescriptorError.cache("authoritative water image exceeds disk cache limit")
        }
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
    @Environment(\.scenePhase) private var scenePhase
    @ObservedObject var model: TabletopSessionModel
    @State private var reconciler: RealityTabletopReconciler
    @State private var interaction = TabletopInteractionState()
    @State private var board = TabletopBoardManipulationState()
    @State private var boardDragStart: SIMD3<Float>?
    @State private var boardMagnification: Float = 1
    @State private var boardYaw: Float = 0
    @State private var additiveSelection = false
    @GestureState private var boardDragActive = false

    init(model: TabletopSessionModel) {
        self.model = model
        _reconciler = State(initialValue: RealityTabletopReconciler(cacheScope: model.selectedEdition.rawValue))
    }

    var body: some View {
        RealityView { content in
            await reconciler.prepare()
            content.add(reconciler.root)
        }
        .task(id: "\(model.renderSnapshot.sessionID):\(model.renderSnapshot.generation)") {
            await reconciler.apply(model.renderSnapshot)
        }
        .onChange(of: model.renderSnapshot.generation) { _, _ in
            interaction.reconcile(authoritativeTarget: model.renderSnapshot.actionLayout.currentTarget)
        }
        .simultaneousGesture(SpatialTapGesture().targetedToAnyEntity().onEnded { value in
            guard model.acceptsGameplayInput else { return }
            if let hit = reconciler.entityHit(for: value.entity) {
                if model.renderSnapshot.actionLayout.currentTarget.acceptsEntity {
                    model.targetEntity(hit)
                } else if interaction.beginSelection() {
                    model.select(hit, mode: additiveSelection ? .additive : .replacement)
                    additiveSelection = false
                    interaction.finishTransient()
                }
                return
            }
            guard reconciler.surfaceKind(for: value.entity) == .board else { return }
            let local = value.convert(value.location3D, from: .local, to: reconciler.root)
            do {
                let point = try TabletopWorldMapping.enginePoint(
                    TabletopVector3(x: local.x, y: local.y, z: local.z),
                    bounds: model.renderSnapshot.worldBounds)
                if model.renderSnapshot.actionLayout.currentTarget.acceptsPoint {
                    model.targetPoint(x: point.0, y: point.1)
                } else if interaction.beginSmartPoint() {
                    model.smartPoint(x: point.0, y: point.1)
                    interaction.finishTransient()
                }
            } catch {
                FileHandle.standardError.write(Data("OpenRealmTabletopControls: \(error)\n".utf8))
            }
        })
        .simultaneousGesture(LongPressGesture(minimumDuration: 0.45).targetedToAnyEntity().onEnded { value in
            guard model.acceptsGameplayInput, interaction.mode == .idle,
                  let hit = reconciler.entityHit(for: value.entity) else { return }
            model.smartEntity(hit)
        })
        .highPriorityGesture(DragGesture(minimumDistance: 8).targetedToAnyEntity()
            .updating($boardDragActive) { value, active, _ in
                if reconciler.surfaceKind(for: value.entity) == .translationHandle { active = true }
            }
            .onChanged { value in
                guard model.acceptsGameplayInput,
                      reconciler.surfaceKind(for: value.entity) == .translationHandle else { return }
                let point = value.convert(value.location3D, from: .local, to: reconciler.root)
                if boardDragStart == nil {
                    guard interaction.beginBoardManipulation(.leftHand), board.begin(.leftHand) else { return }
                    boardDragStart = point
                }
                guard let start = boardDragStart else { return }
                _ = board.update(.leftHand, translation: TabletopVector3(
                    x: point.x - start.x, y: point.y - start.y, z: point.z - start.z))
                reconciler.applyTabletopTransform(board.transform)
            }
            .onEnded { value in
                guard reconciler.surfaceKind(for: value.entity) == .translationHandle else { return }
                _ = board.end(.leftHand)
                boardDragStart = nil
                interaction.finishTransient()
            })
        .simultaneousGesture(MagnifyGesture(minimumScaleDelta: 0.01).targetedToAnyEntity()
            .onChanged { value in
                guard model.acceptsGameplayInput, reconciler.surfaceKind(for: value.entity) == .board,
                      interaction.beginBoardManipulation(.twoHand), board.begin(.twoHand) else { return }
                boardMagnification = Float(value.magnification)
                _ = board.update(.twoHand, yaw: boardYaw, magnification: boardMagnification)
                reconciler.applyTabletopTransform(board.transform)
            }
            .onEnded { _ in finishTwoHandManipulation() })
        .simultaneousGesture(RotateGesture3D(constrainedToAxis: .y).targetedToAnyEntity()
            .onChanged { value in
                guard model.acceptsGameplayInput, reconciler.surfaceKind(for: value.entity) == .board,
                      interaction.beginBoardManipulation(.twoHand), board.begin(.twoHand) else { return }
                boardYaw = Float(value.rotation.angle.radians)
                _ = board.update(.twoHand, yaw: boardYaw, magnification: boardMagnification)
                reconciler.applyTabletopTransform(board.transform)
            }
            .onEnded { _ in finishTwoHandManipulation() })
        .onChange(of: boardDragActive) { _, active in
            guard !active, board.owner == .leftHand else { return }
            _ = board.cancel(.leftHand)
            boardDragStart = nil
            interaction.cancelGesture()
            reconciler.applyTabletopTransform(board.transform)
        }
        .onDisappear {
            interaction.reset()
            board.reset()
            reconciler.reset()
            Task { await model.returnToMenu() }
        }
        .onChange(of: scenePhase) { _, phase in
            Task {
                switch phase {
                case .active: await model.setPaused(false, reason: .background)
                case .inactive, .background: await model.setPaused(true, reason: .background)
                @unknown default: await model.setPaused(true, reason: .background)
                }
            }
        }
        .overlay(alignment: .bottomTrailing) {
            TabletopActionPanel(
                layout: model.renderSnapshot.actionLayout,
                additiveSelection: $additiveSelection,
                activate: model.activate,
                cancel: {
                    if interaction.requestCancel() { model.cancelTargeting() }
                })
            .padding()
        }
        .overlay(alignment: .top) {
            if let error = model.errorMessage {
                VStack {
                    Text(error)
                    Button("Return to Launcher") {
                        Task { await returnToLauncher() }
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
        .overlay(alignment: .topLeading) {
            if case .playing = model.productState {
                Button("Pause") { Task { await model.setPaused(true, reason: .user) } }
                    .padding()
                    .glassBackgroundEffect()
            }
        }
        .overlay { productOverlay }
    }

    @ViewBuilder private var productOverlay: some View {
        switch model.productState {
        case .paused:
            VStack(spacing: 14) {
                Text("Paused").font(.title)
                Button("Resume") { Task { await model.setPaused(false, reason: .user) } }
                    .buttonStyle(.borderedProminent)
                Text("Graphics, key bindings, and difficulty changes are not supported during a match.")
                    .font(.caption).multilineTextAlignment(.center)
                Button("Return to Menu", role: .destructive) { Task { await returnToLauncher() } }
            }
            .padding(24).frame(maxWidth: 420).glassBackgroundEffect()
        case .terminal(_, let result):
            VStack(spacing: 14) {
                Text(result == .victory ? "Victory" : result == .defeat ? "Defeat" : "Map Complete")
                    .font(.largeTitle)
                Button("Retry") { Task { await model.retry() } }
                if model.nextMap != nil {
                    Button("Next Map") { Task { await model.startNextMap() } }
                        .buttonStyle(.borderedProminent)
                }
                Button("Return to Menu") { Task { await returnToLauncher() } }
            }
            .padding(24).glassBackgroundEffect()
        default:
            EmptyView()
        }
    }

    private func returnToLauncher() async {
        await model.returnToMenu()
        await dismissImmersiveSpace()
        openWindow(id: "launcher")
    }

    private func finishTwoHandManipulation() {
        guard board.owner == .twoHand else { return }
        _ = board.end(.twoHand)
        boardMagnification = 1
        boardYaw = 0
        interaction.finishTransient()
    }
}

private struct TabletopActionPanel: View {
    var layout: TabletopActionLayoutSnapshot
    @Binding var additiveSelection: Bool
    var activate: (TabletopActionButtonSnapshot) -> Void
    var cancel: () -> Void

    private let columns = Array(repeating: GridItem(.fixed(92), spacing: 8), count: 4)

    var body: some View {
        if layout.present && layout.visible && layout.valid {
            VStack(alignment: .trailing, spacing: 8) {
                Toggle("Add selection", isOn: $additiveSelection)
                    .toggleStyle(.button)
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(layout.buttons.filter { !$0.hidden }.sorted {
                        ($0.gridY, $0.gridX) < ($1.gridY, $1.gridX)
                    }) { button in
                        Button {
                            activate(button)
                        } label: {
                            VStack(spacing: 3) {
                                Text(button.tooltip.isEmpty ? button.actionCode : button.tooltip)
                                    .lineLimit(2)
                                if button.cooldown > 0 {
                                    Text(button.cooldown, format: .number.precision(.fractionLength(1)))
                                        .font(.caption2)
                                }
                            }
                            .frame(width: 84, height: 54)
                        }
                        .disabled(button.disabled || button.semantic == .unsupported)
                    }
                }
                if layout.currentTarget != .none {
                    Button("Cancel", role: .cancel, action: cancel)
                }
            }
            .padding()
            .glassBackgroundEffect()
        }
    }
}
