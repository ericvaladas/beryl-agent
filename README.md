# beryl-agent

A DLL that injects into Dark Ages, hooks the game's `send` and `recv` functions with Microsoft Detours, and relays packets between the game and a connected client over a WebSocket.

Targets Dark Ages **7.41**. Other versions may have different memory offsets and are not supported.

## Installation

Download `wininet.dll` from the [latest release](../../releases/latest) and copy it into the Dark Ages game directory. Dark Ages will load it automatically on startup.

To build from source instead, see below.

## Prerequisites

Install MinGW-w64:

```bash
brew install mingw-w64              # macOS
sudo apt install mingw-w64 make     # Linux
```

## Build

```bash
make deps    # clone Detours (first time only)
make         # produces build/wininet.dll
```

## Other targets

```bash
make clean      # remove build artifacts
make distclean  # also remove cloned dependencies
```

## Authorization

When a new client tries to connect, the agent shows an in-game dialog asking whether to allow connections from that origin. Approved origins are saved to `%LOCALAPPDATA%\beryl\settings.json` and auto-approved on future connections.

## Acknowledgements

The approach for locating the game's `send`, `recv`, and walk functions was derived from [wren11/da](https://github.com/wren11/da).

## License

Licensed under the GNU General Public License v2. See [LICENSE](LICENSE).

Third-party components and their licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
