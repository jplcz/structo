/* SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Frama-C/WP driver for the *actual*, shipped `reloco_ipc_offset_in_page()`
 * / `reloco_ipc_first_chunk_len()` / `reloco_ipc_available_from_in_use()`
 * helpers in reloco_ipc_ring.h -- unlike proofs/ipc_ring_offset.c (a
 * hand-written model kept only for the historical writeup in
 * proofs/README.md), this drives WP straight over the header the library
 * ships and builds.
 *
 * These three functions are the *only* overflow/buffer-safety-sensitive
 * arithmetic `reloco_ipc_try_write()`/`reloco_ipc_try_read()` (and their
 * `.hpp` `write_slices()`/`read_slices()` mirrors) perform; WP cannot
 * reason through the rest of those functions (atomics, memcpy, shared
 * struct layout), so this driver exists purely to give the header's
 * `static inline` helpers a translation unit that references them --
 * otherwise Frama-C's front end silently discards unused static
 * functions before WP ever sees them.
 *
 * Verify with (from the repository root):
 *   eval $(opam env)
 *   frama-c -cpp-extra-args="-Iinclude/structo -std=c11" -wp -wp-rte \
 *     -wp-fct reloco_ipc_offset_in_page,reloco_ipc_first_chunk_len,\
 *     reloco_ipc_available_from_in_use \
 *     proofs/ipc_ring_offset_header_driver.c
 */

#include "reloco_ipc_ring.h"

/* Referenced so the front end keeps them; never actually called. */
uint32_t (*frama_c_keep_offset_in_page)(uint64_t, uint32_t) = reloco_ipc_offset_in_page;
uint32_t (*frama_c_keep_first_chunk_len)(uint32_t, uint32_t, uint32_t) = reloco_ipc_first_chunk_len;
uint32_t (*frama_c_keep_available_from_in_use)(uint64_t, uint32_t) = reloco_ipc_available_from_in_use;
