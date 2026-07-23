struct TabletopBoardLayout: Equatable {
    var columns: Int
    var rows: Int
    var cellSize: Float
}

enum TabletopPlacement {
    static func clamp(_ position: TabletopVector3, to layout: TabletopBoardLayout) -> TabletopVector3 {
        TabletopVector3(x: min(max(position.x, 0), Float(layout.columns - 1)),
                        y: position.y,
                        z: min(max(position.z, 0), Float(layout.rows - 1)))
    }

    static func worldPosition(_ position: TabletopVector3, in layout: TabletopBoardLayout) -> TabletopVector3 {
        let p = clamp(position, to: layout)
        return TabletopVector3(x: (p.x - Float(layout.columns - 1) * 0.5) * layout.cellSize,
                               y: p.y,
                               z: (p.z - Float(layout.rows - 1) * 0.5) * layout.cellSize)
    }
}
