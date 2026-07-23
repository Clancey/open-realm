enum TabletopGestureSignal {
    case began
    case changed
    case ended
    case cancelled
}

struct TabletopGestureTerminalGate {
    private enum Phase { case idle, active, terminal }
    private var phase = Phase.idle

    mutating func accept(_ signal: TabletopGestureSignal) -> Bool {
        switch signal {
        case .began:
            phase = .active
            return true
        case .changed:
            return phase == .active
        case .ended, .cancelled:
            guard phase == .active else { return false }
            phase = .terminal
            return true
        }
    }
}

struct TabletopDragState {
    private(set) var entityID: UInt64?
    private var terminalGate = TabletopGestureTerminalGate()

    mutating func begin(entityID: UInt64) -> Bool {
        self.entityID = entityID
        return terminalGate.accept(.began)
    }

    mutating func change(entityID: UInt64) -> Bool {
        guard self.entityID == entityID else { return false }
        return terminalGate.accept(.changed)
    }

    mutating func end(entityID: UInt64, cancelled: Bool = false) -> Bool {
        guard self.entityID == entityID else { return false }
        let accepted = terminalGate.accept(cancelled ? .cancelled : .ended)
        if accepted { self.entityID = nil }
        return accepted
    }
}
