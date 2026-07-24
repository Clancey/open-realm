import Foundation

struct WarcraftTerrainChunkID: Codable, Equatable, Hashable, Comparable, Sendable {
    var x: Int
    var z: Int

    static func < (lhs: Self, rhs: Self) -> Bool { lhs.z == rhs.z ? lhs.x < rhs.x : lhs.z < rhs.z }
}

struct WarcraftTerrainChunkDescriptor: Codable, Equatable, Sendable {
    var id: WarcraftTerrainChunkID
    var cellBounds: WarcraftIntBounds
    var mesh: WarcraftModelDescriptor
    var contentKey: String
}

struct WarcraftIntBounds: Codable, Equatable, Sendable {
    var minX: Int
    var minZ: Int
    var maxX: Int
    var maxZ: Int
}

struct WarcraftFogRenderDescriptor: Codable, Equatable, Sendable {
    var image: WarcraftImageDescriptor
    var width: Float
    var depth: Float
    var contentKey: String
}

struct WarcraftRenderEntityDescriptor: Codable, Equatable, Sendable {
    var descriptor: WarcraftEntityDescriptor
    var model: WarcraftModelDescriptor
    var position: WarcraftVector3
    var scale: WarcraftVector3
    var overlayScale: WarcraftVector3
    var teamColor: WarcraftColor
    var animation: WarcraftResolvedAnimation?
    var geometryKey: String
    var materialKey: String
    var stateKey: String
    var usedPlaceholder: Bool
}

struct WarcraftRenderSnapshot: Codable, Equatable, Sendable {
    var generation: UInt64
    var terrainChunks: [WarcraftTerrainChunkDescriptor]
    var fog: WarcraftFogRenderDescriptor?
    var entities: [WarcraftRenderEntityDescriptor]
    var diagnostics: [String]

    static let empty = WarcraftRenderSnapshot(generation: 0, terrainChunks: [], fog: nil,
                                               entities: [], diagnostics: [])
}

struct WarcraftMeshBounds: Equatable, Sendable {
    var center: WarcraftVector3
    var size: WarcraftVector3
}

struct WarcraftBarState: Equatable, Sendable {
    var enabled: Bool
    var scale: Float
    var offset: Float
}

enum WarcraftOverlayReducer {
    static func bar(_ value: Float?) -> WarcraftBarState {
        guard let value else { return WarcraftBarState(enabled: false, scale: 0, offset: -0.38) }
        let clamped = min(max(value, 0), 1)
        return WarcraftBarState(enabled: true, scale: clamped, offset: (clamped - 1) * 0.38)
    }
}

enum WarcraftMeshMath {
    static func bounds(_ model: WarcraftModelDescriptor) -> WarcraftMeshBounds? {
        let positions = model.geosets.flatMap(\.positions)
        guard let first = positions.first else { return nil }
        let limits = positions.dropFirst().reduce((min: first, max: first)) { result, point in
            (WarcraftVector3(x: min(result.min.x, point.x), y: min(result.min.y, point.y),
                             z: min(result.min.z, point.z)),
             WarcraftVector3(x: max(result.max.x, point.x), y: max(result.max.y, point.y),
                             z: max(result.max.z, point.z)))
        }
        return WarcraftMeshBounds(
            center: WarcraftVector3(x: (limits.min.x + limits.max.x) * 0.5,
                                    y: (limits.min.y + limits.max.y) * 0.5,
                                    z: (limits.min.z + limits.max.z) * 0.5),
            size: WarcraftVector3(x: limits.max.x - limits.min.x,
                                  y: limits.max.y - limits.min.y,
                                  z: limits.max.z - limits.min.z))
    }

