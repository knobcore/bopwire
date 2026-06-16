# MusicChain Player

Cross-platform Flutter player for the MusicChain decentralized music network.

## Prerequisites

- Flutter 3.16+ and Dart 3.2+
- Built `libmusicchain` shared library from the `musicchain/` directory

## Setup

1. Build the native library:
   ```bash
   cd ../musicchain
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   make -j$(nproc)
   ```

2. Copy the library to the correct platform directory:
   ```bash
   # Linux
   cp build/libmusicchain.so ../musicchain_player/linux/libs/

   # macOS
   cp build/libmusicchain.dylib ../musicchain_player/macos/Frameworks/

   # Windows
   cp build/musicchain.dll ../musicchain_player/windows/libs/
   ```

3. Generate FFI bindings (requires `ffigen` and clang):
   ```bash
   cd musicchain_player
   flutter pub get
   dart run ffigen
   ```

4. Run the app:
   ```bash
   flutter run -d linux    # Linux desktop
   flutter run -d windows  # Windows desktop
   flutter run -d macos    # macOS desktop
   flutter run             # Connected Android/iOS device
   ```

## Features

- **Library** — Browse all songs on the connected node
- **Search** — Find songs by title, artist, or genre
- **Player** — Stream audio with heartbeat-based proof of play
- **Wallet** — ECDSA secp256k1 keypair, balance display, token transfers
- **Upload** — Submit new Ogg songs to the network
- **Cache** — Local audio cache with configurable size limit

## Configuration

Edit Settings → Node URL to point to your node (default: `http://localhost:9334`).
