// FivebitWebRTCApp.swift — SwiftUI App entry point
// WebRTC video calling powered by 5bit C signaling server.
// Build: open in Xcode, add GoogleWebRTC via SPM/CocoaPods, run on device.
// Signaling: WebSocket → 5bit C binary (fivebit_webrtc.c)
// Media: native WebRTC (GoogleWebRTC iOS SDK)

import SwiftUI

@main
struct FivebitWebRTCApp: App {
    var body: some Scene {
        WindowGroup {
            JoinView()
                .preferredColorScheme(.dark)
        }
    }
}