    static func facesMatchNormals(_ part: WarcraftMeshPartDescriptor) -> Bool {
        stride(from: 0, to: part.indices.count, by: 3).allSatisfy { offset in
            guard offset + 2 < part.indices.count else { return false }
            let ia = Int(part.indices[offset]), ib = Int(part.indices[offset + 1]),
                ic = Int(part.indices[offset + 2])
            guard ia < part.positions.count, ib < part.positions.count, ic < part.positions.count else {
                return false
            }
            let a = part.positions[ia], b = part.positions[ib], c = part.positions[ic]
            let ab = WarcraftVector3(x: b.x - a.x, y: b.y - a.y, z: b.z - a.z)
            let ac = WarcraftVector3(x: c.x - a.x, y: c.y - a.y, z: c.z - a.z)
            let face = WarcraftVector3(x: ab.y * ac.z - ab.z * ac.y,
                                       y: ab.z * ac.x - ab.x * ac.z,
                                       z: ab.x * ac.y - ab.y * ac.x)
            let normal = part.normals[ia]
            return face.x * normal.x + face.y * normal.y + face.z * normal.z >= 0
        }
    }
}

enum WarcraftImageNormalizer {
    static func topLeft(_ image: WarcraftImageDescriptor) throws -> WarcraftImageDescriptor {
        guard image.width > 0, image.height > 0,
              image.rgba8.count == image.width * image.height * 4 else {
            throw WarcraftDescriptorError.invalidImage("RGBA byte count does not match dimensions")
        }
        guard image.orientation == .bottomLeft else { return image }
        let stride = image.width * 4
        var bytes = image.rgba8
        for y in 0..<(image.height / 2) {
            let opposite = image.height - y - 1
            let a = Array(bytes[(y * stride)..<((y + 1) * stride)])
            bytes.replaceSubrange((y * stride)..<((y + 1) * stride),
                                  with: bytes[(opposite * stride)..<((opposite + 1) * stride)])
            bytes.replaceSubrange((opposite * stride)..<((opposite + 1) * stride), with: a)
        }
        return WarcraftImageDescriptor(width: image.width, height: image.height, rgba8: bytes,
                                       orientation: .topLeft)
    }
}

enum WarcraftAtlasMapper {
    static func map(_ part: WarcraftMeshPartDescriptor, region: WarcraftAtlasRegion?)
        throws -> WarcraftMeshPartDescriptor {
        guard let region else { return part }
        guard region.x >= 0, region.y >= 0, region.width > 0, region.height > 0,
              region.atlasWidth > 0, region.atlasHeight > 0,
              region.x + region.width <= region.atlasWidth,
              region.y + region.height <= region.atlasHeight else {
            throw WarcraftDescriptorError.invalidImage("atlas region exceeds texture bounds")
        }
        var mapped = part
        mapped.textureCoordinates = part.textureCoordinates.map {
            WarcraftVector2(
                x: (Float(region.x) + $0.x * Float(region.width)) / Float(region.atlasWidth),
                y: (Float(region.y) + $0.y * Float(region.height)) / Float(region.atlasHeight))
        }
        return mapped
    }
}

enum WarcraftFogBuilder {
    static func build(_ fog: WarcraftFogDescriptor, terrain: WarcraftTerrainDescriptor?)
        throws -> WarcraftFogRenderDescriptor {
        guard fog.width > 0, fog.height > 0, fog.states.count == fog.width * fog.height else {
            throw WarcraftDescriptorError.invalidFog("fog state count does not match dimensions")
        }
        let bytes = fog.states.flatMap { state -> [UInt8] in
            switch state {
            case .hidden: return [0, 0, 0, 235]
            case .explored: return [18, 24, 36, 145]
            case .visible: return [255, 255, 255, 0]
            }
        }
        let image = WarcraftImageDescriptor(width: fog.width, height: fog.height, rgba8: bytes,
                                            orientation: .topLeft)
        let width = Float(terrain?.width ?? fog.width) * (terrain?.cellSize ?? 0.02)
        let depth = Float(terrain?.height ?? fog.height) * (terrain?.cellSize ?? 0.02)
        return WarcraftFogRenderDescriptor(image: image, width: width, depth: depth,
            contentKey: WarcraftDescriptorContentKey.fog(image, width: width, depth: depth))
    }

