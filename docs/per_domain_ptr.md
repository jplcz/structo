<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `per_domain_ptr<Tag, T>`

`include/structo/arch/per_domain_ptr.hpp`

A `per_cpu_ptr<Tag, T>`-shaped (see [`per_cpu_ptr.md`](per_cpu_ptr.md))
resolver, but keyed by an abstract "domain" instead of a CPU: some other
small, fixed-size isolation boundary a kernel/monitor partitions state
by. Canonical examples are Arm's Realm Management Extension World ID
(Root / Secure / Realm / Normal), classic Arm TrustZone's
Secure/Non-secure worlds, a RISC-V PMP/TEE domain, or a hypervisor's
VM/guest id -- anything shaped like "which isolated execution context is
this", resolved and indexed entirely by a caller-supplied `Tag`.

Like `per_cpu_ptr` (and unlike `per_thread_ptr`), the domain set is
small and fixed, and monitor/hypervisor code that mediates between
domains routinely needs to resolve or poke a *foreign* domain's slot --
so `per_domain_ptr` mirrors `per_cpu_ptr`'s shape exactly: both an
explicit `get(domain)`/`set(domain, ptr)` accessor and a "current
domain" `get()`/`set(ptr)` fast path.

This class owns no storage itself -- `Tag` supplies the real per-domain
container (a flat array indexed by domain id, a field inside each
domain's saved context structure, etc.) via `get_ptr()`/`set_ptr()`, and
either a dedicated `get_current_ptr()`/`set_current_ptr()` fast path or
a `current()` domain-id resolver `per_domain_ptr` combines with those
accessors. See the header's `@file` block for the full `Tag` contract
and a complete example `Tag` modeled on Arm RME World IDs.

See also: [`per_cpu_ptr.md`](per_cpu_ptr.md), [`per_thread_ptr.md`](per_thread_ptr.md).
