// CallView.swift — SwiftUI UI for WebRTC video call via 5bit signaling
// Two views: a join/create screen, and the active call screen with video.

import SwiftUI
import WebRTC

// MARK: - Join Screen

struct JoinView: View {
    @State private var room = "default"
    @State private var serverURL = "ws://localhost:8085"
    @State private var isCallActive = false
    @StateObject private var call = CallManager()

    var body: some View {
        NavigationStack {
            ZStack {
                Color(hex: "06060d").ignoresSafeArea()
                VStack(spacing: 24) {
                    VStack(spacing: 8) {
                        Text("5bit ◆ WebRTC").font(.system(size: 28, weight: .bold)).foregroundColor(.white)
                        Text("Signaling server: C binary + 5bit grid").font(.system(size: 12)).foregroundColor(Color(hex: "666666"))
                    }
                    VStack(spacing: 16) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("SERVER").font(.system(size: 10, weight: .bold)).foregroundColor(Color(hex: "555555")).tracking(1)
                            TextField("ws://localhost:8085", text: $serverURL).padding(12).background(Color(hex: "16161f")).cornerRadius(8).foregroundColor(.white)
                        }
                        VStack(alignment: .leading, spacing: 6) {
                            Text("ROOM").font(.system(size: 10, weight: .bold)).foregroundColor(Color(hex: "555555")).tracking(1)
                            TextField("default", text: $room).padding(12).background(Color(hex: "16161f")).cornerRadius(8).foregroundColor(.white)
                        }
                        Button(action: { call.start(serverURL: serverURL, room: room); isCallActive = true }) {
                            HStack { Image(systemName: "phone.fill"); Text("Join Room").fontWeight(.semibold) }
                                .frame(maxWidth: .infinity).padding(14)
                                .background(LinearGradient(colors: [Color(hex: "6c5ce7"), Color(hex: "ff4081")], startPoint: .leading, endPoint: .trailing))
                                .foregroundColor(.white).cornerRadius(10)
                        }
                    }.padding(24).background(Color(hex: "111118")).cornerRadius(16)
                    Text("Connected to 5bit C signaling server\nSDP/ICE exchanged as labeled records").font(.system(size: 10)).foregroundColor(Color(hex: "444444")).multilineTextAlignment(.center)
                }.padding(32)
            }
            .navigationDestination(isPresented: $isCallActive) { ActiveCallView(call: call) }
        }
    }
}

// MARK: - Active Call Screen

struct ActiveCallView: View {
    @ObservedObject var call: CallManager
    @Environment(\.dismiss) var dismiss
    @State private var status = "Connecting..."

    var body: some View {
        ZStack {
            Color(.black).ignoresSafeArea()
            VStack {
                HStack {
                    Text("5bit ◆ Call").font(.system(size: 14, weight: .semibold)).foregroundColor(.white)
                    Spacer()
                    Text(call.connectionState).font(.system(size: 11)).foregroundColor(Color(hex: "e94560"))
                        .padding(.horizontal, 10).padding(.vertical, 4).background(Color(hex: "e94560").opacity(0.15)).cornerRadius(6)
                }.padding()

                VideoView(renderer: call.remoteRenderer).cornerRadius(12).padding(.horizontal, 8)
                VideoView(renderer: call.localRenderer).frame(width: 120, height: 180).cornerRadius(10)
                    .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color(hex: "2a2a3e"), lineWidth: 1))
                    .padding(.trailing, 16).frame(maxWidth: .infinity, alignment: .trailing).offset(y: -30)

                Spacer()

                HStack(spacing: 40) {
                    Button(action: { call.toggleMute() }) {
                        Image(systemName: call.isMuted ? "mic.slash.fill" : "mic.fill").font(.system(size: 20))
                            .foregroundColor(.white).frame(width: 56, height: 56)
                            .background(call.isMuted ? Color(hex: "e94560") : Color(hex: "2a2a3e")).clipShape(Circle())
                    }
                    Button(action: { call.isVideoPaused.toggle() }) {
                        Image(systemName: call.isVideoPaused ? "video.slash.fill" : "video.fill").font(.system(size: 20))
                            .foregroundColor(.white).frame(width: 56, height: 56)
                            .background(call.isVideoPaused ? Color(hex: "e94560") : Color(hex: "2a2a3e")).clipShape(Circle())
                    }
                    Button(action: { call.hangUp(); dismiss() }) {
                        Image(systemName: "phone.down.fill").font(.system(size: 22))
                            .foregroundColor(.white).frame(width: 64, height: 64)
                            .background(Color(hex: "e94560")).clipShape(Circle())
                    }
                }.padding(.bottom, 40)
            }
        }
    }
}

// MARK: - Native Video Renderer Wrapper

