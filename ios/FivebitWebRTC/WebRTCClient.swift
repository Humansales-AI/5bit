// WebRTCClient.swift — Wraps GoogleWebRTC for peer connection management
// Handles media capture, peer connection lifecycle, SDP negotiation, ICE.

import Foundation
import WebRTC

protocol WebRTCClientDelegate: AnyObject {
    func webRTCClient(_ client: WebRTCClient, didGenerate candidate: RTCIceCandidate)
    func webRTCClient(_ client: WebRTCClient, didCreateLocalSession sdp: RTCSessionDescription, isOffer: Bool)
    func webRTCClient(_ client: WebRTCClient, didReceiveRemoteVideo track: RTCVideoTrack)
    func webRTCClient(_ client: WebRTCClient, didChange state: RTCIceConnectionState)
}

final class WebRTCClient: NSObject {
    private let factory: RTCPeerConnectionFactory
    private let peerConnection: RTCPeerConnection
    private let mediaConstraints = RTCMediaConstraints(mandatoryConstraints: [
        kRTCMediaConstraintsOfferToReceiveAudio: kRTCMediaConstraintsValueTrue,
        kRTCMediaConstraintsOfferToReceiveVideo: kRTCMediaConstraintsValueTrue
    ], optionalConstraints: nil)
    private let audioSource: RTCAudioSource
    private let videoSource: RTCVideoSource
    private let localVideoTrack: RTCVideoTrack
    private let localAudioTrack: RTCAudioTrack
    private var remoteVideoTrack: RTCVideoTrack?
    private let videoCapturer: RTCCameraVideoCapturer

    weak var delegate: WebRTCClientDelegate?

    override init() {
        RTCInitializeSSL()
        let decoderFactory = RTCDefaultVideoDecoderFactory()
        let encoderFactory = RTCDefaultVideoEncoderFactory()
        self.factory = RTCPeerConnectionFactory(encoderFactory: encoderFactory, decoderFactory: decoderFactory)
        self.audioSource = factory.audioSource(with: RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil))
        self.videoSource = factory.videoSource()
        self.localVideoTrack = factory.videoTrack(with: videoSource, trackId: "video0")
        self.localAudioTrack = factory.audioTrack(with: audioSource, trackId: "audio0")
        self.videoCapturer = RTCCameraVideoCapturer(delegate: videoSource)

        let config = RTCConfiguration()
        config.iceServers = [RTCIceServer(urlStrings: ["stun:stun.l.google.com:19302"])]
        config.sdpSemantics = .unifiedPlan
        config.continualGatheringPolicy = .gatherContinually

        let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: ["DtlsSrtpKeyAgreement": "true"])
        self.peerConnection = factory.peerConnection(with: config, constraints: constraints, delegate: nil)
        super.init()
        self.peerConnection.delegate = self
        peerConnection.add(localAudioTrack, streamIds: ["stream0"])
        peerConnection.add(localVideoTrack, streamIds: ["stream0"])
    }

    func startCapture(videoView: RTCMTLVideoView) {
        localVideoTrack.add(videoView)

        // Start front camera
        guard let camera = RTCCameraVideoCapturer.captureDevices().first(where: { $0.position == .front })
            ?? RTCCameraVideoCapturer.captureDevices().first else { return }

        let format = RTCCameraVideoCapturer.supportedFormats(for: camera)
            .sorted { f1, f2 in
                let w1 = CMVideoFormatDescriptionGetDimensions(f1.formatDescription).width
                let w2 = CMVideoFormatDescriptionGetDimensions(f2.formatDescription).width
                return w1 < w2
            }.last ?? nil

        guard let format else { return }
        let fps = max(30, format.videoSupportedFrameRateRanges.first?.maxFrameRate ?? 30)
        videoCapturer.startCapture(with: camera, format: format, fps: Int(fps))
    }

    func renderRemoteVideo(to view: RTCMTLVideoView) {
        remoteVideoTrack?.add(view)
    }

    func stopCapture() {
        videoCapturer.stopCapture()
    }

    func offer(completion: @escaping (_ sdp: RTCSessionDescription) -> Void) {
        peerConnection.offer(for: mediaConstraints) { [weak self] sdp, error in
            guard let sdp = sdp, let self = self else { return }
            self.peerConnection.setLocalDescription(sdp) { _ in
                completion(sdp)
            }
        }
    }

    func answer(completion: @escaping (_ sdp: RTCSessionDescription) -> Void) {
        peerConnection.answer(for: mediaConstraints) { [weak self] sdp, error in
            guard let sdp = sdp, let self = self else { return }
            self.peerConnection.setLocalDescription(sdp) { _ in
                completion(sdp)
            }
        }
    }

    func set(remoteSdp: RTCSessionDescription, completion: @escaping (Error?) -> Void) {
        peerConnection.setRemoteDescription(remoteSdp, completionHandler: completion)
    }

    func set(remoteCandidate: RTCIceCandidate) {
        peerConnection.add(remoteCandidate)
    }

    func disconnect() {
        peerConnection.close()
    }
}

extension WebRTCClient: RTCPeerConnectionDelegate {
    func peerConnection(_ peerConnection: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {
        delegate?.webRTCClient(self, didGenerate: candidate)
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange state: RTCIceConnectionState) {
        delegate?.webRTCClient(self, didChange: state)
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd stream: RTCMediaStream) {
        if let videoTrack = stream.videoTracks.first {
            remoteVideoTrack = videoTrack
            delegate?.webRTCClient(self, didReceiveRemoteVideo: videoTrack)
        }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove stream: RTCMediaStream) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceGatheringState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCSignalingState) {}
    func peerConnectionShouldNegotiate(_ peerConnection: RTCPeerConnection) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd rtpReceiver: RTCRtpReceiver, streams: [RTCMediaStream]) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove rtpReceiver: RTCRtpReceiver) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didStartReceivingOn transceiver: RTCRtpTransceiver) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange localCandidate: RTCIceCandidate, remoteCandidate: RTCIceCandidate, lastReceivedMs: Int32, changeReason: String) {}
}
