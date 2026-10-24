# Signed ip redirects

Some titles dial a hard-coded IPv4 address instead of a name, so DNS redirection never sees them.
Brawlhalla's user server is one: the game calls `inet_aton` and then `connect`. For those titles,
the signed ceiling carries an optional `ip_redirects` section. Harbor rewrites the destination of
a listed address:port, and only that one, to the target the signed file names.

## Format

The section sits in the ceiling payload, next to `platforms`, signed with the same key and fetched,
cached, verified and rollback-protected the same way:

```json
"ip_redirects": {
  "switch": [
    { "from": "192.0.2.7:23001", "to": "203.0.113.10:22690", "proto": "tcp",
      "title": "0100C6800B934000" }
  ]
}
```

- `from` and `to` are a dotted-quad IPv4 address with a port from 1 to 65535. The address has no
  leading zeros and is never a name.
- `proto` is `tcp` (a stream connect) or `udp` (connect or sendto on a datagram socket).
- `title` (16 hex digits, optional here) is the program the rule is for. A console needs it: its
  `bsd:u` MITM takes over only the sessions of a listed program, because every command it does
  not rewrite is forwarded synchronously. Harbor checks its shape and applies the rule to every
  process.
- A platform lists each `from` at most once per protocol. One malformed or duplicate entry rejects
  the whole file, the same as a malformed family.
- The section is optional. A client that predates it ignores it, because unknown fields are
  ignored (docs/signed-ceiling.md).

## What Harbor does

- `bsd` asks `Service::Sockets::IpRedirect::For` (`bsd_ip_redirect.h`) before every guest
  `connect` and every addressed `sendto`. The OpenPak side (`service_hooks.cpp`) answers from the
  process ceiling (`NetworkProfile::IpRedirectFor`). It answers nothing while OpenPak is off.
- A rewritten connect is logged once as `bsd: connect a:p -> b:q (ip redirect)`.
- The guest keeps seeing the address it dialled. `getpeername` reports it, and on a datagram
  socket a reply from the target is reported by `recvfrom` as coming from the original address.
- The effective-redirect digest (the restart notice) gains one `ip:<proto>:<from>=<to>` line per
  rule, so a platform without rules keeps its digest.

Real consoles get the same rule from an Atmosphere `bsd:u` MITM in OpenPak's boot package
(switch-patcher).
