import Foundation

struct WarcraftAssetMetadata: Equatable, Sendable {
    var category: WarcraftEntityCategory
    var classID: UInt32
    var teamColor: UInt8
    var tint: WarcraftColor
    var footprint: WarcraftFootprint
}

enum WarcraftItemPublication {
    static func classIDs(_ metadata: [WarcraftAssetMetadata]) -> [UInt32] {
        Array(Set(metadata.filter { $0.category == .item }.map(\.classID))).sorted()
    }

    static func classList(_ classIDs: [UInt32]) -> String {
        classIDs.map { String(format: "%08x", $0) }.joined(separator: ",")
    }
}

struct WarcraftExportedImage: Equatable, Sendable {
    var identity: String
    var placeholder: Bool
    var status: UInt32
    var width: Int
    var height: Int
    var rowBytes: Int
    var rgba8: [UInt8]
    var orientation: WarcraftImageOrientation
}

struct WarcraftExportedGeoset: Equatable, Sendable {
    var positions: [WarcraftVector3]
    var normals: [WarcraftVector3]
    var textureCoordinates: [WarcraftVector2]
    var indices: [UInt16]
    var materialIndex: Int
}

struct WarcraftExportedBounds: Equatable, Sendable {
    var min: WarcraftVector3
    var max: WarcraftVector3
    var radius: Float
}

struct WarcraftExportedMaterial: Equatable, Sendable {
    var firstLayer: Int
    var layerCount: Int
}

struct WarcraftExportedLayer: Equatable, Sendable {
    var blendMode: UInt32
    var flags: UInt32
    var textureIndex: Int
    var alpha: Float
}

struct WarcraftExportedTexture: Equatable, Sendable {
    var identity: String
    var replaceableID: UInt32
    var image: WarcraftExportedImage?
}

struct WarcraftExportedSequence: Equatable, Sendable {
    var name: String
    var startMilliseconds: UInt32
    var endMilliseconds: UInt32
    var flags: UInt32
}

struct WarcraftExportedNode: Equatable, Sendable {
    var name: String
    var objectID: UInt32
    var parentID: UInt32
    var flags: UInt32
    var pivot: WarcraftVector3
    var initialTranslation: WarcraftVector3
    var initialRotation: (Float, Float, Float, Float)
    var initialScale: WarcraftVector3

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.name == rhs.name && lhs.objectID == rhs.objectID && lhs.parentID == rhs.parentID &&
            lhs.flags == rhs.flags && lhs.pivot == rhs.pivot &&
            lhs.initialTranslation == rhs.initialTranslation &&
            lhs.initialRotation.0 == rhs.initialRotation.0 &&
            lhs.initialRotation.1 == rhs.initialRotation.1 &&
            lhs.initialRotation.2 == rhs.initialRotation.2 &&
            lhs.initialRotation.3 == rhs.initialRotation.3 &&
            lhs.initialScale == rhs.initialScale
    }
}

struct WarcraftExportedModel: Equatable, Sendable {
    var identity: String
    var placeholder: Bool
    var status: UInt32
    var metadataStatus: UInt32
    var version: UInt32
    var metadata: WarcraftAssetMetadata
    var bounds: WarcraftExportedBounds
    var geosets: [WarcraftExportedGeoset]
    var materials: [WarcraftExportedMaterial]
    var layers: [WarcraftExportedLayer]
    var textures: [WarcraftExportedTexture]
    var overrideImage: WarcraftExportedImage? = nil
    var teamColorImage: WarcraftExportedImage? = nil
    var teamGlowImage: WarcraftExportedImage? = nil
    var sequences: [WarcraftExportedSequence]
    var nodes: [WarcraftExportedNode]
}

struct WarcraftExportedTerrainCorner: Equatable, Sendable {
    var height: Float
    var waterHeight: Float
    var groundID: UInt32
    var cliffID: UInt32
    var groundVariation: UInt8
    var cliffVariation: UInt8
    var cliffLevel: UInt8
    var flags: UInt8
}

struct WarcraftExportedTerrainTexture: Equatable, Sendable {
    var typeIndex: Int
    var typeID: UInt32
    var cornerCount: Int
    var image: WarcraftExportedImage
}

struct WarcraftExportedTerrain: Equatable, Sendable {
    var cornerWidth: Int
    var cornerHeight: Int
    var tileWidth: Int
    var tileHeight: Int
    var chunkTiles: Int
    var chunkCountX: Int
    var chunkCountZ: Int
    var bounds: TabletopBounds2
    var groundTypes: [UInt32]
    var cliffTypes: [UInt32]
    var groundTextures: [WarcraftExportedTerrainTexture]
    var cliffTextures: [WarcraftExportedTerrainTexture]
    var waterTexture: WarcraftExportedTerrainTexture? = nil
    var corners: [WarcraftExportedTerrainCorner]
}

