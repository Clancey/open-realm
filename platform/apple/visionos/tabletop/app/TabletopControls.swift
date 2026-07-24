enum TabletopActionTarget: UInt32, Equatable, Sendable {
    case none
    case point
    case entity
    case entityOrPoint

    var acceptsEntity: Bool { self == .entity || self == .entityOrPoint }
    var acceptsPoint: Bool { self == .point || self == .entityOrPoint }
}

enum TabletopActionSemantic: UInt32, Equatable, Sendable {
    case unsupported
    case button
    case cancel
}

struct TabletopActionButtonSnapshot: Equatable, Sendable, Identifiable {
    var imageIndex: UInt32
    var tooltip: String
    var actionCode: String
    var hotkey: Int8
    var gridX: UInt8
    var gridY: UInt8
    var hidden: Bool
    var disabled: Bool
    var cooldown: Float
    var target: TabletopActionTarget
    var semantic: TabletopActionSemantic

    var id: String { "\(gridX):\(gridY):\(semantic.rawValue):\(actionCode)" }
}

struct TabletopActionLayoutSnapshot: Equatable, Sendable {
    var present = false
    var visible = false
    var valid = false
    var currentTarget = TabletopActionTarget.none
    var buttons: [TabletopActionButtonSnapshot] = []
}

struct TabletopEntityHit: Equatable, Sendable {
    var entityID: UInt64
    var generation: UInt64
    var sessionID: UInt64
}

enum TabletopSelectionMode: Equatable, Sendable {
    case replacement
    case additive
}

enum TabletopManipulationOwner: Equatable, Sendable {
    case leftHand
    case twoHand
}

enum TabletopInteractionMode: Equatable, Sendable {
    case idle
    case selecting
    case smartPoint
    case abilityTarget(TabletopActionTarget)
    case boardManipulation(TabletopManipulationOwner)
    case cancelling
}

struct TabletopInteractionState: Equatable, Sendable {
    private(set) var mode = TabletopInteractionMode.idle

    mutating func reconcile(authoritativeTarget: TabletopActionTarget) {
        guard case .boardManipulation = mode else {
            mode = authoritativeTarget == .none ? .idle : .abilityTarget(authoritativeTarget)
            return
        }
    }

    mutating func beginSelection() -> Bool { begin(.selecting) }
    mutating func beginSmartPoint() -> Bool { begin(.smartPoint) }
    mutating func beginBoardManipulation(_ owner: TabletopManipulationOwner) -> Bool {
        if mode == .boardManipulation(owner) { return true }
        return begin(.boardManipulation(owner))
    }

    mutating func requestCancel() -> Bool {
        guard case .abilityTarget = mode else { return false }
        mode = .cancelling
        return true
    }

    mutating func finishTransient() {
        switch mode {
        case .selecting, .smartPoint, .boardManipulation: mode = .idle
        default: break
        }
    }

    mutating func cancelGesture() {
        if case .boardManipulation = mode { mode = .idle }
        else if mode == .selecting || mode == .smartPoint { mode = .idle }
    }

    mutating func reset() { mode = .idle }

    private mutating func begin(_ next: TabletopInteractionMode) -> Bool {
        guard mode == .idle else { return false }
        mode = next
        return true
    }
}

struct TabletopBoardTransform: Equatable, Sendable {
    var translation = TabletopVector3(x: 0, y: 1, z: -1.1)
    var yaw: Float = 0
    var scale: Float = 1
}

struct TabletopBoardConstraints: Equatable, Sendable {
    var minTranslation = TabletopVector3(x: -1.5, y: 0.45, z: -2.5)
    var maxTranslation = TabletopVector3(x: 1.5, y: 1.8, z: -0.35)
    var minScale: Float = 0.55
    var maxScale: Float = 1.8

    func clamp(_ value: TabletopBoardTransform) -> TabletopBoardTransform {
        TabletopBoardTransform(
            translation: TabletopVector3(
                x: min(max(value.translation.x, minTranslation.x), maxTranslation.x),
                y: min(max(value.translation.y, minTranslation.y), maxTranslation.y),
                z: min(max(value.translation.z, minTranslation.z), maxTranslation.z)),
            yaw: value.yaw,
            scale: min(max(value.scale, minScale), maxScale))
    }
}

