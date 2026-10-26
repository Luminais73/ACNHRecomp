# Local play over the local network

Goal (2026-10-26): the airport's **local play** (visiting another island, receiving visitors, up to 8 players)
between computers running ACNHRecomp on the same network. A virtual LAN (ZeroTier, Hamachi) counts as the same
network; Tailscale does not forward broadcasts, so a peer's address can also be typed in. Nintendo's online servers
are never contacted: only machines running this project play together, each with its own copy of the game.

On the Switch, local play is `nn::ldn` (a wireless network between consoles, no router) carrying the game's own
traffic (Pia) over UDP sockets with addresses in the LDN subnet. The game calls both as APIs the runtime implements
by name (`runtime/src/hle_ldn.cpp`, `runtime/src/hle_net.cpp`), so both can be carried over the local network: the
LDN network becomes UDP broadcasts (discovery) and a session between hosts, and the game's sockets on LDN addresses
become real UDP sockets towards the peers' real addresses.

## Steps

1. **Trace** ✅ (`NX_LDN=1`): ldn answers as a console with wireless on; every call is logged.
2. **Two instances on one machine**: networks advertised and found, joined, the game's sockets carried between them.
3. **Two machines on one network**: automatic discovery (UDP broadcast), and a typed peer address.
4. **Settings screen**: local play on/off, the peer address, this machine's name.
5. **Polish**: disconnections, several visitors, long sessions.

## What the game does (step 1, 2026-10-26, 3.0.2)

Talking to Orville at the airport: `Initialize`, `AttachStateChangeEvent`, then repeatedly, about once a second:
`GetDisconnectReason`, `GetNetworkInfo` (state Initialized), `OpenStation`, `Scan` (24 results at most), `CloseStation`.
So the game looks for islands nearby by itself while the player is at the counter.

The scan filter (0x60 bytes) starts with the intent id: local communication id `0x01006F8002326000` (ACNH's title
id), then scene id 1. The filter's flags word at +0x5c is 0x25.

Open: with no island found, Orville's dialogue stopped at "Desculpe o incômodo" (sorry for the trouble); the menu
with local play was not reached yet. Next: why (a scan result or a state the game waits for), then the host's side
(`OpenAccessPoint`, `CreateNetwork`, `SetAdvertiseData`) when inviting visitors.

## Structures (from the calls; sizes per the SDK headers' layout as reimplemented elsewhere)

- `NetworkConfig` 0x20: intent id (local communication id u64, scene id u16), channel u16, node count max u8,
  local communication version u16.
- `SecurityConfig` 0x44: security mode u16, passphrase size u16, passphrase (0x40).
- `UserConfig` 0x30: user name (0x21, UTF-8).
- `ScanFilter` 0x60, `NetworkInfo` 0x480 (network id, common info with the SSID, then the LDN part: nodes (8 x 0x40,
  IPv4, MAC, name), advertise data up to 0x180 bytes).
