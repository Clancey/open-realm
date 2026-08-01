import Foundation

struct FixtureWarcraftRenderProvider: WarcraftRenderDescriptorProvider {
    func scene(for snapshot: TabletopSnapshot) throws -> WarcraftSceneDescriptor {
        let terrain = Self.terrain()
        let categories: [WarcraftEntityCategory] = [.unit, .building, .resource, .doodad, .destructable]
        let models = Dictionary(uniqueKeysWithValues: categories.map { ($0, Self.model($0)) })
        let moving = Dictionary(uniqueKeysWithValues: snapshot.entities.map { ($0.id, $0) })
        let fixtureEntities: [(UInt64, WarcraftEntityCategory, WarcraftVector3, WarcraftFootprint, UInt8)] = [
            (1, .unit, WarcraftVector3(x: moving[1]?.position.x ?? 18, y: 0.05, z: 20),
             WarcraftFootprint(width: 1, depth: 1), 0),
            (2, .unit, WarcraftVector3(x: moving[2]?.position.x ?? 27, y: 0.05, z: 24),
             WarcraftFootprint(width: 1.15, depth: 1.15), 1),
            (3, .building, WarcraftVector3(x: 22, y: 0.05, z: 34),
             WarcraftFootprint(width: 4, depth: 4), 2),
            (4, .resource, WarcraftVector3(x: 43, y: 0.05, z: 18),
             WarcraftFootprint(width: 2, depth: 2), 4),
            (5, .doodad, WarcraftVector3(x: 47, y: 0.05, z: 37),
             WarcraftFootprint(width: 1.5, depth: 1.5), 6),
            (6, .destructable, WarcraftVector3(x: 15, y: 0.05, z: 43),
             WarcraftFootprint(width: 1.5, depth: 1.5), 8),
        ]
        let entities = fixtureEntities.map { id, category, position, footprint, team in
            WarcraftEntityDescriptor(
                id: id, assetName: "fixture-only/\(category.rawValue)", category: category,
                model: models[category], position: position,
                heading: id == 2 ? Float(snapshot.generation % 16) * .pi / 8 : 0,
                footprint: footprint, selected: id == 2,
                health: id == 4 ? nil : max(0.15, 1 - Float(snapshot.generation % 8) * 0.08),
                mana: category == .unit ? 0.65 : nil, teamColor: team,
                teamTint: WarcraftColor(red: 0.92, green: 0.92, blue: 0.92, alpha: 1),
                animation: WarcraftAnimationRequest(
                    sequence: id == 2 ? "Walk" : "Stand", frame: UInt32(snapshot.generation * 5)))
        }
        return WarcraftSceneDescriptor(
            generation: snapshot.generation, coordinateSpace: .terrainGrid,
            terrainKey: "fixture-terrain-64x64-v1", terrain: terrain,
            fog: Self.fog(generation: snapshot.generation), entities: entities,
            diagnostics: ["Fixture descriptors are test-only; no MPQ art is represented."])
    }

    static func terrain(width: Int = 64, height: Int = 64) -> WarcraftTerrainDescriptor {
        var heights = Array(repeating: Float(0), count: (width + 1) * (height + 1))
        for z in 0...height {
            for x in 0...width {
                if x > width / 2 { heights[z * (width + 1) + x] = x < width / 2 + 5 ? Float(x - width / 2) * 0.015 : 0.075 }
            }
        }
        var cells = Array(repeating: WarcraftTerrainCellDescriptor(materialIndex: 0, waterLevel: nil),
                          count: width * height)
        for z in 0..<height {
            for x in 0..<width {
                var cell = WarcraftTerrainCellDescriptor(materialIndex: (x + z) % 9 == 0 ? 1 : 0,
                                                          waterLevel: nil)
                if x >= width - 9 || z >= height - 7 { cell.waterLevel = 0.035 }
                if x == width / 2 && z % 6 != 0 { cell.features = [.cliff] }
                if x >= width / 2 && x < width / 2 + 5 && z % 6 == 0 { cell.features = [.ramp] }
                cells[z * width + x] = cell
            }
        }
        return WarcraftTerrainDescriptor(
            width: width, height: height, cellSize: 0.02, heights: heights, cells: cells,
            materials: [
                WarcraftMaterialDescriptor(name: "fixture-only-grass",
                    color: WarcraftColor(red: 0.18, green: 0.46, blue: 0.12, alpha: 1),
                    texture: nil, blendMode: .opaque),
                WarcraftMaterialDescriptor(name: "fixture-only-dirt",
                    color: WarcraftColor(red: 0.35, green: 0.22, blue: 0.1, alpha: 1),
                    texture: nil, blendMode: .opaque),
                WarcraftMaterialDescriptor(name: "fixture-only-water",
                    color: WarcraftColor(red: 0.08, green: 0.28, blue: 0.7, alpha: 0.72),
                    texture: nil, blendMode: .alpha, role: .water, unlit: true),
                WarcraftMaterialDescriptor(name: "fixture-only-cliff",
                    color: WarcraftColor(red: 0.28, green: 0.25, blue: 0.2, alpha: 1),
                    texture: nil, blendMode: .opaque),
                WarcraftMaterialDescriptor(name: "fixture-only-ramp",
                    color: WarcraftColor(red: 0.42, green: 0.34, blue: 0.18, alpha: 1),
                    texture: nil, blendMode: .opaque),
            ], waterMaterialIndex: 2, cliffMaterialIndex: 3, rampMaterialIndex: 4)
    }