struct WarcraftWorldTransform: Equatable, Sendable {
    var centerX: Float
    var centerZ: Float
    var scale: Float

    func point(_ value: TabletopVector3) -> WarcraftVector3 {
        WarcraftVector3(x: (value.x - centerX) * scale, y: value.y * scale,
                        z: (value.z - centerZ) * scale)
    }
}

struct WarcraftProductionEntityAsset: Equatable, Sendable {
    var identity: String
    var metadata: WarcraftAssetMetadata
    var bounds: WarcraftExportedBounds?
    var model: WarcraftModelDescriptor?
    var diagnostics: [String]
}

struct WarcraftAssetCacheCounters: Equatable, Sendable {
    var hits: UInt64
    var misses: UInt64
    var placeholderLogs: UInt64
    var metadataLogs: UInt64 = 0
}

struct WarcraftExportedModelCacheKey: Equatable, Hashable, Sendable {
    var configStringIndex: UInt32
    var identity: String
    var status: UInt32
    var placeholder: Bool
    var overrideIdentity: String = ""
    var overrideContentKey: String = ""
    var teamColorIndex: UInt32 = .max
    var teamColorContentKey: String = ""
    var teamGlowIndex: UInt32 = .max
    var teamGlowContentKey: String = ""
}

enum WarcraftTeamTextureRequest: Equatable {
    case absent
    case valid(UInt64)
    case invalid

    static func resolve(kind: UInt32, teamColor: UInt32, count: UInt32,
                        required: Bool) -> WarcraftTeamTextureRequest {
        guard required else { return .absent }
        guard (kind == 1 || kind == 2), count > 0, teamColor < count else { return .invalid }
        return .valid(UInt64(kind) << 32 | UInt64(teamColor))
    }
}

struct WarcraftExportedAssetCache {
    private struct Entry {
        var model: WarcraftExportedModel
        var adapted: WarcraftProductionEntityAsset?
        var tick: UInt64
    }
    let modelLimit: Int
    private var models: [WarcraftExportedModelCacheKey: Entry] = [:]
    private var terrain: (key: String, value: WarcraftExportedTerrain)?
    private var tick: UInt64 = 0
    private(set) var counters = WarcraftCacheCounters()

    init(modelLimit: Int) { self.modelLimit = max(modelLimit, 0) }

    mutating func model(for key: WarcraftExportedModelCacheKey) -> WarcraftExportedModel? {
        tick &+= 1
        guard var entry = models[key] else { counters.misses += 1; return nil }
        entry.tick = tick
        models[key] = entry
        counters.hits += 1
        return entry.model
    }

    mutating func insert(_ model: WarcraftExportedModel, for key: WarcraftExportedModelCacheKey) {
        tick &+= 1
        models[key] = Entry(model: model, adapted: nil, tick: tick)
        while models.count > modelLimit, let victim = models.min(by: { $0.value.tick < $1.value.tick }) {
            models.removeValue(forKey: victim.key)
            counters.evictions += 1
        }
    }

    mutating func adaptedModel(for key: WarcraftExportedModelCacheKey) -> WarcraftProductionEntityAsset? {
        models[key]?.adapted
    }

    mutating func insertAdapted(_ adapted: WarcraftProductionEntityAsset,
                                for key: WarcraftExportedModelCacheKey) {
        guard var entry = models[key] else { return }
        entry.adapted = adapted
        models[key] = entry
    }

    mutating func terrain(for key: String) -> WarcraftExportedTerrain? {
        guard terrain?.key == key else { counters.misses += 1; return nil }
        counters.hits += 1
        return terrain?.value
    }

    mutating func insert(_ value: WarcraftExportedTerrain, terrainKey: String) {
        terrain = (terrainKey, value)
    }

    mutating func reset() {
        models.removeAll()
        terrain = nil
        tick = 0
        counters = WarcraftCacheCounters()
    }
}

struct WarcraftProductionAssets: Equatable, Sendable {
    var abiVersion: UInt32
    var terrainKey: String?
    var terrain: WarcraftTerrainDescriptor?
    var worldTransform: WarcraftWorldTransform?
    var entities: [UInt64: WarcraftProductionEntityAsset]
    var counters: WarcraftAssetCacheCounters
    var terrainTextureCount: Int
    var terrainNoCliffCount: Int
    var diagnostics: [String]

    var placeholderModelCount: Int { entities.values.filter { $0.model == nil }.count }
    var placeholderMaterialCount: Int {
        let entityMaterials = entities.values.compactMap(\.model).flatMap(\.materials)
        return entityMaterials.filter { $0.role == .placeholder }.count +
            (terrain?.materials.filter { $0.role == .placeholder }.count ?? 0)
    }
    var placeholderCount: Int { placeholderModelCount + placeholderMaterialCount }
}

struct WarcraftEntityAssetSignature: Equatable, Sendable {
    var id: UInt64
    var classID: UInt32
    var model: UInt32
    var modelIdentity: String
    var image: UInt32
    var imageIdentity: String
    var player: UInt32
}

