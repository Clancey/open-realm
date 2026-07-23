enum TabletopLauncherState: Equatable {
    case waiting
    case opening
    case open
    case failed(String)
}

enum TabletopLauncherEvent {
    case launchRequested
    case openSucceeded
    case openCancelled
    case openFailed(String)
    case retryRequested
}

enum TabletopLauncherReducer {
    static func reduce(_ state: TabletopLauncherState, _ event: TabletopLauncherEvent) -> TabletopLauncherState {
        switch event {
        case .launchRequested where state == .waiting, .retryRequested:
            return .opening
        case .openSucceeded where state == .opening:
            return .open
        case .openCancelled where state == .opening:
            return .failed("Opening the immersive space was cancelled. Choose Retry when you are ready.")
        case .openFailed(let message) where state == .opening:
            return .failed(message)
        default:
            return state
        }
    }
}
