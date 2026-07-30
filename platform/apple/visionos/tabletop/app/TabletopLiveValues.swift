struct TabletopEntityMetadata: Equatable, Sendable {
    var classID: UInt32 = 0
    var rotation = TabletopVector3(x: 0, y: 0, z: 0)
    var scale: Float = 0
    var radius: Float = 0
    var player: UInt32 = 0
    var model: UInt32 = 0
    var model2: UInt32 = 0
    var image: UInt32 = 0
    var sound: UInt32 = 0
    var frame: UInt32 = 0
    var event: UInt32 = 0
    var flags: UInt16 = 0
    var renderFX: UInt8 = 0
    var ability: UInt8 = 0
    var splat: UInt32 = 0
    var shadow: UInt32 = 0
    var shadowRect: UInt32 = 0

    var hasClassIdentity: Bool { classID != 0 }
}

struct TabletopPlayerSnapshot: Equatable, Sendable {
    var number: UInt32
    var team: UInt32
    var color: UInt32
    var race: UInt32
    var uiFlags: UInt32
    var clientUIState: UInt32
    var selectedEntity: UInt32
    var startLocation: Int32
    var gold: UInt32
    var lumber: UInt32
    var foodUsed: UInt32
    var foodCap: UInt32
    var heroTokens: UInt32
    var name: String
    var target: TabletopActionTarget = .none
    var gameResult: TabletopGameResult = .none
}

struct TabletopFogSnapshot: Equatable, Sendable {
    var width: UInt32
    var height: UInt32
    var visible: [UInt8]
    var explored: [UInt8]
}

struct TabletopCommandButtonSnapshot: Equatable, Sendable {
    var art: String
    var tooltip: String
    var ubertip: String
    var command: String
    var hotkey: Int8
    var gridX: UInt8
    var gridY: UInt8
    var research: Bool
    var active: Bool
}

struct TabletopInventoryItemSnapshot: Equatable, Sendable {
    var art: String
    var tooltip: String
    var ubertip: String
    var slot: UInt8
}

struct TabletopQueueItemSnapshot: Equatable, Sendable {
    var art: String
    var entityID: UInt32
}

struct TabletopUnitLayoutSnapshot: Equatable, Sendable {
    var entityID: UInt32
    var buttons: [TabletopCommandButtonSnapshot]
    var inventory: [TabletopInventoryItemSnapshot]
    var queue: [TabletopQueueItemSnapshot]
}