struct WarcraftProductionAssetSignature: Equatable, Sendable {
    var mapName: String?
    var terrainKey: String?
    var entities: [WarcraftEntityAssetSignature]
}

/* Snapshot generations change every frame, but immutable descriptors change only with these identities. */
struct WarcraftProductionAssetCache {
    private var entry: (signature: WarcraftProductionAssetSignature, assets: WarcraftProductionAssets)?
    private(set) var hits: UInt64 = 0

    mutating func assets(for signature: WarcraftProductionAssetSignature) -> WarcraftProductionAssets? {
        guard let entry, entry.signature == signature else { return nil }
        hits &+= 1
        return presented(entry.assets)
    }

    @discardableResult
    mutating func insert(_ assets: WarcraftProductionAssets, for signature: WarcraftProductionAssetSignature)
        -> WarcraftProductionAssets {
        entry = (signature, assets)
        return presented(assets)
    }

    mutating func reset() { entry = nil; hits = 0 }

    private func presented(_ assets: WarcraftProductionAssets) -> WarcraftProductionAssets {
        var value = assets
        value.counters.hits &+= hits
        return value
    }
}

private struct WarcraftAdaptedTerrain {
    var descriptor: WarcraftTerrainDescriptor
    var transform: WarcraftWorldTransform
    var textureCount: Int
    var noCliffCount: Int
    var diagnostics: [String]
}

enum WarcraftAssetDescriptorAdapter {
    static let mdxVersion: UInt32 = 800
    static let terrainChunkTiles = 32
    private static let layerUnshaded: UInt32 = 0x1
    private static let layerTwoSided: UInt32 = 0x10
    private static let layerUnfogged: UInt32 = 0x20
    private static let layerNoDepthTest: UInt32 = 0x40
    private static let layerNoDepthSet: UInt32 = 0x80
    private static let terrainMapEdge: UInt8 = 1 << 0
    private static let terrainRamp: UInt8 = 1 << 1
    private static let terrainWater: UInt8 = 1 << 3
    private static let terrainNoCliff: UInt8 = 1 << 5

    static func image(_ source: WarcraftExportedImage) throws -> WarcraftImageDescriptor {
        guard source.width > 0, source.height > 0, source.rowBytes == source.width * 4,
              source.rgba8.count == source.rowBytes * source.height else {
            throw WarcraftDescriptorError.invalidImage(
                "asset '\(source.identity)' has inconsistent RGBA8 dimensions")
        }
        return try WarcraftImageNormalizer.topLeft(WarcraftImageDescriptor(
            width: source.width, height: source.height, rgba8: source.rgba8,
            orientation: source.orientation))
    }

    /* Image identity and copied pixels both participate so shared MDX geometry cannot alias materials. */
    static func imageContentKey(_ source: WarcraftExportedImage?) throws -> String {
        guard let source else { return "" }
        var hash = WarcraftContentHasher()
        hash.update(source.identity); hash.update(source.status); hash.update(source.placeholder)
        hash.update(WarcraftDescriptorContentKey.image(try image(source)))
        return hash.digest()
    }

    /* Grayscale destination multiply equals black alpha composition with alpha = 1 - luminance. */
    private static func modulateImage(_ source: WarcraftExportedImage, doubled: Bool)
        throws -> WarcraftImageDescriptor? {
        var output = try image(source)
        for index in stride(from: 0, to: output.rgba8.count, by: 4) {
            let channels = output.rgba8[index..<(index + 3)]
            guard Int(channels.max()!) - Int(channels.min()!) <= 8 else { return nil }
            let luminance = (Int(output.rgba8[index]) + Int(output.rgba8[index + 1]) +
                             Int(output.rgba8[index + 2]) + 1) / 3
            let factor = min(luminance * (doubled ? 2 : 1), 255)
            let alpha = (255 - factor) * Int(output.rgba8[index + 3]) / 255
            output.rgba8[index] = 0; output.rgba8[index + 1] = 0
            output.rgba8[index + 2] = 0; output.rgba8[index + 3] = UInt8(alpha)
        }
        return output
    }

