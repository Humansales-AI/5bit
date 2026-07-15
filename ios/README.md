# 5bit WebRTC — iOS Video Calling App

End-to-end WebRTC video calls powered by the 5bit C signaling server.
Signaling is native 5bit records. Media is native WebRTC.

## Architecture

```
iPhone A ←→ 5bit C signaling server ←→ iPhone B
  WebRTC            (your Mac)           WebRTC
  (media)          ws://IP:8085          (media)
```

The signaling server runs on your Mac. Both iPhones connect via WiFi.
SDP offers/answers and ICE candidates are exchanged through the server
and stored as labeled 5bit records in the grid.

## Setup

### 1. Start the signaling server on your Mac

```bash
cd c
cc -O2 -o fivebit_webrtc fivebit_webrtc.c fivebit_grid.c -lssl -lcrypto
./fivebit_webrtc 8085 ./data_webrtc
```

Find your Mac's local IP:
```bash
ipconfig getifaddr en0   # WiFi
```

### 2. Install WebRTC dependency

```bash
cd ios/FivebitWebRTC
pod install
```

### 3. Open the project

```bash
open FivebitWebRTC.xcworkspace
```

### 4. Configure and run

- Select your iPhone as the build target
- Enter your Mac's IP in the server URL field: `ws://192.168.1.100:8085`
- Enter a room name (both phones must use the same room)
- Tap "Join Room"
- When the second phone joins, the call starts automatically

## Files

| File | Purpose |
|---|---|
| `FivebitWebRTCApp.swift` | SwiftUI app entry point |
| `JoinView.swift` | Room join screen + active call UI |
| `SignalingClient.swift` | WebSocket → C signaling server (zero deps) |
| `WebRTCClient.swift` | GoogleWebRTC wrapper (peer connection, media) |
| `Podfile` | GoogleWebRTC dependency |
| `Info.plist` | Camera/mic permissions |

## Dependencies

- iOS 16.0+
- Xcode 16+
- GoogleWebRTC (via CocoaPods)
- 5bit C signaling server running on a reachable host
