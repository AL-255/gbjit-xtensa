/* Per-block dispatcher counters and periodic-status logs are
 * compile-time gated by DEBUG so the production board firmware doesn't
 * pay for them. The host build and the port/esp32s3 bench harness both
 * define DEBUG (their main()s read .blocks_compiled / .chain_hits /
 * .blocks_executed); the board firmwares under boards/ do not. */

#ifndef GBJIT_DEBUG_H
#define GBJIT_DEBUG_H

#ifdef DEBUG
/* `p` is a pointer to a struct, `f` is the u64 counter field name. */
#define GBJIT_STAT_INC(p, f) (++((p)->f))
#else
#define GBJIT_STAT_INC(p, f) ((void)0)
#endif

#endif