    static func mesh(_ fog: WarcraftFogRenderDescriptor) -> WarcraftMeshPartDescriptor {
        WarcraftMeshPartDescriptor(
            name: "fog-plane", positions: [
                WarcraftVector3(x: -fog.width * 0.5, y: 0.002, z: -fog.depth * 0.5),
                WarcraftVector3(x: fog.width * 0.5, y: 0.002, z: -fog.depth * 0.5),
                WarcraftVector3(x: fog.width * 0.5, y: 0.002, z: fog.depth * 0.5),
                WarcraftVector3(x: -fog.width * 0.5, y: 0.002, z: fog.depth * 0.5),
            ], normals: Array(repeating: WarcraftVector3(x: 0, y: 1, z: 0), count: 4),
            textureCoordinates: [
                WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                WarcraftVector2(x: 1, y: 1), WarcraftVector2(x: 0, y: 1),
            ], indices: [0, 2, 1, 0, 3, 2], materialIndex: 0)
    }
}

enum WarcraftDescriptorContentKey {
    static func image(_ image: WarcraftImageDescriptor) -> String {
        var hash = WarcraftContentHasher()
        hash.update("image")
        hash.update(image.width); hash.update(image.height)
        hash.update(image.orientation.rawValue)
        hash.update(image.rgba8)
        return hash.digest()
    }

    static func fog(_ image: WarcraftImageDescriptor, width: Float, depth: Float) -> String {
        var hash = WarcraftContentHasher()
        hash.update("fog")
        hash.update(image.width); hash.update(image.height)
        hash.update(image.orientation.rawValue)
        hash.update(width); hash.update(depth)
        hash.update(image.rgba8)
        return hash.digest()
    }

    static func geometry(_ model: WarcraftModelDescriptor) -> String {
        var hash = WarcraftContentHasher()
        hash.update("model-geometry")
        hash.update(model.name)
        hash.update(model.geosets.count)
        for part in model.geosets {
            hash.update(part.name)
            hash.update(part.materialIndex)
            hash.update(part.positions.count)
            for value in part.positions { hash.update(value.x); hash.update(value.y); hash.update(value.z) }
            hash.update(part.normals.count)
            for value in part.normals { hash.update(value.x); hash.update(value.y); hash.update(value.z) }
            hash.update(part.textureCoordinates.count)
            for value in part.textureCoordinates { hash.update(value.x); hash.update(value.y) }
            hash.update(part.vertexColors.count)
            for value in part.vertexColors {
                hash.update(value.red); hash.update(value.green)
                hash.update(value.blue); hash.update(value.alpha)
            }
            hash.update(part.indices.count)
            for value in part.indices { hash.update(value) }
        }
        hash.update(model.sequences.count)
        for sequence in model.sequences {
            hash.update(sequence.name)
            hash.update(sequence.firstFrame)
            hash.update(sequence.lastFrame)
            hash.update(sequence.looping)
        }
        return hash.digest()
    }

    static func materials(_ materials: [WarcraftMaterialDescriptor]) -> String {
        var hash = WarcraftContentHasher()
        hash.update("model-materials")
        hash.update(materials.count)
        for material in materials {
            hash.update(material.name)
            hash.update(material.color.red); hash.update(material.color.green)
            hash.update(material.color.blue); hash.update(material.color.alpha)
            hash.update(material.blendMode.rawValue)
            hash.update(material.role.rawValue)
            hash.update(material.unlit)
            if let sourceBlendMode = material.sourceBlendMode {
                hash.update(true); hash.update(sourceBlendMode)
            } else {
                hash.update(false)
            }
            hash.update(material.sourceFlags)
            hash.update(material.writesDepth); hash.update(material.readsDepth)
            hash.update(material.twoSided); hash.update(material.unfogged)
            if let atlas = material.atlasRegion {
                hash.update(true)
                hash.update(atlas.x); hash.update(atlas.y)
                hash.update(atlas.width); hash.update(atlas.height)
                hash.update(atlas.atlasWidth); hash.update(atlas.atlasHeight)
            } else {
                hash.update(false)
            }
            if let image = material.texture {
                hash.update(true)
                hash.update(image.width); hash.update(image.height)
                hash.update(image.orientation.rawValue)
                hash.update(image.rgba8)
            } else {
                hash.update(false)
            }
        }
        return hash.digest()
    }