    static func model(_ source: WarcraftExportedModel) throws -> WarcraftProductionEntityAsset {
        if source.placeholder {
            return WarcraftProductionEntityAsset(
                identity: source.identity, metadata: source.metadata, bounds: source.bounds, model: nil,
                diagnostics: ["Asset '\(source.identity)' status \(source.status); using explicit placeholder."])
        }

        if source.metadataStatus != 0 {
            return WarcraftProductionEntityAsset(
                identity: source.identity, metadata: source.metadata, bounds: source.bounds, model: nil,
                diagnostics: [
                    "Asset '\(source.identity)' class 0x\(String(source.metadata.classID, radix: 16)) " +
                    "metadata status \(source.metadataStatus); using explicit placeholder.",
                ])
        }
        guard source.version == mdxVersion else {
            return WarcraftProductionEntityAsset(
                identity: source.identity, metadata: source.metadata, bounds: source.bounds, model: nil,
                diagnostics: ["Asset '\(source.identity)' MDX \(source.version) is unsupported; using explicit placeholder."])
        }
        var parts: [WarcraftMeshPartDescriptor] = [], materials: [WarcraftMaterialDescriptor] = []
        var diagnostics: [String] = []
        for (geosetIndex, geoset) in source.geosets.enumerated() {
            guard geoset.positions.count == geoset.normals.count,
                  geoset.textureCoordinates.isEmpty ||
                    geoset.positions.count == geoset.textureCoordinates.count,
                  !geoset.positions.isEmpty, !geoset.indices.isEmpty,
                  geoset.indices.count.isMultiple(of: 3),
                  geoset.indices.allSatisfy({ Int($0) < geoset.positions.count }),
                  source.materials.indices.contains(geoset.materialIndex) else {
                throw WarcraftDescriptorError.invalidMesh(
                    "asset '\(source.identity)' geoset \(geosetIndex) has malformed buffers " +
                    "(vertices=\(geoset.positions.count), normals=\(geoset.normals.count), " +
                    "uvs=\(geoset.textureCoordinates.count), indices=\(geoset.indices.count), " +
                    "material=\(geoset.materialIndex)/\(source.materials.count))")
            }
            let sourceMaterial = source.materials[geoset.materialIndex]
            guard sourceMaterial.layerCount > 0, sourceMaterial.firstLayer >= 0,
                  sourceMaterial.firstLayer <= source.layers.count - sourceMaterial.layerCount else {
                throw WarcraftDescriptorError.invalidMesh(
                    "asset '\(source.identity)' material \(geoset.materialIndex) has invalid layers")
            }
            let positions = geoset.positions.map { WarcraftVector3(x: $0.x, y: $0.z, z: $0.y) }
            let normals = geoset.normals.map { WarcraftVector3(x: $0.x, y: $0.z, z: $0.y) }
            let textureCoordinates = geoset.textureCoordinates.isEmpty ?
                Array(repeating: WarcraftVector2(x: 0, y: 0), count: geoset.positions.count) :
                geoset.textureCoordinates
            var indices: [UInt32] = []
            indices.reserveCapacity(geoset.indices.count)
            for index in stride(from: 0, to: geoset.indices.count, by: 3) {
                indices += [UInt32(geoset.indices[index]), UInt32(geoset.indices[index + 2]),
                            UInt32(geoset.indices[index + 1])]
            }
            let layerEnd = sourceMaterial.firstLayer + sourceMaterial.layerCount
            for layerIndex in sourceMaterial.firstLayer..<layerEnd {
                let layer = source.layers[layerIndex]
                let output = try material(source, layer: layer, layerIndex: layerIndex)
                let outputIndex = materials.count
                materials.append(output.material)
                if let diagnostic = output.diagnostic { diagnostics.append(diagnostic) }
                parts.append(WarcraftMeshPartDescriptor(
                    name: "geoset-\(geosetIndex)-layer-\(layerIndex)",
                    positions: positions, normals: normals,
                    textureCoordinates: textureCoordinates, indices: indices,
                    materialIndex: outputIndex))
            }
        }
        let sequences = source.sequences.map {
            WarcraftSequenceDescriptor(name: $0.name, firstFrame: $0.startMilliseconds,
                                       lastFrame: $0.endMilliseconds, looping: $0.flags == 0)
        }
        let model = WarcraftDescriptorContentKey.populate(WarcraftModelDescriptor(
            name: source.identity, geosets: parts, materials: materials, sequences: sequences))
        return WarcraftProductionEntityAsset(
            identity: source.identity, metadata: source.metadata, bounds: source.bounds,
            model: model,
            diagnostics: Array(Set(diagnostics)).sorted())
    }

    /* A copied model is immutable; only metadata varies between entities using the same configstring. */
    static func modelTemplate(_ source: WarcraftExportedModel) throws -> WarcraftProductionEntityAsset {
        var template = source
        template.metadataStatus = 0
        return try model(template)
    }

    /* Team/skin variants change materials only; retain the base configstring's validated geometry buffers. */
    static func modelTemplate(_ source: WarcraftExportedModel,
                              reusingGeometry geometry: WarcraftProductionEntityAsset) throws
        -> WarcraftProductionEntityAsset {
        guard geometry.identity == source.identity else {
            throw WarcraftDescriptorError.invalidMesh(
                "asset '\(source.identity)' cannot reuse geometry from '\(geometry.identity)'")
        }
        var template = try modelTemplate(source)
        guard var model = template.model, let geometryModel = geometry.model else { return template }
        model.geosets = geometryModel.geosets
        model.geometryKey = geometryModel.geometryKey
        template.model = model
        return template
    }