    static func fog(generation: UInt64) -> WarcraftFogDescriptor {
        let side = 16
        let states = (0..<(side * side)).map { index -> WarcraftFogState in
            let x = index % side, z = index / side
            if x < 3 || z < 2 { return .hidden }
            return (x + z + Int(generation / 4)) % 5 == 0 ? .explored : .visible
        }
        return WarcraftFogDescriptor(width: side, height: side, states: states)
    }

    static func asymmetricTexture() -> WarcraftImageDescriptor {
        // Rows are authored bottom-first; normalization must put red/green above blue/yellow.
        WarcraftImageDescriptor(width: 2, height: 2, rgba8: [
            0, 0, 255, 255, 255, 255, 0, 255,
            255, 0, 0, 255, 0, 255, 0, 255,
        ], orientation: .bottomLeft)
    }

    static func model(_ category: WarcraftEntityCategory) -> WarcraftModelDescriptor {
        let body = boxPart(name: "fixture-only-\(category.rawValue)-body", material: 0,
                           width: category == .building ? 0.8 : 0.42,
                           height: category == .doodad ? 1.2 : 0.65,
                           depth: category == .building ? 0.8 : 0.42)
        let team = pyramidPart(name: "fixture-only-\(category.rawValue)-team", material: 1)
        return WarcraftModelDescriptor(
            name: "fixture-only-\(category.rawValue)-model", geosets: [body, team],
            materials: [
                WarcraftMaterialDescriptor(
                    name: "fixture-only-asymmetric", color: .white,
                    texture: asymmetricTexture(), blendMode: .opaque),
                WarcraftMaterialDescriptor(
                    name: "fixture-only-team", color: .white, texture: nil,
                    blendMode: category == .resource ? .additive : .modulate,
                    role: category == .resource ? .teamGlow : .teamColor, unlit: category == .resource),
            ],
            sequences: [
                WarcraftSequenceDescriptor(name: "Stand", firstFrame: 0, lastFrame: 39, looping: true),
                WarcraftSequenceDescriptor(name: "Walk", firstFrame: 40, lastFrame: 79, looping: true),
                WarcraftSequenceDescriptor(name: "Death", firstFrame: 80, lastFrame: 99, looping: false),
            ])
    }

    private static func boxPart(name: String, material: Int, width: Float, height: Float, depth: Float)
        -> WarcraftMeshPartDescriptor {
        let x = width * 0.5, z = depth * 0.5
        let positions = [
            WarcraftVector3(x: -x, y: 0, z: -z), WarcraftVector3(x: x, y: 0, z: -z),
            WarcraftVector3(x: x, y: height, z: -z), WarcraftVector3(x: -x, y: height, z: -z),
            WarcraftVector3(x: -x, y: 0, z: z), WarcraftVector3(x: x, y: 0, z: z),
            WarcraftVector3(x: x, y: height, z: z), WarcraftVector3(x: -x, y: height, z: z),
        ]
        let indices: [UInt32] = [
            0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
            0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
            3, 7, 6, 3, 6, 2, 0, 1, 5, 0, 5, 4,
        ]
        return WarcraftMeshPartDescriptor(
            name: name, positions: positions,
            normals: positions.map { _ in WarcraftVector3(x: 0, y: 1, z: 0) },
            textureCoordinates: positions.enumerated().map {
                WarcraftVector2(x: Float($0.offset & 1), y: Float(($0.offset >> 1) & 1))
            }, indices: indices, materialIndex: material)
    }

