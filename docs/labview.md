# LabVIEW supervision VI

The supervision runs on the PC and talks to the Arduino over USB serial (VISA).

## Front panel

| Control / indicator | Type | Role |
| --- | --- | --- |
| `COM port` | VISA resource name | Arduino serial port |
| `Tank level (%)` | Tank indicator 0–100 | level received from the Arduino |
| `Distance (cm)` | Numeric | raw sensor value |
| `Pump` | Round LED | pump state (`P`) |
| `Float switch` | Round LED | high-level switch (`F`) |
| `Mode` | Text (Auto / Manual) | controller mode (`M`) |
| `Alarm` | String + red LED | alarm code (`A`), LED on when not `NONE` |
| `Level history` | Waveform chart | level over time, with the 20 % / 85 % thresholds |
| `AUTO` / `MAN` | Buttons | sends `AUTO` / `MAN` |
| `Pump ON` / `Pump OFF` | Buttons | sends `PUMP=1` / `PUMP=0` (manual mode) |
| `RESET alarm` | Button | sends `RESET` |
| **Attack panel** | | **lab demonstration only** |
| `Bias (%)` + `Inject` | Numeric + button | sends `INJECT=<bias>` |
| `Freeze at (%)` + `Freeze` | Numeric + button | sends `STUCK=<value>` (`-1` to stop) |
| `STOP` | Button | closes the port and stops the VI |

## Block diagram

1. **Init:** `VISA Configure Serial Port` (9600 baud, 8 data bits, no parity, 1 stop bit, termination char `\n` enabled, timeout 2 s).
2. **While loop**, 100 ms:
   - **Read:** call `Bytes at Port`. If it is > 0, call `VISA Read` up to the termination character.
   - **Parse:** `Match Pattern` / `Scan From String` with the format
     `L=%f;D=%f;P=%d;F=%d;M=%s;A=%s`
     These values feed the indicators and the chart.
   - **Commands:** an event structure (or a case structure on each button's latched value) runs `VISA Write` of the command plus `\n`.
   - **Alarm:** when `A ≠ NONE`, the alarm LED is on, a beep plays, and the line is logged with a timestamp to a CSV file (`Write Delimited Spreadsheet`).
3. **Exit (STOP or error):** `VISA Close`, then `Simple Error Handler`.

## Example frames

```
L=46.3;D=17.5;P=0;F=0;M=A;A=NONE
L=19.8;D=24.6;P=1;F=0;M=A;A=NONE
L=42.0;D=18.6;P=1;F=0;M=A;A=STUCK
```