    static func combined(geometry: String, materials: String) -> String {
        var hash = WarcraftContentHasher()
        hash.update(geometry)
        hash.update(materials)
        return hash.digest()
    }

    static func populate(_ model: WarcraftModelDescriptor) -> WarcraftModelDescriptor {
        var output = model
        output.geometryKey = geometry(model)
        output.materialKey = materials(model.materials)
        return output
    }
}

enum WarcraftTerrainChunkBuilder {
    static let chunkSide = 32

    static func build(_ terrain: WarcraftTerrainDescriptor) throws -> [WarcraftTerrainChunkDescriptor] {
        guard terrain.width > 0, terrain.height > 0,
              terrain.heights.count == (terrain.width + 1) * (terrain.height + 1),
              terrain.cells.count == terrain.width * terrain.height else {
            throw WarcraftDescriptorError.invalidTerrain("terrain arrays do not match dimensions")
        }
        let materialCount = terrain.materials.count
        guard materialCount > 0,
              [terrain.waterMaterialIndex, terrain.cliffMaterialIndex, terrain.rampMaterialIndex]
                .allSatisfy({ $0 >= 0 && $0 < materialCount }),
              terrain.cells.allSatisfy({
                  $0.materialIndex >= 0 && $0.materialIndex < materialCount &&
                      $0.surfaceLayers.allSatisfy {
                          $0.materialIndex >= 0 && $0.materialIndex < materialCount &&
                              $0.textureCoordinates.count == 4
                      } &&
                      $0.cliffMaterialIndex.map { $0 >= 0 && $0 < materialCount } ?? true
              }) else {
            throw WarcraftDescriptorError.invalidTerrain("terrain material index is out of bounds")
        }
        var result: [WarcraftTerrainChunkDescriptor] = []
        let materialKey = WarcraftDescriptorContentKey.materials(terrain.materials)
        for z in stride(from: 0, to: terrain.height, by: chunkSide) {
            for x in stride(from: 0, to: terrain.width, by: chunkSide) {
                result.append(try chunk(terrain, minX: x, minZ: z,
                                        maxX: min(x + chunkSide, terrain.width),
                                        maxZ: min(z + chunkSide, terrain.height),
                                        materialKey: materialKey))
            }
        }
        return result
    }