    private static func pyramidPart(name: String, material: Int) -> WarcraftMeshPartDescriptor {
        WarcraftMeshPartDescriptor(
            name: name, positions: [
                WarcraftVector3(x: -0.22, y: 0.58, z: -0.22), WarcraftVector3(x: 0.22, y: 0.58, z: -0.22),
                WarcraftVector3(x: 0.22, y: 0.58, z: 0.22), WarcraftVector3(x: -0.22, y: 0.58, z: 0.22),
                WarcraftVector3(x: 0, y: 0.98, z: 0),
            ], normals: Array(repeating: WarcraftVector3(x: 0, y: 1, z: 0), count: 5),
            textureCoordinates: [
                WarcraftVector2(x: 0, y: 0), WarcraftVector2(x: 1, y: 0),
                WarcraftVector2(x: 1, y: 1), WarcraftVector2(x: 0, y: 1),
                WarcraftVector2(x: 0.5, y: 0.5),
            ], indices: [0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4, 0, 3, 2, 0, 2, 1],
            materialIndex: material)
    }
}

struct ProductionWarcraftRenderProvider: WarcraftRenderDescriptorProvider {
    func scene(for snapshot: TabletopSnapshot) throws -> WarcraftSceneDescriptor {
        let assets = snapshot.warcraftAssets
        let transform = assets?.worldTransform ?? worldTransform(snapshot.coordinateSpace)
        let entities = snapshot.entities.map { entity in
            let asset = assets?.entities[entity.id]
            let metadata = asset?.metadata ?? WarcraftAssetMetadata(
                category: .unknown, classID: entity.metadata.classID,
                teamColor: UInt8(truncatingIfNeeded: entity.metadata.player), tint: .white,
                footprint: WarcraftFootprint(width: 0, depth: 0))
            let scale = max(entity.metadata.scale, 0.0001) * (transform?.scale ?? 1)
            let categoryScale = WarcraftCategoryScale.scale(
                category: metadata.category, footprint: metadata.footprint)
            let worldScale = transform?.scale ?? 1
            let overlay = WarcraftVector3(
                x: categoryScale.x * worldScale, y: categoryScale.y * worldScale,
                z: categoryScale.z * worldScale)
            return WarcraftEntityDescriptor(
                id: entity.id, assetName: asset?.identity ?? assetName(entity, configStrings: snapshot.configStrings),
                category: metadata.category, model: asset?.model,
                position: transform?.point(entity.position) ?? WarcraftVector3(
                    x: entity.position.x, y: entity.position.y, z: entity.position.z),
                heading: entity.heading, footprint: metadata.footprint,
                selected: entity.selected, health: nil, mana: nil,
                teamColor: metadata.teamColor, teamTint: metadata.tint,
                animation: WarcraftAnimationRequest(sequence: nil, frame: entity.metadata.frame),
                renderScale: WarcraftVector3(x: scale, y: scale, z: scale), overlayScale: overlay)
        }
        let fog = snapshot.fog.map { source in
            WarcraftFogDescriptor(width: Int(source.width), height: Int(source.height),
                states: zip(source.visible, source.explored).map {
                    $0 != 0 ? .visible : ($1 != 0 ? .explored : .hidden)
                })
        }
        return WarcraftSceneDescriptor(
            generation: snapshot.generation, coordinateSpace: .world, terrainKey: assets?.terrainKey,
            terrain: assets?.terrain, fog: fog, entities: entities,
            diagnostics: (assets?.diagnostics ?? [
                "Production snapshot has no copied asset descriptors; explicit placeholders are active.",
            ]) + (assets?.terrain == nil && !entities.isEmpty ? [
                "Production terrain descriptor is unavailable; entity rendering remains explicit.",
            ] : []) + (assets?.entities.values.flatMap(\.diagnostics) ?? []) +
                snapshot.entities.compactMap { entity in
                    assets?.entities[entity.id] == nil ?
                        "Entity \(entity.id) lacks authoritative asset metadata; using explicit placeholder." : nil
                })
    }

    private func assetName(_ entity: TabletopEntitySnapshot, configStrings: [UInt32: String]) -> String {
        let modelConfigStringBase: UInt32 = 32
        if let value = configStrings[modelConfigStringBase + entity.metadata.model], !value.isEmpty { return value }
        return "model:\(entity.metadata.model)"
    }

    private func worldTransform(_ coordinateSpace: TabletopCoordinateSpace) -> WarcraftWorldTransform? {
        guard case .world(let bounds) = coordinateSpace, let bounds else { return nil }
        let spanX = bounds.maxX - bounds.minX, spanZ = bounds.maxZ - bounds.minZ
        guard spanX > 0, spanZ > 0 else { return nil }
        return WarcraftWorldTransform(
            centerX: (bounds.minX + bounds.maxX) * 0.5,
            centerZ: (bounds.minZ + bounds.maxZ) * 0.5,
            scale: 1.08 / max(spanX, spanZ))
    }
}
