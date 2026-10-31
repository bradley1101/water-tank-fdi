"""False Data Injection on a water tank — simulation of the attack and the defences.

The controller logic mirrors firmware/water_tank/water_tank.ino:
hysteresis 20 % / 85 %, plausibility checks (range, rate, stuck value,
float-switch cross-check) and the high-level float switch.

Scenarios
  1. Normal operation
  2. Stealthy FDI (reported level = real level - 40 %), no defence  -> overflow
  3. Same attack, with the defences                                -> float-switch mismatch, pump stopped
  4. Replay FDI (reported level frozen at its last value), with defences -> stuck-value alarm

Usage:  python fdi_simulation.py      (writes ../docs/fdi_scenarios.png)
"""
from dataclasses import dataclass, field
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DT = 0.5               # s, controller period
FILL_RATE = 3.0        # %/s when the pump runs
DRAW_RATE = 0.6        # %/s constant consumption
FLOAT_LEVEL = 95.0     # % — float switch position
LEVEL_LOW, LEVEL_HIGH = 20.0, 85.0
MAX_RISE, MAX_DROP = 4.0, 2.0
STUCK_S, STUCK_EPS = 8.0, 1.0


@dataclass
class Controller:
    defences: bool
    pump: bool = False
    alarm: str = ""
    last: float | None = None
    stuck_ref: float = 0.0
    stuck_t: float = 0.0

    def step(self, t, reported, float_full):
        if self.defences and not self.alarm:
            self._check(t, reported, float_full)
        if not self.alarm:
            if reported < LEVEL_LOW:
                self.pump = True
            elif reported > LEVEL_HIGH:
                self.pump = False
        if self.defences and float_full:
            self.pump = False
        self.last = reported
        return self.pump

    def _check(self, t, lvl, float_full):
        def trip(code):
            self.alarm, self.pump = code, False
        if not -5 <= lvl <= 105:
            return trip("RANGE")
        if self.last is not None:
            rate = (lvl - self.last) / DT
            if rate > MAX_RISE * 1.5 or rate < -MAX_DROP * 1.5:
                return trip("RATE")
        if self.pump:
            if abs(lvl - self.stuck_ref) > STUCK_EPS:
                self.stuck_ref, self.stuck_t = lvl, t
            elif t - self.stuck_t > STUCK_S:
                return trip("STUCK")
        else:
            self.stuck_ref, self.stuck_t = lvl, t
        if float_full and lvl < LEVEL_HIGH - 5:
            trip("FLOAT_MISMATCH")


@dataclass
class Result:
    t: list = field(default_factory=list)
    real: list = field(default_factory=list)
    reported: list = field(default_factory=list)
    pump: list = field(default_factory=list)
    alarm_t: float | None = None
    alarm: str = ""
    overflow_s: float = 0.0


def simulate(attack, defences, attack_start=60.0, duration=220.0, start_level=50.0):
    c = Controller(defences)
    level, t, r = start_level, 0.0, Result()
    attack_ramp = 0.0
    frozen = None
    while t < duration:
        reported = level
        if t >= attack_start:
            if attack == "bias":
                # stealthy: the bias grows slowly so the rate check does not see a jump
                attack_ramp = min(40.0, attack_ramp + 0.8 * DT)
                reported = level - attack_ramp
            elif attack == "frozen":
                # replay: the attacker keeps sending the last genuine value
                frozen = level if frozen is None else frozen
                reported = frozen
        float_full = level >= FLOAT_LEVEL
        pump = c.step(t, reported, float_full)
        if c.alarm and r.alarm_t is None:
            r.alarm_t, r.alarm = t, c.alarm
        level += ((FILL_RATE if pump else 0) - DRAW_RATE) * DT
        level = min(100.0, max(0.0, level))   # above 100 % the water spills
        r.overflow_s += DT if level >= 100.0 and pump else 0
        r.t.append(t); r.real.append(level); r.reported.append(reported); r.pump.append(pump)
        t += DT
    return r


def panel(ax, r, title, attack_start=None):
    ax.plot(r.t, r.real, color="#1f5fbf", lw=2, label="real level")
    ax.plot(r.t, r.reported, color="#d64545", lw=1.4, ls="--", label="level seen by the controller")
    ax.fill_between(r.t, 0, [8 if p else 0 for p in r.pump], color="#2e9e44", alpha=0.35, step="post", label="pump ON")
    ax.axhline(100, color="#000", lw=0.8)
    ax.text(r.t[-1], 101.5, "overflow", ha="right", fontsize=7)
    ax.axhline(FLOAT_LEVEL, color="#888", lw=0.8, ls=":")
    for y in (LEVEL_LOW, LEVEL_HIGH):
        ax.axhline(y, color="#bbb", lw=0.6)
    if attack_start is not None:
        ax.axvspan(attack_start, r.t[-1], color="#d64545", alpha=0.05)
        ax.text(attack_start + 1, 4 + 8, "attack", fontsize=7, color="#d64545")
    if r.alarm_t is not None:
        ax.axvline(r.alarm_t, color="#e0a800", lw=1.5)
        ax.text(r.alarm_t + 1, 60, f"ALARM\n{r.alarm}\npump OFF", fontsize=7, color="#a87b00")
    status = (f"OVERFLOW for {r.overflow_s:.0f} s" if r.overflow_s
              else f"max real level {max(r.real):.0f} % — no overflow")
    ax.set_title(f"{title}\n{status}", fontsize=9)
    ax.set_ylim(0, 125)
    ax.set_xlabel("time (s)", fontsize=8)
    ax.set_ylabel("level (%)", fontsize=8)
    ax.tick_params(labelsize=7)


def main():
    fig, axes = plt.subplots(2, 2, figsize=(12, 7.5), sharex=True)
    runs = [
        (simulate(None, True), "1. Normal operation", None),
        (simulate("bias", False), "2. Stealthy FDI (−40 % drift) — no defence", 60),
        (simulate("bias", True), "3. Same attack — with defences", 60),
        (simulate("frozen", True), "4. Replay / frozen value — with defences", 60),
    ]
    for ax, (r, title, a) in zip(axes.flat, runs):
        panel(ax, r, title, a)
        print(f"{title:45s} max real = {max(r.real):6.1f} %  overflow = {r.overflow_s:5.1f} s  alarm = {r.alarm or '-'}"
              + (f" at t = {r.alarm_t:.1f} s" if r.alarm_t else ""))
    axes[0, 0].legend(fontsize=7, loc="upper right")
    fig.suptitle("Water tank under False Data Injection", fontsize=12)
    fig.tight_layout()
    out = Path(__file__).resolve().parent.parent / "docs/fdi_scenarios.png"
    fig.savefig(out, dpi=130)
    print("saved", out)


if __name__ == "__main__":
    main()
