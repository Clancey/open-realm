import Foundation

struct WarcraftVector2: Codable, Equatable, Hashable, Sendable {
    var x: Float
    var y: Float
}

struct WarcraftVector3: Codable, Equatable, Hashable, Sendable {
    var x: Float
    var y: Float
    var z: Float
}

struct WarcraftColor: Codable, Equatable, Hashable, Sendable {
    var red: Float
    var green: Float
    var blue: Float
    var alpha: Float

    static let white = WarcraftColor(red: 1, green: 1, blue: 1, alpha: 1)
    static let placeholder = WarcraftColor(red: 1, green: 0, blue: 1, alpha: 1)
}

enum WarcraftImageOrientation: String, Codable, Equatable, Sendable {
    case topLeft
    case bottomLeft
}

struct WarcraftImageDescriptor: Codable, Equatable, Sendable {
    var width: Int
    var height: Int
    var rgba8: [UInt8]
    var orientation: WarcraftImageOrientation
}

struct WarcraftAtlasRegion: Codable, Equatable, Hashable, Sendable {
    var x: Int
    var y: Int
    var width: Int
    var height: Int
    var atlasWidth: Int
    var atlasHeight: Int
}

enum WarcraftBlendMode: String, Codable, Equatable, Sendable {
    case opaque
    case alphaKey
    case alpha
    case additive
    case addAlpha
    case modulate
    case modulate2x
}

enum WarcraftRealityMaterialKind: String, Equatable, Sendable {
    case litOpaque
    case litAlpha
    case unlitOpaque
    case unlitAlpha
    case unlitAdditive
    case litModulate
}

enum WarcraftMaterialMapping {
    static func kind(_ material: WarcraftMaterialDescriptor) -> WarcraftRealityMaterialKind {
        if material.blendMode == .additive || material.blendMode == .addAlpha {
            return .unlitAdditive
        }
        if material.unlit { return material.blendMode == .opaque ? .unlitOpaque : .unlitAlpha }
        switch material.blendMode {
        case .opaque: return .litOpaque
        case .alphaKey, .alpha: return .litAlpha
        case .additive, .addAlpha: return .unlitAdditive
        case .modulate, .modulate2x: return .litModulate
        }
    }
}

enum WarcraftMaterialRole: String, Codable, Equatable, Sendable {
    case surface
    case teamColor
    case teamGlow
    case water
    case fog
    case overlay
    case placeholder
}

struct WarcraftMaterialDescriptor: Codable, Equatable, Sendable {
    var name: String
    var color: WarcraftColor
    var texture: WarcraftImageDescriptor?
    var atlasRegion: WarcraftAtlasRegion? = nil
    var blendMode: WarcraftBlendMode
    var role: WarcraftMaterialRole = .surface
    var unlit = false
    var sourceBlendMode: UInt32? = nil
    var sourceFlags: UInt32 = 0
    var writesDepth = true
    var readsDepth = true
    var twoSided = false
    var unfogged = false
}

struct WarcraftMeshPartDescriptor: Codable, Equatable, Sendable {
    var name: String
    var positions: [WarcraftVector3]
    var normals: [WarcraftVector3]
    var textureCoordinates: [WarcraftVector2]
    var indices: [UInt32]
    var materialIndex: Int
    var vertexColors: [WarcraftColor] = []
}

struct WarcraftModelDescriptor: Codable, Equatable, Sendable {
    var name: String
    var geosets: [WarcraftMeshPartDescriptor]
    var materials: [WarcraftMaterialDescriptor]
    var sequences: [WarcraftSequenceDescriptor]
    var geometryKey: String? = nil
    var materialKey: String? = nil
}

struct WarcraftSequenceDescriptor: Codable, Equatable, Sendable {
    var name: String
    var firstFrame: UInt32
    var lastFrame: UInt32
    var looping: Bool
}

enum WarcraftEntityCategory: String, Codable, CaseIterable, Equatable, Sendable {
    case unknown
    case unit
    case building
    case resource
    case doodad
    case destructable
    case item
}

struct WarcraftFootprint: Codable, Equatable, Sendable {
    var width: Float
    var depth: Float
}

struct WarcraftAnimationRequest: Codable, Equatable, Sendable {
    var sequence: String?
    var frame: UInt32
}

