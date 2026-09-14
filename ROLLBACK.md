# KOYODA — how to go back

Four levels, cheapest first. Try them in order; you rarely need to go past
level 2.

---

## Level 0 (do this FIRST, before flashing anything new)

Keep the firmware that currently works, on your own disk. This is the only
rollback that still works if GitHub, CI, or the toolchain misbehaves.

```powershell
# Save the artifact folder you last flashed successfully
xcopy /E /I C:\Users\peemm\Downloads\Koyoda C:\Users\peemm\KOYODA-GOOD
```

And mark the matching commit so you can always find it again:

```powershell
cd C:\Users\peemm\KOYODA-C
git log --oneline -5          # find the commit you flashed and verified
git tag -a good-YYYYMMDD <commit> -m "Verified working: AI conversation + stable display"
git push origin --tags
```

To restore that firmware later — no build, no internet:

```powershell
cd C:\Users\peemm\KOYODA-GOOD
python -m esptool --chip esp32s3 --port COM11 --baud 460800 ^
  --before default_reset --after hard_reset write_flash "@flash_args"
```

---

## Level 1 — undo the mic gain change WITHOUT touching code

Both gain stages are Kconfig values, and the original values switch the new
behaviour off completely:

| Setting | Original value | Effect at that value |
|---|---|---|
| `KOYODA_MIC_ANALOG_GAIN_DB` | **24** | identical to the old hard-coded `24.0f` |
| `KOYODA_MIC_DIGITAL_GAIN_X10` | **10** | the whole gain block is removed by the preprocessor |

**Edit `sdkconfig.defaults`, not menuconfig.** CI runs `idf.py set-target`,
which regenerates `sdkconfig` from `sdkconfig.defaults` on every build, and
`sdkconfig` is gitignored - so a local menuconfig change never reaches the
firmware CI produces.

```
CONFIG_KOYODA_MIC_ANALOG_GAIN_DB=24
CONFIG_KOYODA_MIC_DIGITAL_GAIN_X10=10
```

menuconfig only works if you build locally with `idf.py build`.

This is verified, not assumed: at `10` the preprocessor emits zero lines of
the gain code, so the binary behaves exactly as it did before.

Partial back-off is usually better than full: try analog 27 or 30 with
digital 10 before giving up on the improvement entirely.

---

## Level 2 — undo one commit

```powershell
cd C:\Users\peemm\KOYODA-C
git log --oneline -10
git revert <commit>        # creates a new commit that undoes it
git push
```

`git revert` is preferred over `git reset` because it keeps history and
cannot lose work. Every change in this project is a separate commit, so a
single revert undoes exactly one thing:

- mic gain → `Raise mic gain: analog 24->33 dB ...`
- Wi-Fi page → `Wi-Fi page: stop status label overlapping ...`
- Wi-Fi page text → `Wi-Fi page: show pairing instructions ...`
- draw buffer → `Give LVGL a small DMA-capable draw buffer ...`
- backend stack → `Fix backend task stack overflow ...`

---

## Level 3 — go back to a known-good commit entirely

```powershell
git log --oneline --all
git checkout -b rollback-test <good-commit>    # branch, does not touch main
git push -u origin rollback-test
```

Build that branch in Actions and flash it. If it is good and you want it to
become main:

```powershell
git checkout main
git revert --no-commit <bad-commit>..HEAD
git commit -m "Roll back to <good-commit>"
git push
```

---

## Which settings are risky to change, and their safe values

| Setting | Safe / proven | Notes |
|---|---|---|
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | **98304** | 65536 caused the display DMA failures |
| `KOYODA_SMALL_DRAW_BUFFER` | **y** | turning it off brings the DMA failures back |
| `KOYODA_DRAW_BUFFER_LINES` | **24** | more lines = smoother but less RAM for the codec |
| `BACKEND_TASK_STACK_BYTES` | **28672** | 10240 overflowed the moment Opus ran |
| `MBEDTLS_EXTERNAL_MEM_ALLOC` | **y** | off puts TLS back in internal RAM |
| `KOYODA_ECHO_TEST` | **n** | y disables the network backend entirely |
| `KOYODA_CODEC_SELFTEST` | **n** | y only adds ~1 s to boot |
| `KOYODA_MIC_ANALOG_GAIN_DB` | **33** (24 = original) | rounds to 30 on the 3 dB grid. 36 was tried and measured WORSE: SNR fell from 17.1 to 14.7 dB because the mic's own noise scales with gain. Judge changes by SNR (speech avg / noise floor), not by peak level |
| `KOYODA_MIC_DIGITAL_GAIN_X10` | **20** (10 = off) | at 10 the code is removed by the preprocessor |
| `NEWLIB_NANO_FORMAT` | **y** | saves RAM but has no `%lld`/`%llu`; use `%lu`/`%d` |
| Mic sensitivity preset | **NORMAL** | Now chosen on the MIC page at runtime and stored in NVS, not in sdkconfig. NORMAL reproduces the original VAD constants exactly (3 / 850 ms / 70 / x3 / +30). It only affects behaviour while AI mode is ON - VAD is skipped entirely when AI is off, so the pet cannot be harmed by it. To undo: open the MIC page and tap NORMAL, no rebuild needed |

---

## How to tell quickly whether a flash went bad

Watch these lines after reset:

```
esp_psram: Reserving pool of 96K            <- must be 96K, not 64K
KOYODA: LVGL draw buffer: 24 lines, ...     <- small buffer active
KOYODA_BACKEND: heap at boot: ... (largest DMA block 77824 B)
```

Then long-press for AI and check:

```
KOYODA_BACKEND: backend stack low-water: ... bytes free   <- should stay > 4000
KOYODA_BACKEND: Xiaozhi backend channel open             <- and no reboot after it
```

Any `Failed to allocate priv TX buffer` means the display lost its DMA
memory again — go to Level 1 or 2.
