import AVFoundation
import OpenRealmTabletopBridge

actor LiveTabletopAudio {
    private struct Entry {
        let player: AVAudioPlayer
        var intentionallyPaused = false
    }
    private var players: [Entry] = []
    private var bytes = [UInt8](repeating: 0, count: Int(BZ_TT_AUDIO_MAX_BYTES))
    private var started = false

    func start() throws {
        guard !started else { return }
#if targetEnvironment(simulator)
        guard BZ_TTAudio_Configure(UInt32(BZ_TABLETOP_AUDIO_ABI_VERSION), BZ_TT_AUDIO_DUMMY) else {
            throw TabletopTransportError.runtime("Simulator dummy audio sink initialization failed")
        }
#else
        guard BZ_TTAudio_Configure(UInt32(BZ_TABLETOP_AUDIO_ABI_VERSION), BZ_TT_AUDIO_DEVICE) else {
            throw TabletopTransportError.runtime("AVFoundation audio sink initialization failed")
        }
#endif
        started = true
    }

    func drain() throws {
        guard started else { return }
        players.removeAll {
            !TabletopAudioLifetime.shouldRetain(
                isPlaying: $0.player.isPlaying, intentionallyPaused: $0.intentionallyPaused)
        }
#if !targetEnvironment(simulator)
        while true {
            var size = 0
            let copied = bytes.withUnsafeMutableBytes {
                BZ_TTAudio_Dequeue(
                    UInt32(BZ_TABLETOP_AUDIO_ABI_VERSION), $0.baseAddress, $0.count, &size)
            }
            guard copied else { break }
            do {
                let player = try AVAudioPlayer(data: Data(bytes: bytes, count: size))
                guard player.prepareToPlay(), player.play() else {
                    throw TabletopTransportError.runtime("AVFoundation rejected a decoded Warcraft sound")
                }
                players.append(Entry(player: player))
            } catch {
                throw TabletopTransportError.runtime("Native Warcraft audio playback failed: \(error)")
            }
        }
#endif
    }

    func suspend() {
        for index in players.indices where players[index].player.isPlaying {
            players[index].player.pause()
            players[index].intentionallyPaused = true
        }
    }

    func resume() {
        for index in players.indices where players[index].intentionallyPaused {
            if !players[index].player.play() {
                FileHandle.standardError.write(Data("OpenRealmTabletop: AVFoundation failed to resume audio\n".utf8))
            }
            players[index].intentionallyPaused = false
        }
    }

    func stop() {
        players.forEach { $0.player.stop() }
        players.removeAll()
        started = false
        _ = BZ_TTAudio_Configure(UInt32(BZ_TABLETOP_AUDIO_ABI_VERSION), BZ_TT_AUDIO_DUMMY)
    }
}