struct VideoView: UIViewRepresentable {
    let renderer: RTCVideoRenderer
    func makeUIView(context: Context) -> RTCMTLVideoView {
        let v = RTCMTLVideoView(); v.videoContentMode = .scaleAspectFill; return v
    }
    func updateUIView(_ uiView: RTCMTLVideoView, context: Context) {
        renderer.setRenderer(uiView)
    }
    static func dismantleUIView(_ uiView: RTCMTLVideoView, coordinator: ()) {
        uiView.renderer = nil
    }
}

// MARK: - Call Manager (ViewModel)

class CallManager: NSObject, ObservableObject {
    @Published var connectionState = "disconnected"
    @Published var isMuted = false
    @Published var isVideoPaused = false

    let localRenderer: RTCEAGLVideoView
    let remoteRenderer: RTCEAGLVideoView

    private var signaling: SignalingClient?
    private var rtc: WebRTCClient?
    private var hasRemoteSDP = false

    override init() {
        localRenderer = RTCEAGLVideoView(frame: .zero)
        remoteRenderer = RTCEAGLVideoView(frame: .zero)
        super.init()
    }

    func start(serverURL: String, room: String) {
        guard let url = URL(string: serverURL) else { return }
        signaling = SignalingClient(serverURL: url, room: room)
        signaling?.delegate = self
        signaling?.connect()

        rtc = WebRTCClient()
        rtc?.delegate = self
        rtc?.startCapture(videoView: localRenderer)
        connectionState = "connecting"
    }

    func hangUp() {
        signaling?.disconnect()
        rtc?.disconnect()
        connectionState = "disconnected"
    }

    func toggleMute() { isMuted.toggle() }

    func startCall() {
        rtc?.offer { [weak self] sdp in
            self?.signaling?.sendOffer(sdp.sdp)
        }
    }
}

extension CallManager: SignalingClientDelegate {
    func signalingClient(_ client: SignalingClient, didReceiveOffer sdp: String, from sender: String) {
        guard let rtc = rtc else { return }
        rtc.set(remoteSdp: RTCSessionDescription(type: .offer, sdp: sdp)) { _ in
            rtc.answer { [weak self] ans in
                self?.signaling?.sendAnswer(ans.sdp, target: sender)
            }
        }
    }

    func signalingClient(_ client: SignalingClient, didReceiveAnswer sdp: String, from sender: String) {
        rtc?.set(remoteSdp: RTCSessionDescription(type: .answer, sdp: sdp)) { _ in }
    }

    func signalingClient(_ client: SignalingClient, didReceiveCandidate candidate: String, from sender: String) {
        let parts = candidate.components(separatedBy: " ")
        guard let sdpLineIndex = parts.firstIndex(of: "a=candidate:").map({Int(parts[$0+1])}) ?? Int(parts[1]),
              let sdpMid = parts.firstIndex(of: "a=candidate:").map({parts[$0+2]}) ?? Optional(parts[2]) as? String else { return }
        let sdpMLineIndex = Int32(sdpLineIndex ?? 0)
        let ice = RTCIceCandidate(sdp: candidate, sdpMLineIndex: sdpMLineIndex, sdpMid: sdpMid)
        rtc?.set(remoteCandidate: ice)
    }

    func signalingClient(_ client: SignalingClient, peerJoined peerId: String) {
        if peerId != client.peerId { connectionState = "peer joined"; startCall() }
    }

    func signalingClient(_ client: SignalingClient, peerLeft peerId: String) {
        connectionState = "peer left"
    }
}

extension CallManager: WebRTCClientDelegate {
    func webRTCClient(_ client: WebRTCClient, didGenerate candidate: RTCIceCandidate) {
        signaling?.sendCandidate("\(candidate.sdp)")
    }

    func webRTCClient(_ client: WebRTCClient, didCreateLocalSession sdp: RTCSessionDescription, isOffer: Bool) {
        if isOffer { signaling?.sendOffer(sdp.sdp) }
        else { signaling?.sendAnswer(sdp.sdp) }
    }

    func webRTCClient(_ client: WebRTCClient, didReceiveRemoteVideo track: RTCVideoTrack) {
        DispatchQueue.main.async { track.add(self.remoteRenderer) }
    }

    func webRTCClient(_ client: WebRTCClient, didChange state: RTCIceConnectionState) {
        DispatchQueue.main.async {
            switch state {
            case .connected: self.connectionState = "connected"
            case .disconnected: self.connectionState = "disconnected"
            case .failed: self.connectionState = "failed"
            default: self.connectionState = "\(state)"
            }
        }
    }
}

// MARK: - Helpers

extension Color {
    init(hex: String) {
        let hex = hex.trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
        var int: UInt64 = 0
        Scanner(string: hex).scanHexInt64(&int)
        let r, g, b: UInt64
        switch hex.count {
        case 6: (r, g, b) = ((int >> 16) & 0xFF, (int >> 8) & 0xFF, int & 0xFF)
        default: (r, g, b) = (0, 0, 0)
        }
        self.init(.sRGB, red: Double(r)/255, green: Double(g)/255, blue: Double(b)/255, opacity: 1)
    }
}