    static func model(_ source: WarcraftExportedModel,
                      template: WarcraftProductionEntityAsset) -> WarcraftProductionEntityAsset {
        if source.metadataStatus != 0 {
            return WarcraftProductionEntityAsset(
                identity: source.identity, metadata: source.metadata, bounds: source.bounds, model: nil,
                diagnostics: [
                    "Asset '\(source.identity)' class 0x\(String(source.metadata.classID, radix: 16)) " +
                    "metadata status \(source.metadataStatus); using explicit placeholder.",
                ])
        }
        var entity = template
        entity.metadata = source.metadata
        return entity
    }

    static func production(abiVersion: UInt32, terrain sourceTerrain: WarcraftExportedTerrain?,
                           models: [UInt64: WarcraftExportedModel],
                           counters: WarcraftAssetCacheCounters) throws -> WarcraftProductionAssets {
        let adaptedTerrain = try sourceTerrain.map(terrain)
        var entities: [UInt64: WarcraftProductionEntityAsset] = [:]
        var diagnostics = adaptedTerrain?.diagnostics ?? []
        for (id, source) in models {
            let adapted = try model(source)
            diagnostics += adapted.diagnostics
            entities[id] = adapted
        }
        return WarcraftProductionAssets(
            abiVersion: abiVersion, terrainKey: nil, terrain: adaptedTerrain?.descriptor,
            worldTransform: adaptedTerrain?.transform, entities: entities, counters: counters,
            terrainTextureCount: adaptedTerrain?.textureCount ?? 0,
            terrainNoCliffCount: adaptedTerrain?.noCliffCount ?? 0,
            diagnostics: Array(Set(diagnostics)).sorted())
    }

    static func production(abiVersion: UInt32, terrain sourceTerrain: WarcraftExportedTerrain?,
                           entities: [UInt64: WarcraftProductionEntityAsset],
                           counters: WarcraftAssetCacheCounters) throws -> WarcraftProductionAssets {
        let adaptedTerrain = try sourceTerrain.map(terrain)
        return WarcraftProductionAssets(
            abiVersion: abiVersion, terrainKey: nil, terrain: adaptedTerrain?.descriptor,
            worldTransform: adaptedTerrain?.transform, entities: entities, counters: counters,
            terrainTextureCount: adaptedTerrain?.textureCount ?? 0,
            terrainNoCliffCount: adaptedTerrain?.noCliffCount ?? 0,
            diagnostics: Array(Set((adaptedTerrain?.diagnostics ?? []) +
                entities.values.flatMap(\.diagnostics))).sorted())
    }

    private static func material(_ model: WarcraftExportedModel, layer: WarcraftExportedLayer,
                                 layerIndex: Int) throws
        -> (material: WarcraftMaterialDescriptor, diagnostic: String?) {
        let blend = try blendMode(layer.blendMode)
        guard layer.alpha.isFinite else {
            throw WarcraftDescriptorError.invalidMesh(
                "asset '\(model.identity)' layer \(layerIndex) has non-finite alpha")
        }
        guard model.textures.indices.contains(layer.textureIndex) else {
            throw WarcraftDescriptorError.invalidMesh(
                "asset '\(model.identity)' layer \(layerIndex) has invalid texture index")
        }
        let source = model.textures[layer.textureIndex]
        let role: WarcraftMaterialRole, sourceImage: WarcraftExportedImage?
        switch source.replaceableID {
        case 1: role = .teamColor; sourceImage = model.teamColorImage
        case 2: role = .teamGlow; sourceImage = model.teamGlowImage
        case 0: role = .surface; sourceImage = source.image
        default:
            role = .surface; sourceImage = model.overrideImage
        }
        if source.replaceableID != 0 {
            guard let sourceImage, !sourceImage.placeholder else {
                let status = sourceImage.map { " status \($0.status)" } ?? ""
                let ownership = source.replaceableID == 1 || source.replaceableID == 2 ?
                    "team image" : "entity image"
                return (WarcraftPlaceholder.material,
                        "Asset '\(model.identity)' replaceable texture \(source.replaceableID)\(status) " +
                        "has no authoritative \(ownership); using explicit placeholder material.")
            }
            return try imageMaterial(
                model, layer: layer, layerIndex: layerIndex, source: sourceImage,
                blend: blend, role: role)
        }
        guard let sourceImage, !sourceImage.placeholder else {
            let status = source.image.map { " status \($0.status)" } ?? ""
            return (WarcraftPlaceholder.material,
                    "Asset '\(model.identity)' texture '\(source.identity)'\(status) is unresolved; " +
                    "using explicit placeholder material.")
        }
        return try imageMaterial(
            model, layer: layer, layerIndex: layerIndex, source: sourceImage, blend: blend, role: role)
    }

