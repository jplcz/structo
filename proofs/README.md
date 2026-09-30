# Formal proof-of-concept: Frama-C/WP on `reloco_ipc_ring.h`'s index math

This directory demonstrates that the overflow/buffer-safety arithmetic in
`reloco_ipc_ring.h` (previously only justified by prose comments plus a
runtime `RELOCO_IPC_ASSERT`) can be **machine-checked** with an
SMT-backed proof assistant. It is not wired into the library's build/CI
-- it requires [Frama-C](https://frama-c.com) (tested with 33.0
"Arsenic") and its WP plugin, an optional toolchain most contributors
won't have installed.

## What's here

- `ipc_ring_offset_header_driver.c` drives WP directly over the
  **actual, shipped** `reloco_ipc_offset_in_page()` /
  `reloco_ipc_first_chunk_len()` / `reloco_ipc_available_from_in_use()`
  helper functions in `include/structo/reloco_ipc_ring.h` -- the real
  library code, not a model. `reloco_ipc_try_write()`/`try_read()` (and
  the `.hpp` `write_slices()`/`read_slices()` mirrors) call these three
  pure, side-effect-free functions for every overflow/buffer-safety
  -sensitive computation they perform; WP cannot reason through the rest
  of those callers (atomics, `memcpy`, shared struct layout), so this
  driver's only job is to give the header's `static inline` functions a
  translation unit that references them (otherwise Frama-C's front end
  silently discards unused `static` functions before WP ever sees them).
- `ipc_ring_offset.c` is the original standalone model used to first
  work out this approach (kept for its detailed writeup below); its
  functions are copies of the same logic, now superseded as the actual
  proof target by the header driver above.

Each function carries an ACSL (`/*@ ... */`) contract stating the exact
safety property the original header's comments claim in prose.

## Running it

```sh
opam switch 5.5.1   # or any switch with frama-c + alt-ergo installed
eval $(opam env)

# Proves the real, shipped header functions:
frama-c -cpp-extra-args="-Iinclude/structo -std=c11" -wp -wp-rte \
  -wp-fct reloco_ipc_offset_in_page,reloco_ipc_first_chunk_len,reloco_ipc_available_from_in_use \
  proofs/ipc_ring_offset_header_driver.c

# Runs the original standalone model:
frama-c -wp -wp-rte proofs/ipc_ring_offset.c
```

## Result

Against the **real header** (`ipc_ring_offset_header_driver.c`), 19/20
proof obligations are discharged fully automatically (Alt-Ergo),
including every actually security/safety-relevant claim:

- `reloco_ipc_first_chunk_len()`: the `cap - physical` subtraction never
  underflows, and the two `memcpy()` calls it feeds (in both
  `try_write()`/`try_read()` and the `.hpp` `*_slices()` mirrors) never
  read/write past `payload[cap]`.
- `reloco_ipc_available_from_in_use()`: the `uint64_t -> uint32_t`
  narrowing of `available`/`cap - in_use` (after the security-boundary
  check has already rejected `in_use > cap`) is lossless.
- `reloco_ipc_offset_in_page()`: the result is always `< cap` (this
  bound -- the only consequence anything downstream actually depends on
  -- is proved; see below for the one part that isn't).

(The original standalone model, `ipc_ring_offset.c`, proves the
equivalent 21/22 goals -- one extra goal because it also contains an
explicit local `assert` re-stating the same fact, added while
diagnosing the residual timeout below.)

The one holdout, in both files, is the deep bitwise fact everything else
is built on: "`w & (cap - 1) == w % cap` for any power-of-two `cap`".
This is true and well-known, but sits outside Alt-Ergo's decidable
fragment -- confirmed experimentally (even the single *concrete* case
`w & 15 == w % 16` times out un-axiomatized; see the `PowerOfTwoMask`/
`RelocoIpcPowerOfTwoMask` axiomatic block's comment for the full
writeup). It's asserted as a **trusted ACSL `axiom`** instead of
re-derived -- the standard way to cite an established mathematical fact
without re-proving it from first principles -- and every other lemma is
a genuine, from-scratch proof built on top of it. A bit-vector-native
solver (Z3/CVC5 via why3) or an inductive Coq proof over the exponent
could close this last gap; neither is set up here.

Notably, `reloco_ipc_ring.h` itself never blindly trusts this bitwise
fact either -- `reloco_ipc_offset_in_page()` re-verifies the one
consequence it actually depends on (`physical < cap`) with a runtime
`RELOCO_IPC_ASSERT` immediately after computing the mask, in every build
(the exact `\result == idx % cap` ensures clause is the one that times
out; the `0 <= \result < cap` bound clause proves fine).

## Where the proven functions are actually used

`reloco_ipc_offset_in_page()`, `reloco_ipc_first_chunk_len()`, and
`reloco_ipc_available_from_in_use()` are not a side artifact: they are
the real functions `reloco_ipc_try_write()`/`reloco_ipc_try_read()` in
`reloco_ipc_ring.h`, and `write_slices()`/`read_slices()` in
`reloco_ipc_ring.hpp`, call to compute physical offsets and clamp
transfer lengths. There is exactly one implementation of this
arithmetic in the whole library, in both the C and C++ APIs, and it is
the one this directory verifies -- not a stand-in.