    private static func chunk(_ terrain: WarcraftTerrainDescriptor, minX: Int, minZ: Int,
                              maxX: Int, maxZ: Int, materialKey: String) throws
        -> WarcraftTerrainChunkDescriptor {
        var parts = terrain.materials.indices.map {
            WarcraftMeshPartDescriptor(name: "terrain-material-\($0)", positions: [], normals: [],
                                       textureCoordinates: [], indices: [], materialIndex: $0)
        }
        let centerX = Float(terrain.width) * terrain.cellSize * 0.5
        let centerZ = Float(terrain.height) * terrain.cellSize * 0.5
        func point(_ x: Int, _ z: Int, _ height: Float) -> WarcraftVector3 {
            WarcraftVector3(x: Float(x) * terrain.cellSize - centerX, y: height,
                            z: Float(z) * terrain.cellSize - centerZ)
        }
        func height(_ x: Int, _ z: Int) -> Float { terrain.heights[z * (terrain.width + 1) + x] }
        func quad(_ material: Int, _ a: WarcraftVector3, _ b: WarcraftVector3,
                  _ c: WarcraftVector3, _ d: WarcraftVector3, normal: WarcraftVector3,
                  uv: [WarcraftVector2] = [
                    WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                    WarcraftVector2(x: 1, y: 1), WarcraftVector2(x: 0, y: 1),
                  ], colors: [WarcraftColor] = []) {
            let base = UInt32(parts[material].positions.count)
            parts[material].positions += [a, b, c, d]
            parts[material].normals += [normal, normal, normal, normal]
            parts[material].textureCoordinates += uv
            parts[material].vertexColors += colors
            let ab = WarcraftVector3(x: b.x - a.x, y: b.y - a.y, z: b.z - a.z)
            let ac = WarcraftVector3(x: c.x - a.x, y: c.y - a.y, z: c.z - a.z)
            let face = WarcraftVector3(x: ab.y * ac.z - ab.z * ac.y,
                                       y: ab.z * ac.x - ab.x * ac.z,
                                       z: ab.x * ac.y - ab.y * ac.x)
            if face.x * normal.x + face.y * normal.y + face.z * normal.z >= 0 {
                parts[material].indices += [base, base + 1, base + 2, base, base + 2, base + 3]
            } else {
                parts[material].indices += [base, base + 2, base + 1, base, base + 3, base + 2]
            }
        }
        for z in minZ..<maxZ {
            for x in minX..<maxX {
                let cell = terrain.cells[z * terrain.width + x]
                let h00 = height(x, z), h10 = height(x + 1, z)
                let h11 = height(x + 1, z + 1), h01 = height(x, z + 1)
                let fallbackMaterial = cell.features.contains(.ramp) ?
                    terrain.rampMaterialIndex : cell.materialIndex
                let surface = cell.surfaceLayers.isEmpty ? [
                    WarcraftTerrainSurfaceLayer(materialIndex: fallbackMaterial, textureCoordinates: [
                        WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                        WarcraftVector2(x: 1, y: 1), WarcraftVector2(x: 0, y: 1),
                    ]),
                ] : cell.surfaceLayers
                for layer in surface {
                    quad(layer.materialIndex, point(x, z, h00), point(x + 1, z, h10),
                         point(x + 1, z + 1, h11), point(x, z + 1, h01),
                         normal: normalizedNormal(h00: h00, h10: h10, h01: h01,
                                                  cellSize: terrain.cellSize),
                         uv: layer.textureCoordinates)
                }
                if let water = cell.waterLevel {
                    let levels = cell.waterCornerHeights.flatMap { $0.count == 4 ? $0 : nil } ??
                        [water, water, water, water]
                    let uv = cell.waterTextureCoordinates.flatMap { $0.count == 4 ? $0 : nil } ?? [
                        WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                        WarcraftVector2(x: 1, y: 1), WarcraftVector2(x: 0, y: 1),
                    ]
                    let opacity = cell.waterCornerOpacities.flatMap { $0.count == 4 ? $0 : nil } ??
                        [Float](repeating: 0.5, count: 4)
                    quad(terrain.waterMaterialIndex, point(x, z, levels[0]), point(x + 1, z, levels[1]),
                         point(x + 1, z + 1, levels[2]), point(x, z + 1, levels[3]),
                         normal: WarcraftVector3(x: 0, y: 1, z: 0), uv: uv,
                         colors: opacity.map {
                             WarcraftColor(red: 1, green: 1, blue: 1, alpha: $0)
                         })
                }
                guard cell.features.contains(.cliff) else { continue }
                let bottom = min(h00, h10, h11, h01) - terrain.cellSize
                let cliffMaterial = cell.cliffMaterialIndex ?? terrain.cliffMaterialIndex
                quad(cliffMaterial, point(x, z, bottom), point(x, z + 1, bottom),
                     point(x, z + 1, h01), point(x, z, h00),
                     normal: WarcraftVector3(x: -1, y: 0, z: 0))
                quad(cliffMaterial, point(x + 1, z + 1, bottom), point(x + 1, z, bottom),
                     point(x + 1, z, h10), point(x + 1, z + 1, h11),
                     normal: WarcraftVector3(x: 1, y: 0, z: 0))
                quad(cliffMaterial, point(x + 1, z, bottom), point(x, z, bottom),
                     point(x, z, h00), point(x + 1, z, h10),
                     normal: WarcraftVector3(x: 0, y: 0, z: -1))
                quad(cliffMaterial, point(x, z + 1, bottom), point(x + 1, z + 1, bottom),
                     point(x + 1, z + 1, h11), point(x, z + 1, h01),
                     normal: WarcraftVector3(x: 0, y: 0, z: 1))
            }
        }
        parts.removeAll { $0.positions.isEmpty }
        for part in parts {
            guard part.normals.count == part.positions.count,
                  part.textureCoordinates.count == part.positions.count,
                  (part.vertexColors.isEmpty || part.vertexColors.count == part.positions.count),
                  part.indices.allSatisfy({ Int($0) < part.positions.count }),
                  part.indices.count.isMultiple(of: 3) else {
                throw WarcraftDescriptorError.invalidMesh("terrain part topology is invalid")
            }
        }
        var model = WarcraftModelDescriptor(
            name: "terrain-\(minX)-\(minZ)", geosets: parts,
            materials: terrain.materials, sequences: [])
        let geometryKey = WarcraftDescriptorContentKey.geometry(model)
        model.geometryKey = geometryKey
        model.materialKey = materialKey
        return WarcraftTerrainChunkDescriptor(
            id: WarcraftTerrainChunkID(x: minX / chunkSide, z: minZ / chunkSide),
            cellBounds: WarcraftIntBounds(minX: minX, minZ: minZ, maxX: maxX, maxZ: maxZ),
            mesh: model, contentKey: WarcraftDescriptorContentKey.combined(
                geometry: geometryKey, materials: materialKey))
    }