    private static func imageMaterial(_ model: WarcraftExportedModel, layer: WarcraftExportedLayer,
                                      layerIndex: Int, source: WarcraftExportedImage,
                                      blend: WarcraftBlendMode, role: WarcraftMaterialRole) throws
        -> (material: WarcraftMaterialDescriptor, diagnostic: String?) {
        if blend == .modulate || blend == .modulate2x {
            guard let image = try modulateImage(source, doubled: blend == .modulate2x) else {
                return (WarcraftPlaceholder.material,
                        "Asset '\(model.identity)' layer \(layerIndex) uses a colored modulate texture; " +
                        "using explicit placeholder material.")
            }
            return (WarcraftMaterialDescriptor(
                name: source.identity, color: WarcraftColor(
                    red: 1, green: 1, blue: 1, alpha: min(max(layer.alpha, 0), 1)),
                texture: image, blendMode: .alpha, role: role, unlit: true,
                sourceBlendMode: layer.blendMode, sourceFlags: layer.flags,
                writesDepth: false, readsDepth: layer.flags & layerNoDepthTest == 0,
                twoSided: layer.flags & layerTwoSided != 0,
                unfogged: layer.flags & layerUnfogged != 0), nil)
        }
        /* Replaceable images alter only TEXS selection; retain the authored layer render state. */
        return (WarcraftMaterialDescriptor(
            name: source.identity, color: WarcraftColor(
                red: 1, green: 1, blue: 1, alpha: min(max(layer.alpha, 0), 1)),
            texture: try image(source), blendMode: blend, role: role,
            unlit: layer.flags & layerUnshaded != 0,
            sourceBlendMode: layer.blendMode, sourceFlags: layer.flags,
            writesDepth: (layer.blendMode == 0 || layer.blendMode == 1) &&
                layer.flags & layerNoDepthSet == 0,
            readsDepth: layer.flags & layerNoDepthTest == 0,
            twoSided: layer.flags & layerTwoSided != 0,
            unfogged: layer.flags & layerUnfogged != 0), nil)
    }

    private static func blendMode(_ raw: UInt32) throws -> WarcraftBlendMode {
        switch raw {
        case 0: return .opaque
        case 1: return .alphaKey
        case 2: return .alpha
        case 3: return .additive
        case 4: return .addAlpha
        case 5: return .modulate
        case 6: return .modulate2x
        default: throw WarcraftDescriptorError.invalidMesh("unknown MDX blend mode \(raw)")
        }
    }

