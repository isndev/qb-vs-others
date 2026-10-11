#!/usr/bin/env python3
"""cigsmok-schedule-model.py -- a deterministic schedule model of savina/cigsmok at two cores (Huly QB-1010).

Replays the spec's draws exactly (qvo::mix over the round number, qvospec/savina/cigsmok.h) and simulates two
schedules with MEASURED costs as inputs:

  static  the arbiter on core 0 and smoker j on core (j + 1) % 2 -- qb's placement and the floor's; --split F
          puts a fraction F of the smokers on core 1 instead (the form question for the adapter and the floor).
          A core-0 smoke holds the arbiter until it ends (same thread); a core-1 smoke overlaps the arbiter's next
          decision, and a round sent to core 1 while it still smokes waits in its ring.
  pool    two threads; a runnable agent is taken by whichever thread is free first (SObjectizer's thread_pool with
          fifo_t::individual; max_demands_at_once is irrelevant at one demand per agent per round).

Inputs, ns:
  --iter   one spin_work iteration: from the 1c cell, (t_1c - msg_1c) / 510 (the mean period is 509.6 iterations)
  --hop    one cross-core hop in qb: half the ping-pong 2c-spin round trip (~82 on WSL2, ~100 on Windows)
  --local  a same-core turnaround: send into the self pipe, then the next pass (~15-25)
  --handler the arbiter's or smoker's handler body without the smoke (~20)
  --pool-hop one pool hand-off (a global-queue lock, a per-agent spinlock, the thread's wake): from SObjectizer's
          own 1c cell, (t_1c_so - 510 * iter) / 2 is a first estimate

Output: ns per round for each schedule beside the measured 2c-spin cells (results/<host>/savina-cigsmok/*.json
when --results is given). A model whose inputs are measurements, to say whether placement alone accounts for the
cell -- not a measurement. Phase B of QB-1010 runs it; phase A wrote it.
"""
import argparse
import json
import os

M64 = (1 << 64) - 1


def mix(x):
    x &= M64
    x ^= x >> 33
    x = (x * 0xFF51AFD7ED558CCD) & M64
    x ^= x >> 33
    x = (x * 0xC4CEB9FE1A85EC53) & M64
    x ^= x >> 33
    return x


K_CHOICE = 0xC165C0C000000001
K_PERIOD = 0xC165C0C000000002


def draws(rounds, smokers, smoke):
    for r in range(rounds):
        yield mix(K_CHOICE + r) % smokers, mix(K_PERIOD + r) % smoke + 10


def static_schedule(rounds, smokers, smoke, iter_ns, hop, local, handler, core_of):
    """Returns (ns per round, rounds on core 1, rounds that waited behind a core-1 smoke)."""
    t = 0.0          # the arbiter sends round r at t
    busy = [0.0, 0.0]  # each core's handler sequence ends here
    on1 = waited = 0
    for s, p in draws(rounds, smokers, smoke):
        d = p * iter_ns
        if core_of(s) == 1:
            on1 += 1
            arrive = t + hop
            start = max(arrive, busy[1])
            if busy[1] > arrive:
                waited += 1
            ack_at = max(start + handler + hop, busy[0])  # the arbiter's core must be free to route it
            busy[1] = start + handler + d
            t = ack_at + handler
        else:
            start = max(t + local, busy[0])
            busy[0] = start + handler + d           # the ack sits in the self pipe behind the smoke
            t = busy[0] + local + handler
    return t / rounds, on1, waited


def pool_schedule(rounds, smokers, smoke, iter_ns, hop, handler):
    """Two threads; the smoker's demand goes to the first free thread, the arbiter's to the first free one after
    the acknowledgement, which is the OTHER thread while the smoke runs."""
    t = 0.0
    free = [0.0, 0.0]
    for _, p in draws(rounds, smokers, smoke):
        d = p * iter_ns
        i = 0 if free[0] <= free[1] else 1
        start = max(t + hop, free[i])
        free[i] = start + handler + d
        j = 1 - i
        ack_run = max(start + handler + hop, free[j])
        free[j] = ack_run + handler
        t = free[j]
    return t / rounds


def measured(results, host):
    out = {}
    for fw in ("qb", "baseline", "sobjectizer"):
        p = os.path.join(results, host, "savina-cigsmok", f"{fw}__2c-spin.json")
        if os.path.exists(p):
            d = json.load(open(p, encoding="utf-8"))
            out[fw] = d["summary"]["work_p50"] / d["work_units"]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rounds", type=int, default=50000)
    ap.add_argument("--smokers", type=int, default=200)
    ap.add_argument("--smoke", type=int, default=1000)
    ap.add_argument("--iter", type=float, required=True, help="ns per spin_work iteration")
    ap.add_argument("--hop", type=float, default=82.0)
    ap.add_argument("--local", type=float, default=20.0)
    ap.add_argument("--handler", type=float, default=20.0)
    ap.add_argument("--pool-hop", type=float, default=130.0)
    ap.add_argument("--split", type=float, action="append", default=[],
                    help="fraction of smokers on core 1 (default: the spec's (j + 1) %% 2)")
    ap.add_argument("--results", help="qb-vs-others/results")
    ap.add_argument("--host", default="wsl-debian-g++14")
    a = ap.parse_args()

    ns, on1, waited = static_schedule(a.rounds, a.smokers, a.smoke, a.iter, a.hop, a.local, a.handler,
                                      lambda s: (s + 1) % 2)
    print(f"static (j+1)%2 : {ns:8.1f} ns/round  core-1 rounds {on1}  waited behind a core-1 smoke {waited}")
    for f in a.split:
        cut = int(round(a.smokers * f))
        ns, on1, waited = static_schedule(a.rounds, a.smokers, a.smoke, a.iter, a.hop, a.local, a.handler,
                                          lambda s, cut=cut: 1 if s < cut else 0)
        print(f"static split {f:.2f}: {ns:8.1f} ns/round  core-1 rounds {on1}  waited {waited}")
    print(f"pool (2 threads): {pool_schedule(a.rounds, a.smokers, a.smoke, a.iter, a.pool_hop, a.handler):8.1f} ns/round")
    print(f"serial (1 core) : {sum(p for _, p in draws(a.rounds, a.smokers, a.smoke)) * a.iter / a.rounds + 2 * a.local + 2 * a.handler:8.1f} ns/round (smoke + two local turnarounds)")
    if a.results:
        for fw, v in measured(a.results, a.host).items():
            print(f"measured {fw:12s} 2c-spin p50 {v:8.1f} ns/round ({a.host})")


if __name__ == "__main__":
    main()
