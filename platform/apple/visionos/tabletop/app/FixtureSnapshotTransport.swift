actor FixtureSnapshotTransport: TabletopSnapshotTransport {
    private var pollCount = 0

    func poll() async throws -> TabletopSnapshot {
        let generation = UInt64(pollCount / 6)
        pollCount += 1
        return FixtureSnapshotSource.snapshot(generation: generation)
    }
}

enum FixtureSnapshotSource {
    static let side = 7

    static func snapshot(generation: UInt64) -> TabletopSnapshot {
        let terrain = (0..<(side * side)).map { id -> TabletopTerrainTile in
            let x = id % side, z = id / side
            let water = x == side - 1 || z == side - 1
            return TabletopTerrainTile(id: id, x: x, z: z, elevation: water ? -0.03 : 0,
                                       kind: water ? .water : ((x + z) % 5 == 0 ? .dirt : .grass))
        }
        let phase = Float(generation % 12) / 12
        let entities = [
            TabletopEntitySnapshot(id: 1, kind: .worker,
                position: TabletopVector3(x: 1.5 + phase * 2, y: 0, z: 2), heading: 0, selected: false),
            TabletopEntitySnapshot(id: 2, kind: .soldier,
                position: TabletopVector3(x: 4, y: 0, z: 3.5), heading: phase * 6.283185, selected: true),
            TabletopEntitySnapshot(id: 3, kind: .building,
                position: TabletopVector3(x: 2, y: 0, z: 4.5), heading: 0, selected: false),
        ]
        return TabletopSnapshot(generation: generation, terrain: terrain, entities: entities)
    }
}