struct WarcraftEntityDescriptor: Codable, Equatable, Sendable {
    var id: UInt64
    var assetName: String
    var category: WarcraftEntityCategory
    var model: WarcraftModelDescriptor?
    var position: WarcraftVector3
    var heading: Float
    var footprint: WarcraftFootprint
    var selected: Bool
    var health: Float?
    var mana: Float?
    var teamColor: UInt8
    var teamTint: WarcraftColor
    var animation: WarcraftAnimationRequest
    var renderScale: WarcraftVector3? = nil
    var overlayScale: WarcraftVector3? = nil
}

enum WarcraftTerrainFeature: String, Codable, Equatable, Sendable {
    case cliff
    case ramp
}

struct WarcraftTerrainSurfaceLayer: Codable, Equatable, Sendable {
    var materialIndex: Int
    var textureCoordinates: [WarcraftVector2]
}

struct WarcraftTerrainCellDescriptor: Codable, Equatable, Sendable {
    var materialIndex: Int
    var waterLevel: Float?
    var features: [WarcraftTerrainFeature] = []
    var waterCornerHeights: [Float]? = nil
    var waterTextureCoordinates: [WarcraftVector2]? = nil
    var waterCornerOpacities: [Float]? = nil
    var surfaceLayers: [WarcraftTerrainSurfaceLayer] = []
    var cliffMaterialIndex: Int? = nil
}

struct WarcraftTerrainDescriptor: Codable, Equatable, Sendable {
    var width: Int
    var height: Int
    var cellSize: Float
    var heights: [Float]
    var cells: [WarcraftTerrainCellDescriptor]
    var materials: [WarcraftMaterialDescriptor]
    var waterMaterialIndex: Int
    var cliffMaterialIndex: Int
    var rampMaterialIndex: Int
}

enum WarcraftFogState: UInt8, Codable, Equatable, Sendable {
    case hidden
    case explored
    case visible
}

struct WarcraftFogDescriptor: Codable, Equatable, Sendable {
    var width: Int
    var height: Int
    var states: [WarcraftFogState]
}

enum WarcraftSceneCoordinateSpace: String, Codable, Equatable, Sendable {
    case terrainGrid
    case world
}

struct WarcraftSceneDescriptor: Codable, Equatable, Sendable {
    var generation: UInt64
    var coordinateSpace: WarcraftSceneCoordinateSpace
    var terrainKey: String?
    var terrain: WarcraftTerrainDescriptor?
    var fog: WarcraftFogDescriptor?
    var entities: [WarcraftEntityDescriptor]
    var diagnostics: [String] = []
}

protocol WarcraftRenderDescriptorProvider: Sendable {
    func scene(for snapshot: TabletopSnapshot) throws -> WarcraftSceneDescriptor
}

struct WarcraftPreparedSnapshot: Sendable {
    var snapshot: TabletopSnapshot
    var render: WarcraftRenderSnapshot
}

actor TabletopPreparedSnapshotMailbox {
    private var latest: WarcraftPreparedSnapshot?
    private var scheduled = false

    func submit(_ value: WarcraftPreparedSnapshot) -> Bool {
        latest = value
        guard !scheduled else { return false }
        scheduled = true
        return true
    }

    func take() -> WarcraftPreparedSnapshot? {
        let value = latest
        latest = nil
        scheduled = false
        return value
    }

    func reset() { latest = nil; scheduled = false }
}

actor TabletopSnapshotMailbox {
    private var latest: TabletopSnapshot?
    private var processing = false

    func submit(_ value: TabletopSnapshot) -> Bool {
        latest = value
        guard !processing else { return false }
        processing = true
        return true
    }

    func take() -> TabletopSnapshot? {
        let value = latest
        latest = nil
        return value
    }

    func finish() -> Bool {
        guard latest == nil else { return true }
        processing = false
        return false
    }

    func reset() { latest = nil; processing = false }
}

actor WarcraftRenderPipeline {
    private let provider: any WarcraftRenderDescriptorProvider
    private var generation: UInt64?
    private var terrainCache = WarcraftTerrainChunkCache()

    init(provider: any WarcraftRenderDescriptorProvider) { self.provider = provider }

    func reset() { generation = nil; terrainCache.reset() }

    func prepare(_ snapshot: TabletopSnapshot) throws -> WarcraftPreparedSnapshot? {
        guard generation != snapshot.generation else { return nil }
        let scene = try provider.scene(for: snapshot)
        let chunks: [WarcraftTerrainChunkDescriptor]
        if let cached = terrainCache.chunks(for: scene.terrainKey) {
            chunks = cached
        } else {
            chunks = try scene.terrain.map(WarcraftTerrainChunkBuilder.build) ?? []
            terrainCache.insert(chunks, for: scene.terrainKey)
        }
        let render = try WarcraftSceneBuilder.build(scene, terrainChunks: chunks)
        generation = snapshot.generation
        return WarcraftPreparedSnapshot(snapshot: snapshot, render: render)
    }
}

