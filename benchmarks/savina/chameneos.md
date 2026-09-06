# savina/chameneos

Savina benchmark 9 of the "concurrency" group (Imam & Sarkar, *Savina — An Actor Benchmark
Suite*, AGERE 2014), after the Chameneos game of Kaiser & Pradat-Peyre — `chameneos` creatures
meet pairwise at a `mall` that pairs whoever arrives with whoever is waiting, for `meetings`
meetings in all.

## What it measures, and what it does not

It measures **fan-in with a fan-out of two on a single hot actor**: every creature's request lands
in the mall's mailbox and every request the mall completes sends exactly two announcements out.
The mall is the bottleneck by construction — nothing else does work — so the number is what one
actor costs to drain a mailbox that a hundred others write, and to answer each item with two sends.
`counting` is the same fan-in with one writer and no answer; `big` is all-to-all with no hot spot.

It does **not** measure balancing, because there is nothing to balance: one actor is runnable
almost all the time and a hundred are runnable briefly. Where the pools put the mall is their
decision; qb's placement puts it alone on core 0 with all the creatures on the far side, so at
`cores=2` the mall's inbound pipe is written cross-core by a hundred actors and the mapping table
says so. It does not measure the colour arithmetic either, which is two table lookups.

## Parameters

| parameter | default here | Savina | note |
|---|---|---|---|
| `chameneos` | 100 | 100 | no deviation |
| `meetings` | 200 000 | 200 000 | no deviation; 400 000 requests and 400 000 announcements per repetition |
| `cores` | 2 | n/a | mall on core 0; creature c on core `1 + c % (cores-1)` for the frameworks that place |
| `wait` | 1 (spin) | n/a | spin/park; both values are always measured and published |

## How `cores` maps to each framework

| framework | `cores=1` | `cores=2` |
|---|---|---|
| qb | the mall and all 100 creatures on VirtualCore 0; a request is a same-core pipe write and an announcement is the request event mutated and `reply()`ed | mall alone on VirtualCore 0, every creature on core 1: **every request and every announcement crosses a core**, and the mall's mailbox is one cross-core pipe with 100 writers on the far side — the fan-in hot spot the spec describes |
| CAF | `max-threads=1` | `=2`, both pinned; the work-stealing pool places the mall and the creatures — whether the mall's mailbox is written cross-core is the scheduler's decision, and it can change from run to run |
| SObjectizer | `one_thread` dispatcher | `thread_pool` with 2 pinned work threads and `fifo_t::individual`; same remark — the pool decides which thread runs the mall |
| floor | one thread with the mall and the creatures in its own ring | two pinned threads; the mall is thread 0 and every creature thread 1 — the same placement qb gets, but the 100 creatures share ONE SPSC ring into the mall, so the 100-writer fan-in is engineered out and the floor is what remains when it is |

## The verified answer

The mall numbers its meetings `k = 0, 1, …`; both creatures of meeting `k` fold `mix(k)` into
their accumulator, as a **wrapping sum**, and report it in their exit; the mall adds `mix(total
meetings reported)`. So `expected = Σ_{k<m} 2·mix(k) + mix(2m)`: a meeting announced to one
creature only, announced twice, numbered twice or reported by a creature that did not exit changes
the total. `expected_messages = 4m + 4c` — `c` starts, `2m + c` requests, `2m` announcements, `c`
exits and `c` reports — is counted at the receivers and asserted alongside.

## The measured window

Opens when the mall, having seen all `c` creatures report ready, sends the `c` starts — every
thread is running and every creature has been scheduled once — and closes when the mall has
received the hundredth report. The floor's second thread is created just before the window; its
start-up, once, is below resolution at 800 000 messages.
