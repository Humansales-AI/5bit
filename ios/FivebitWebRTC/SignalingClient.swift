// SignalingClient.swift — WebSocket connection to 5bit signaling server
// Connects to the C binary signaling server, exchanges SDP/ICE via JSON.
// Uses native URLSessionWebSocketTask — zero third-party signaling deps.

import Foundation

protocol SignalingClientDelegate: AnyObject {
    func signalingClient(_ client: SignalingClient, didReceiveOffer sdp: String, from sender: String)
    func signalingClient(_ client: SignalingClient, didReceiveAnswer sdp: String, from sender: String)
    func signalingClient(_ client: SignalingClient, didReceiveCandidate candidate: String, sdpMLineIndex: Int32, sdpMid: String?, from sender: String)
    func signalingClient(_ client: SignalingClient, peerJoined peerId: String)
    func signalingClient(_ client: SignalingClient, peerLeft peerId: String)
}

final class SignalingClient: NSObject {
    private var webSocket: URLSessionWebSocketTask?
    private let serverURL: URL
    private let room: String
    let peerId: String
    weak var delegate: SignalingClientDelegate?
    private var isConnected = false
    private var pingTimer: Timer?
    private var reconnectTimer: Timer?
    private var shouldReconnect = true
    private static var persistentId: String = UUID().uuidString  // survive reconnects

    init(serverURL: URL, room: String, peerId: String? = nil) {
        self.serverURL = serverURL
        self.room = room
        self.peerId = peerId ?? Self.persistentId
        super.init()
    }

    func connect() {
        var comps = URLComponents(url: serverURL, resolvingAgainstBaseURL: false)!
        comps.scheme = serverURL.scheme == "https" ? "wss" : "ws"
        comps.queryItems = [URLQueryItem(name: "room", value: room),
                            URLQueryItem(name: "peer", value: peerId)]
        let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        webSocket = session.webSocketTask(with: comps.url!)
        webSocket?.resume()
        receive()
        // Keepalive ping every 15s to prevent Railway timeout
        pingTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.send(["type": "ping"])
        }
    }

    func disconnect() {
        shouldReconnect = false
        pingTimer?.invalidate(); pingTimer = nil
        reconnectTimer?.invalidate(); reconnectTimer = nil
        webSocket?.cancel(with: .normalClosure, reason: nil)
        isConnected = false
    }

    private func receive() {
        webSocket?.receive { [weak self] result in
            switch result {
            case .success(let message):
                self?.handle(message)
                self?.receive()
            case .failure:
                self?.isConnected = false
            }
        }
    }

    private func handle(_ message: URLSessionWebSocketTask.Message) {
        guard case .string(let text) = message,
              let data = text.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: String],
              let type = json["type"] else { return }

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            switch type {
            case "offer":
                if let sdp = json["sdp"] {
                    self.delegate?.signalingClient(self, didReceiveOffer: sdp, from: json["sender"] ?? "unknown")
                }
            case "answer":
                if let sdp = json["sdp"] {
                    self.delegate?.signalingClient(self, didReceiveAnswer: sdp, from: json["sender"] ?? "unknown")
                }
            case "ice-candidate", "candidate":
                if let cand = json["candidate"] ?? json["candidate"] {
                    let sdpMLineIndex = Int32(json["sdpMLineIndex"] ?? "0") ?? 0
                    let sdpMid = json["sdpMid"]
                    self.delegate?.signalingClient(self, didReceiveCandidate: cand, sdpMLineIndex: sdpMLineIndex, sdpMid: sdpMid, from: json["sender"] ?? "unknown")
                }
            case "peer-joined":
                if let pid = json["peer_id"] {
                    self.delegate?.signalingClient(self, peerJoined: pid)
                }
            case "peer-left":
                if let pid = json["peer_id"] {
                    self.delegate?.signalingClient(self, peerLeft: pid)
                }
            default: break
            }
        }
    }

    func sendOffer(_ sdp: String, target: String? = nil) {
        send(["type": target != nil ? "target-offer" : "offer", "sdp": sdp, "sender": peerId])
    }

    func sendAnswer(_ sdp: String, target: String? = nil) {
        send(["type": target != nil ? "target-answer" : "answer", "sdp": sdp, "sender": peerId])
    }

    func sendCandidate(_ sdp: String, sdpMLineIndex: Int32, sdpMid: String?) {
        var dict: [String: String] = ["type": "ice-candidate", "candidate": sdp, "sender": peerId, "sdpMLineIndex": "\(sdpMLineIndex)"]
        if let mid = sdpMid { dict["sdpMid"] = mid }
        send(dict)
    }

    private func send(_ dict: [String: String]) {
        guard let data = try? JSONSerialization.data(withJSONObject: dict),
              let text = String(data: data, encoding: .utf8) else { return }
        webSocket?.send(.string(text)) { _ in }
    }
}

extension SignalingClient: URLSessionWebSocketDelegate {
    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        isConnected = true
        print("[5bit] Connected to signaling server — room: \(room) peer: \(peerId)")
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        isConnected = false
        print("[5bit] Disconnected — reconnecting in 2s")
        guard shouldReconnect else { return }
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { [weak self] _ in
            self?.connect()
        }
    }
}