enum WarcraftRenderProviderMode: Equatable, Sendable {
    case fixture
    case production
}

enum WarcraftRenderProviderModeResolver {
    static func resolve(environment: [String: String], runtimeMode: TabletopRuntimeMode) throws
        -> WarcraftRenderProviderMode {
        let fallback: WarcraftRenderProviderMode = runtimeMode == .fixture ? .fixture : .production
        guard let value = environment["BZ_TABLETOP_RENDER_PROVIDER"] else { return fallback }
        switch value {
        case "fixture": return .fixture
        case "production": return .production
        default:
            throw TabletopTransportError.configuration("Unknown BZ_TABLETOP_RENDER_PROVIDER '\(value)'")
        }
    }
}

enum WarcraftDescriptorError: Error, Equatable {
    case invalidTerrain(String)
    case invalidImage(String)
    case invalidMesh(String)
    case invalidFog(String)
    case cache(String)
}

struct WarcraftResolvedAnimation: Codable, Equatable, Sendable {
    var sequence: String
    var frame: UInt32
    var looping: Bool
}

enum WarcraftAnimationSelector {
    static func resolve(_ request: WarcraftAnimationRequest, sequences: [WarcraftSequenceDescriptor])
        -> WarcraftResolvedAnimation? {
        guard !sequences.isEmpty else { return nil }
        let requested = request.sequence?.lowercased()
        let sequence = sequences.first { $0.name.lowercased() == requested } ??
            sequences.first { $0.name.lowercased().hasPrefix("stand") } ?? sequences[0]
        guard sequence.lastFrame >= sequence.firstFrame else { return nil }
        let count = sequence.lastFrame - sequence.firstFrame + 1
        let frame = sequence.looping ? sequence.firstFrame + request.frame % count :
            min(max(request.frame, sequence.firstFrame), sequence.lastFrame)
        return WarcraftResolvedAnimation(sequence: sequence.name, frame: frame, looping: sequence.looping)
    }
}

enum WarcraftTeamPalette {
    private static let colors = [
        WarcraftColor(red: 0.9, green: 0.05, blue: 0.05, alpha: 1),
        WarcraftColor(red: 0.1, green: 0.25, blue: 1, alpha: 1),
        WarcraftColor(red: 0.1, green: 0.8, blue: 0.8, alpha: 1),
        WarcraftColor(red: 0.55, green: 0.15, blue: 0.75, alpha: 1),
        WarcraftColor(red: 1, green: 0.85, blue: 0.1, alpha: 1),
        WarcraftColor(red: 1, green: 0.45, blue: 0.05, alpha: 1),
        WarcraftColor(red: 0.1, green: 0.75, blue: 0.2, alpha: 1),
        WarcraftColor(red: 0.95, green: 0.35, blue: 0.65, alpha: 1),
        WarcraftColor(red: 0.6, green: 0.6, blue: 0.6, alpha: 1),
        WarcraftColor(red: 0.35, green: 0.65, blue: 1, alpha: 1),
        WarcraftColor(red: 0.1, green: 0.45, blue: 0.15, alpha: 1),
        WarcraftColor(red: 0.45, green: 0.25, blue: 0.1, alpha: 1),
    ]

    static func color(_ index: UInt8) -> WarcraftColor { colors[Int(index) % colors.count] }
}

enum WarcraftMaterialTint {
    static func roleColor(_ material: WarcraftMaterialDescriptor,
                          teamColor: WarcraftColor) -> WarcraftColor {
        let isTeam = material.role == .teamColor || material.role == .teamGlow
        return isTeam && material.texture == nil ? teamColor : material.color
    }
}

enum WarcraftCategoryScale {
    static func scale(category: WarcraftEntityCategory, footprint: WarcraftFootprint) -> WarcraftVector3 {
        let categoryScale: Float
        switch category {
        case .unknown: categoryScale = 0.72
        case .unit: categoryScale = 0.72
        case .building: categoryScale = 1
        case .resource: categoryScale = 0.9
        case .doodad: categoryScale = 0.8
        case .destructable: categoryScale = 0.86
        case .item: categoryScale = 0.72
        }
        return WarcraftVector3(x: max(footprint.width, 0.25) * categoryScale,
                               y: categoryScale,
                               z: max(footprint.depth, 0.25) * categoryScale)
    }
}