    private static func terrain(_ source: WarcraftExportedTerrain) throws -> WarcraftAdaptedTerrain {
        guard source.cornerWidth > 1, source.cornerHeight > 1,
              source.tileWidth == source.cornerWidth - 1,
              source.tileHeight == source.cornerHeight - 1,
              source.chunkTiles == terrainChunkTiles,
              source.chunkCountX == (source.tileWidth + terrainChunkTiles - 1) / terrainChunkTiles,
              source.chunkCountZ == (source.tileHeight + terrainChunkTiles - 1) / terrainChunkTiles,
              source.corners.count == source.cornerWidth * source.cornerHeight,
              !source.groundTypes.isEmpty,
              Set(source.groundTypes).count == source.groundTypes.count,
              !source.groundTextures.isEmpty else {
            throw WarcraftDescriptorError.invalidTerrain("exported terrain metadata is inconsistent")
        }
        let spanX = source.bounds.maxX - source.bounds.minX
        let spanZ = source.bounds.maxZ - source.bounds.minZ
        guard spanX.isFinite, spanZ.isFinite, spanX > 0, spanZ > 0 else {
            throw WarcraftDescriptorError.invalidTerrain("exported terrain bounds are invalid")
        }
        let tileSizeX = spanX / Float(source.tileWidth), tileSizeZ = spanZ / Float(source.tileHeight)
        guard abs(tileSizeX - tileSizeZ) <= max(tileSizeX, tileSizeZ) * 0.001 else {
            throw WarcraftDescriptorError.invalidTerrain("exported terrain tiles are not square")
        }
        let scale: Float = 1.08 / max(spanX, spanZ)
        let transform = WarcraftWorldTransform(
            centerX: (source.bounds.minX + source.bounds.maxX) * 0.5,
            centerZ: (source.bounds.minZ + source.bounds.maxZ) * 0.5, scale: scale)
        let groundIndices = Dictionary(uniqueKeysWithValues: source.groundTypes.enumerated().map {
            ($0.element, $0.offset)
        })
        let cliffIndices = Dictionary(uniqueKeysWithValues: source.cliffTypes.enumerated().map {
            ($0.element, $0.offset)
        })
        let groundImages = try textureImages(
            source.groundTextures, types: source.groundTypes, label: "ground")
        let cliffImages = try textureImages(
            source.cliffTextures, types: source.cliffTypes, label: "cliff")
        var materials: [WarcraftMaterialDescriptor] = []
        var groundMaterials: [Int: Int] = [:]
        let groundLayers = groundImages.keys.sorted()
        guard let baseGroundLayer = groundLayers.first else {
            throw WarcraftDescriptorError.invalidTerrain("terrain has no referenced ground textures")
        }
        for index in groundLayers {
            let texture = groundImages[index]!
            guard !texture.placeholder else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "ground texture \(fourCC(source.groundTypes[index])) status \(texture.status)")
            }
            groundMaterials[index] = materials.count
            materials.append(WarcraftMaterialDescriptor(
                name: texture.identity, color: .white, texture: try image(texture),
                blendMode: index == baseGroundLayer ? .opaque : .alpha))
        }
        let baseGroundMaterial = groundMaterials[baseGroundLayer]!
        var cliffMaterials: [Int: Int] = [:]
        for index in cliffImages.keys.sorted() {
            let texture = cliffImages[index]!
            guard !texture.placeholder else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "cliff texture \(fourCC(source.cliffTypes[index])) status \(texture.status)")
            }
            cliffMaterials[index] = materials.count
            materials.append(WarcraftMaterialDescriptor(
                name: texture.identity, color: .white, texture: try image(texture), blendMode: .opaque))
        }
        func corner(_ x: Int, _ z: Int) -> WarcraftExportedTerrainCorner {
            source.corners[z * source.cornerWidth + x]
        }
        let wetCornerCount = source.corners.filter { $0.flags & terrainWater != 0 }.count
        let hasRenderableWater = (0..<(source.cornerHeight - 1)).contains { z in
            (0..<(source.cornerWidth - 1)).contains { x in
                let values = [
                    corner(x, z), corner(x + 1, z), corner(x + 1, z + 1), corner(x, z + 1),
                ]
                return values.contains { $0.flags & terrainWater != 0 } &&
                    !values.contains { $0.flags & terrainMapEdge != 0 }
            }
        }
        let waterIndex: Int
        if hasRenderableWater {
            guard let water = source.waterTexture, water.typeIndex == 0, water.typeID == 0,
                  water.cornerCount == wetCornerCount else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "renderable water has no matching authoritative texture reference")
            }
            waterIndex = materials.count
            materials.append(WarcraftMaterialDescriptor(
                name: water.image.identity, color: .white, texture: try image(water.image),
                blendMode: .alpha, role: water.image.placeholder ? .placeholder : .water, unlit: true))
        } else {
            guard source.waterTexture == nil else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "terrain without a renderable water tile unexpectedly exported a water texture")
            }
            waterIndex = baseGroundMaterial
        }
        var cells: [WarcraftTerrainCellDescriptor] = []
        cells.reserveCapacity(source.tileWidth * source.tileHeight)
        for z in 0..<source.tileHeight {
            for x in 0..<source.tileWidth {
                let values = [corner(x, z), corner(x + 1, z), corner(x + 1, z + 1), corner(x, z + 1)]
                guard let materialIndex = groundIndices[values[0].groundID] else {
                    throw WarcraftDescriptorError.invalidTerrain(
                        "terrain ground \(fourCC(values[0].groundID)) is absent from its type table")
                }
                var features: [WarcraftTerrainFeature] = []
                let ramp = values.contains { $0.flags & terrainRamp != 0 }
                if ramp { features.append(.ramp) }
                let cliff = !ramp && values.contains { $0.flags & terrainNoCliff == 0 } &&
                    Set(values.map(\.cliffLevel)).count > 1
                if cliff { features.append(.cliff) }
                /* Desktop IsTileWater rejects a wet tile when any of its four corners is a map edge. */
                let water = values.contains { $0.flags & terrainWater != 0 } &&
                    !values.contains { $0.flags & terrainMapEdge != 0 }
                let u0 = Float(x % 3) / 3, u1 = Float(x % 3 + 1) / 3
                let v0 = Float(z % 3) / 3, v1 = Float(z % 3 + 1) / 3
                let surfaces = try surfaceLayers(
                    values, groundIndices: groundIndices,
                    textures: groundImages, materials: groundMaterials)
                let cliffID = values.first { $0.flags & terrainNoCliff == 0 }?.cliffID
                let cellCliffIndex = try cliffID.map {
                    guard let index = cliffIndices[$0] else {
                        throw WarcraftDescriptorError.invalidTerrain(
                            "terrain cliff \(fourCC($0)) is absent from its type table")
                    }
                    guard let material = cliffMaterials[index] else {
                        throw WarcraftDescriptorError.invalidTerrain(
                            "referenced cliff \(fourCC($0)) has no exported texture")
                    }
                    return material
                }
                cells.append(WarcraftTerrainCellDescriptor(
                    materialIndex: surfaces.first?.materialIndex ?? materialIndex,
                    waterLevel: water ? values.map(\.waterHeight).reduce(0, +) * scale / 4 : nil,
                    features: features,
                    waterCornerHeights: water ? values.map { $0.waterHeight * scale } : nil,
                    waterTextureCoordinates: water ? [
                        WarcraftVector2(x: u0, y: v0), WarcraftVector2(x: u1, y: v0),
                        WarcraftVector2(x: u1, y: v1), WarcraftVector2(x: u0, y: v1),
                    ] : nil,
                    waterCornerOpacities: water ? values.map {
                        min(0.5, max(0, ($0.waterHeight - $0.height) / 50))
                    } : nil,
                    surfaceLayers: surfaces, cliffMaterialIndex: cellCliffIndex))
            }
        }
        return WarcraftAdaptedTerrain(
            descriptor: WarcraftTerrainDescriptor(
                width: source.tileWidth, height: source.tileHeight,
                cellSize: tileSizeX * scale,
                heights: source.corners.map { $0.height * scale }, cells: cells,
                materials: materials, waterMaterialIndex: waterIndex,
                cliffMaterialIndex: cliffMaterials.values.first ?? baseGroundMaterial,
                rampMaterialIndex: baseGroundMaterial),
            transform: transform,
            textureCount: source.groundTextures.count + source.cliffTextures.count +
                (source.waterTexture == nil ? 0 : 1),
            noCliffCount: source.corners.filter { $0.flags & terrainNoCliff != 0 }.count,
            diagnostics: ["Terrain geometry and textures use authoritative asset ABI v2 descriptors."])
    }

    private static func surfaceLayers(
        _ corners: [WarcraftExportedTerrainCorner], groundIndices: [UInt32: Int],
        textures: [Int: WarcraftExportedImage],
        materials: [Int: Int]) throws -> [WarcraftTerrainSurfaceLayer] {
        let indices = try corners.map { corner -> Int in
            guard let index = groundIndices[corner.groundID] else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "terrain ground \(fourCC(corner.groundID)) is absent from its type table")
            }
            return index
        }
        var result: [WarcraftTerrainSurfaceLayer] = []
        let layers = textures.keys.sorted()
        guard !layers.isEmpty else {
            throw WarcraftDescriptorError.invalidTerrain("terrain has no referenced ground textures")
        }
        for (position, layer) in layers.enumerated() {
            var tile = position == 0 ? 15 :
                (indices[2] >= layer ? 4 : 0) + (indices[3] >= layer ? 8 : 0) +
                (indices[1] >= layer ? 1 : 0) + (indices[0] >= layer ? 2 : 0)
            guard tile != 0 else { continue }
            guard let texture = textures[layer], let material = materials[layer] else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "exported ground layer \(layer) has no material")
            }
            guard texture.width >= 64, texture.height >= 64,
                  texture.width.isMultiple(of: 64), texture.height.isMultiple(of: 64) else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "terrain atlas '\(texture.identity)' is not 64-pixel tiled")
            }
            var offsetX: Float = 0
            if tile == 15 && texture.width > texture.height {
                switch corners[0].groundVariation {
                case 0...15: tile = Int(corners[0].groundVariation); offsetX = 0.5
                case 16: tile = 15
                default: tile = 0
                }
            }
            let u = 1 / Float(texture.width / 64), v = 1 / Float(texture.height / 64)
            let minU = u * Float(tile % 4) + offsetX, maxU = minU + u
            let minV = v * Float(tile / 4), maxV = minV + v
            let centerU = minU + u * 0.5, centerV = minV + v * 0.5
            func inset(_ x: Float, _ center: Float) -> Float { x + (center - x) * 0.05 }
            result.append(WarcraftTerrainSurfaceLayer(
                materialIndex: material, textureCoordinates: [
                    WarcraftVector2(x: inset(minU, centerU), y: inset(maxV, centerV)),
                    WarcraftVector2(x: inset(maxU, centerU), y: inset(maxV, centerV)),
                    WarcraftVector2(x: inset(maxU, centerU), y: inset(minV, centerV)),
                    WarcraftVector2(x: inset(minU, centerU), y: inset(minV, centerV)),
                ]))
        }
        return result
    }

    private static func textureImages(
        _ references: [WarcraftExportedTerrainTexture], types: [UInt32], label: String) throws
        -> [Int: WarcraftExportedImage] {
        guard Set(references.map(\.typeIndex)).count == references.count else {
            throw WarcraftDescriptorError.invalidTerrain("\(label) texture references contain duplicate indices")
        }
        var result: [Int: WarcraftExportedImage] = [:]
        for reference in references {
            guard types.indices.contains(reference.typeIndex),
                  types[reference.typeIndex] == reference.typeID,
                  reference.cornerCount > 0 else {
                throw WarcraftDescriptorError.invalidTerrain(
                    "\(label) texture reference \(reference.typeIndex) is inconsistent")
            }
            result[reference.typeIndex] = reference.image
        }
        return result
    }

    private static func fourCC(_ value: UInt32) -> String {
        String(decoding: (0..<4).map { UInt8(truncatingIfNeeded: value >> UInt32($0 * 8)) },
               as: UTF8.self)
    }
}