    private static func normalizedNormal(h00: Float, h10: Float, h01: Float,
                                         cellSize: Float) -> WarcraftVector3 {
        let x = h00 - h10, y = max(cellSize, 0.0001), z = h00 - h01
        let length = sqrt(x * x + y * y + z * z)
        return WarcraftVector3(x: x / length, y: y / length, z: z / length)
    }
}

enum WarcraftPlaceholder {
    static let material = WarcraftMaterialDescriptor(
        name: "missing-production-asset", color: .placeholder, texture: nil,
        blendMode: .opaque, role: .placeholder, unlit: true)
    static let model = WarcraftModelDescriptor(
        name: "missing-production-asset",
        geosets: [WarcraftMeshPartDescriptor(
            name: "missing-production-asset", positions: [
                WarcraftVector3(x: -0.5, y: 0, z: -0.5), WarcraftVector3(x: 0.5, y: 0, z: -0.5),
                WarcraftVector3(x: 0, y: 1, z: 0.5), WarcraftVector3(x: 0, y: 0.25, z: 0),
            ], normals: Array(repeating: WarcraftVector3(x: 0, y: 1, z: 0), count: 4),
            textureCoordinates: [
                WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                WarcraftVector2(x: 0.5, y: 1), WarcraftVector2(x: 0.5, y: 0.5),
            ], indices: [0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0], materialIndex: 0)],
        materials: [material], sequences: [])
}

