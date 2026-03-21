# Beryl Agent

A DLL proxy (`wininet.dll`) for Dark Ages that hooks the game's send/recv functions using Microsoft Detours, exposing packet data over WebSocket.

## Prerequisites

Install MinGW-w64 (macOS):

```bash
brew install mingw-w64
```

## Build

```bash
make deps    # clone Detours (first time only)
make         # produces build/wininet.dll
```

Copy `build/wininet.dll` to the Dark Ages game directory.

## Other targets

```bash
make clean      # remove build artifacts
make distclean  # also remove cloned dependencies
```
