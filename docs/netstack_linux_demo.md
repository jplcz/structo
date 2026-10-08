# Linux demo: the bootloader network stack on your host

`examples/netstack_linux_demo.cpp` runs `bootldr::netstack` (scheduler, SLIP or
PPP, ICMP, UDP socket) as a normal Linux process. Instead of a hardware UART
it uses a pseudo-terminal, so the Linux kernel can be hooked to the other end
and treat the demo as a machine on a point-to-point link. The demo

- answers ping,
- echoes every UDP datagram sent to port 7 (`--port N` to change),
- logs every IPv4 datagram in and out through a microfmt logger (`-v` adds a
  hexdump of each).

C++20 or newer is required; the target is only built on UNIX hosts.

## Build

```sh
cmake -S . -B build -G Ninja
ninja -C build netstack_linux_demo
```

## Usage

```
netstack_linux_demo [--ppp] [--ip A.B.C.D] [--port N] [-v] [DEVICE]
```

| Option | Meaning |
|--------|---------|
| *(none)* | SLIP, board address 192.168.7.2, gateway 192.168.7.1 |
| `--ppp` | PPP instead; the address is assigned by the host's `pppd` |
| `--ip` | board address in SLIP mode |
| `DEVICE` | use an existing tty (e.g. one end of a `socat` pair or a USB serial adapter) instead of creating a pty |

Start it first; it prints the pty to attach the host to:

```
$ ./build/examples/netstack_linux_demo -v
[...] [netdemo] [INFO ] SLIP over /dev/pts/9, UDP echo on port 7
```

Keep it running in this terminal and use a second one for the host side
(root is needed to create network interfaces). Replace `/dev/pts/9` with the
path printed above.

## Option A: SLIP with `slattach`

```sh
# Attach the SLIP line discipline to the demo's pty; this creates interface sl0.
# -L: local line (no modem control signals), -s: baud rate (irrelevant for a pty).
# If it fails with "No such device", load the driver first: sudo modprobe slip
sudo slattach -L -p slip -s 115200 /dev/pts/9 &

# Host end 192.168.7.1, peer (the demo) 192.168.7.2; the demo's MTU is 1006 bytes.
sudo ip addr add 192.168.7.1 peer 192.168.7.2/32 dev sl0
sudo ip link set sl0 mtu 1006 up
```

If you passed `--ip`, use that address as the peer. The demo's gateway
(192.168.7.1) must be the host address.

## Option B: PPP with `pppd`

```sh
# Run the demo with --ppp, then:
# 192.168.7.1:192.168.7.2 = host address : address handed to the demo over IPCP.
# noauth: the demo implements no PAP/CHAP. local: ignore modem lines.
# nodetach: stay in the foreground (Ctrl-C to disconnect).
sudo pppd /dev/pts/9 115200 192.168.7.1:192.168.7.2 noauth local nodetach
```

`pppd` creates `ppp0`; the demo logs `network up, address 192.168.7.2` once
IPCP has finished. Optionally add `ms-dns 192.168.7.1` to hand the demo a DNS
server (it is only recorded, nothing in the demo uses it).

## Try it

```sh
# ICMP: answered by netstack's built-in echo responder
ping -c3 192.168.7.2

# UDP: answered by the demo's udp_socket echo task (-u UDP, -w1 wait 1 s)
echo "hello" | nc -u -w1 192.168.7.2 7
```

The demo's terminal shows each datagram, e.g.:

```
[...] [netdemo] [INFO ] rx ICMP type 8 192.168.7.1 -> 192.168.7.2 len 84
[...] [netdemo] [INFO ] tx ICMP type 0 192.168.7.2 -> 192.168.7.1 len 84
[...] [netdemo] [INFO ] rx UDP 192.168.7.1:41234 -> 192.168.7.2:7 len 34
[...] [netdemo] [INFO ] udp echo: 6 bytes from 192.168.7.1:41234
```

Cross-check with `sudo tcpdump -i sl0 -n` (or `ppp0`) on the host.

## Using a real serial line or `socat`

To link the demo to something that is not the kernel's SLIP/PPP (a second
process, a USB serial adapter to real hardware), create the pair yourself and
pass one end as `DEVICE`:

```sh
socat -d -d pty,raw,echo=0,link=/tmp/board pty,raw,echo=0,link=/tmp/host &
./build/examples/netstack_linux_demo /tmp/board
sudo slattach -L -p slip -s 115200 /tmp/host &   # or pppd /tmp/host ...
```

## Troubleshooting

- *Nothing is answered:* the demo must be started before `slattach`/`pppd`
  and keep running; restarting it creates a new pty, so re-attach.
- *PPP never comes up:* check `pppd` is run with `noauth` and watch its
  output; the demo only accepts LCP MRU/ACCM/magic/PFC/ACFC options and
  rejects authentication.
- *Large pings fail:* the demo's MTU is 1006 (SLIP) / 1006 (PPP); keep the
  interface MTU at or below that.