enum WarcraftSceneBuilder {
    static func build(_ scene: WarcraftSceneDescriptor) throws -> WarcraftRenderSnapshot {
        let chunks = try scene.terrain.map(WarcraftTerrainChunkBuilder.build) ?? []
        let fog = try scene.fog.map { try WarcraftFogBuilder.build($0, terrain: scene.terrain) }
        var diagnostics = scene.diagnostics
        let entities = try scene.entities.map { entity -> WarcraftRenderEntityDescriptor in
            let placeholder = entity.model == nil
            let model = entity.model ?? WarcraftPlaceholder.model
            if placeholder { diagnostics.append("Missing production descriptor '\(entity.assetName)'; using placeholder.") }
            let position: WarcraftVector3
            if scene.coordinateSpace == .terrainGrid, let terrain = scene.terrain {
                position = WarcraftVector3(
                    x: entity.position.x * terrain.cellSize - Float(terrain.width) * terrain.cellSize * 0.5,
                    y: entity.position.y,
                    z: entity.position.z * terrain.cellSize - Float(terrain.height) * terrain.cellSize * 0.5)
            } else {
                position = entity.position
            }
            let geometryKey = model.geometryKey ?? WarcraftDescriptorContentKey.geometry(model)
            var materialHasher = WarcraftContentHasher()
            materialHasher.update(model.materialKey ?? WarcraftDescriptorContentKey.materials(model.materials))
            materialHasher.update(UInt32(entity.teamColor))
            let materialKey = materialHasher.digest()
            let stateData = try JSONEncoder.sorted.encode([
                String(entity.selected), String(entity.health ?? -1), String(entity.mana ?? -1),
                String(entity.animation.frame), entity.animation.sequence ?? "",
              ])
            var scale = entity.renderScale ??
                WarcraftCategoryScale.scale(category: entity.category, footprint: entity.footprint)
            if entity.renderScale == nil {
                if scene.coordinateSpace == .terrainGrid, let terrain = scene.terrain {
                    scale = WarcraftVector3(x: scale.x * terrain.cellSize * 2.5, y: scale.y * 0.08,
                                            z: scale.z * terrain.cellSize * 2.5)
                } else {
                    scale = WarcraftVector3(x: min(scale.x, 2) * 0.06, y: scale.y * 0.08,
                                            z: min(scale.z, 2) * 0.06)
                }
            }
            let overlayScale = entity.overlayScale ?? scale
            if placeholder {
                scale = WarcraftVector3(
                    x: max(overlayScale.x, 0.02), y: max(max(overlayScale.x, overlayScale.z), 0.02),
                    z: max(overlayScale.z, 0.02))
            }
            return WarcraftRenderEntityDescriptor(
                descriptor: entity, model: model, position: position,
                scale: scale, overlayScale: overlayScale,
                teamColor: WarcraftTeamPalette.color(entity.teamColor),
                animation: WarcraftAnimationSelector.resolve(entity.animation, sequences: model.sequences),
                geometryKey: geometryKey, materialKey: materialKey,
                stateKey: WarcraftContentHash.hash(stateData), usedPlaceholder: placeholder)
        }

        return WarcraftRenderSnapshot(generation: scene.generation, terrainChunks: chunks, fog: fog,
                                      entities: entities, diagnostics: Array(Set(diagnostics)).sorted())
    }
}

enum WarcraftOverlayMesh {
    static func selectionRing(segments: Int = 32) throws -> WarcraftMeshPartDescriptor {
        guard segments >= 3 else { throw WarcraftDescriptorError.invalidMesh("selection ring needs three segments") }
        var positions: [WarcraftVector3] = [], normals: [WarcraftVector3] = []
        var uvs: [WarcraftVector2] = [], indices: [UInt32] = []
        for index in 0..<segments {
            let angle = Float(index) * 2 * .pi / Float(segments)
            for radius: Float in [0.42, 0.5] {
                positions.append(WarcraftVector3(x: cos(angle) * radius, y: 0, z: sin(angle) * radius))
                normals.append(WarcraftVector3(x: 0, y: 1, z: 0))
                uvs.append(WarcraftVector2(x: radius == 0.42 ? 0 : 1, y: Float(index) / Float(segments)))
            }
        }
        for index in 0..<segments {
            let next = (index + 1) % segments
            let a = UInt32(index * 2), b = UInt32(next * 2)
            indices += [a, b, b + 1, a, b + 1, a + 1]
        }
        return WarcraftMeshPartDescriptor(name: "selection-ring", positions: positions, normals: normals,
                                           textureCoordinates: uvs, indices: indices, materialIndex: 0)
    }
}

extension JSONEncoder {
    static var sorted: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }
}
