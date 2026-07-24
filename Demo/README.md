# Presentation build — on-demand irrigation / fertigation cycle

Standalone copies of the two ESP32 firmwares, used to **show a complete service cycle on demand**
instead of waiting for the schedule window and a dry column. Everything the audience sees on the LCD —
the pre-run receipt, the live stage screen, the result receipt and the error table — is the **production
UI**; it is present in the real firmware too. Only *when* a run may start (and one nutrient's minimum
dose) differs here.

```
Demo/
  ESP1-Force/    copy of ESP1/src/main.cpp + the #if FORCE_BUILD blocks
  ESP2-Force/    copy of ESP2/src/main.cpp (currently identical to production)
```

## What differs from production

All of it is inside clearly-marked `#if FORCE_BUILD` blocks in `ESP1-Force/src/main.cpp`:

1. **Run Cycle Now** (Settings row) — pick the column and FERTIGATE/IRRIGATE, press ENTER, and the cycle
   starts immediately. Bypasses the service window, the soil-moisture threshold, the once-per-day stamp
   and the NPK gap.
2. **Run Mode** (Settings row) — `RIG` drives the real hardware (a genuine forced run); `OFFLINE` plays
   the whole cycle with **no ESP2 and no actuators**, synthesizing plausible water, nutrient and N-P-K
   movement. `OFFLINE` is the default so the board is safe on a table.
3. **`RUNDEMO,COL_x[,FERT|IRR]` SMS** — same thing from a phone (owner-gated, like every other command).
4. **Nutrient A (CALCINIT)** always doses at least the minimum resolvable amount (`MIN_DOSE_PULSES`,
   ~12 mL) so the nutrient stage is observable. Production keeps the honest gap gate, which resolves to
   zero for these crops — that is deliberate, so the thesis N-P-K data stays unbiased.

`ESP2-Force/` is currently byte-identical to `ESP2/`: the stage reporting the run screen depends on
(`STAGE` / `PROG`) belongs in the real firmware. It exists so ESP2 timings can be tweaked for a
presentation without touching production. **It is only needed in `RIG` mode.**

## Build & flash

```bash
cd Demo/ESP1-Force && pio run -e esp32dev -t upload     # set upload_port first
cd Demo/ESP2-Force && pio run -e esp32dev -t upload     # only needed for RIG mode
```

Flash `ESP1-Force` **instead of** `ESP1`. To go back to normal operation, re-flash `ESP1/`.

## Running it

1. Settings → **Run Mode** → `OFFLINE` (table) or `RIG` (plumbed rig).
2. Settings → **Run Cycle Now** → UP/DOWN pick the column + FERTIGATE/IRRIGATE → **ENTER**.
3. The **pre-run receipt** shows the targets and the plan. ENTER starts it; it also auto-continues after
   20 s so an unattended run never stalls.
4. The **live stage screen** walks the real sequence — Transfer Water → Pump Nutrients → Mixing →
   Check EC/pH → Release → Flush Fill → Flush Release (irrigation runs Transfer Water → Release).
5. The **result receipt** shows water delivered, and per nutrient the measured N-P-K before → after
   next to the increase expected from the millilitres actually pumped.
6. If anything went wrong, an **error table** follows, one problem per screen.

During the cycle the buttons are locked so nobody can wander into a menu. **UP+DOWN together releases
the screen** (the run keeps going — it is not an abort). **MODE+BACK** remains the emergency stop.

Every receipt and error is also written to the SD log as `RECEIPT|PRE|…`, `RECEIPT|POST|…`,
`RECEIPT|ERR|…`, so the whole cycle is recoverable from the CSV afterwards.