struct TabletopBoardManipulationState: Equatable, Sendable {
    private(set) var transform = TabletopBoardTransform()
    private(set) var owner: TabletopManipulationOwner?
    private var baseline = TabletopBoardTransform()
    private let constraints = TabletopBoardConstraints()

    mutating func begin(_ requestedOwner: TabletopManipulationOwner) -> Bool {
        if owner == requestedOwner { return true }
        guard owner == nil else { return false }
        owner = requestedOwner
        baseline = transform
        return true
    }

    mutating func update(_ requestedOwner: TabletopManipulationOwner,
                         translation: TabletopVector3 = TabletopVector3(x: 0, y: 0, z: 0),
                         yaw: Float = 0, magnification: Float = 1) -> Bool {
        guard owner == requestedOwner, magnification.isFinite, magnification > 0 else { return false }
        transform = constraints.clamp(TabletopBoardTransform(
            translation: TabletopVector3(
                x: baseline.translation.x + translation.x,
                y: baseline.translation.y + translation.y,
                z: baseline.translation.z + translation.z),
            yaw: baseline.yaw + yaw, scale: baseline.scale * magnification))
        return true
    }

    mutating func end(_ requestedOwner: TabletopManipulationOwner) -> Bool {
        guard owner == requestedOwner else { return false }
        owner = nil
        baseline = transform
        return true
    }

    mutating func cancel(_ requestedOwner: TabletopManipulationOwner) -> Bool {
        guard owner == requestedOwner else { return false }
        transform = baseline
        owner = nil
        return true
    }

    mutating func reset() {
        transform = TabletopBoardTransform()
        baseline = transform
        owner = nil
    }
}

enum TabletopHitValidation {
    static func entityID(_ hit: TabletopEntityHit, in snapshot: TabletopRenderSnapshot) throws -> UInt32 {
        guard hit.sessionID == snapshot.sessionID, hit.generation == snapshot.generation,
              snapshot.entities.contains(where: { $0.id == hit.entityID }),
              let id = UInt32(exactly: hit.entityID) else {
            throw TabletopTransportError.staleEntityHit(hit.entityID)
        }
        return id
    }

    static func selection(_ hit: TabletopEntityHit, mode: TabletopSelectionMode,
                          in snapshot: TabletopRenderSnapshot) throws -> [UInt32] {
        let id = try entityID(hit, in: snapshot)
        guard mode == .additive else { return [id] }
        var values = snapshot.selectedEntityIDs.filter { $0 != id }
        values.append(id)
        guard values.count <= TabletopCommandLowering.maxSelectionCount else {
            throw TabletopTransportError.invalidCommand("Additive selection exceeds the transport bound")
        }
        return values
    }
}

enum TabletopActionValidation {
    static func command(_ button: TabletopActionButtonSnapshot, layout: TabletopActionLayoutSnapshot,
                        generation: UInt64, sessionID: UInt64) throws -> TabletopCommand {
        guard layout.present, layout.visible, layout.valid,
              let current = layout.buttons.first(where: { $0.id == button.id }) else {
            throw TabletopTransportError.missingSemanticAction(button.actionCode)
        }
        guard !current.hidden, !current.disabled else {
            throw TabletopTransportError.invalidInteractionState("The authoritative action is unavailable")
        }
        switch current.semantic {
        case .button:
            guard !current.actionCode.isEmpty else {
                throw TabletopTransportError.missingSemanticAction(current.actionCode)
            }
            return .button(code: current.actionCode, observedGeneration: generation, sessionID: sessionID)
        case .cancel:
            return .cancel(observedGeneration: generation, sessionID: sessionID)
        case .unsupported:
            throw TabletopTransportError.missingSemanticAction(current.actionCode)
        }
    }
}

enum TabletopWorldMapping {
    static func enginePoint(_ local: TabletopVector3, bounds: TabletopBounds2?) throws -> (Float, Float) {
        guard let bounds else { throw TabletopTransportError.invalidInteractionState("Map bounds are unavailable") }
        let width = max(bounds.maxX - bounds.minX, 1), depth = max(bounds.maxZ - bounds.minZ, 1)
        let scale: Float = 1.08 / max(width, depth)
        return (local.x / scale + (bounds.minX + bounds.maxX) * 0.5,
                local.z / scale + (bounds.minZ + bounds.maxZ) * 0.5)
    }
}
