# Replay 2026 Badge Data Bundle

The firmware embeds [`data/out/bundle.bin`](out/bundle.bin) at build time. That
bundle is generated from the public schedule, speaker, and floor files under
`data/in/`:

- [`in/schedule.yaml`](in/schedule.yaml)
- [`in/speakers.md`](in/speakers.md)
- [`in/floors.md`](in/floors.md)

The generated outputs include:

- [`out/schedule.json`](out/schedule.json)
- [`out/speakers.json`](out/speakers.json)
- [`out/floors.json`](out/floors.json)
- [`out/data_api.h`](out/data_api.h)
- [`out/bundle.bin`](out/bundle.bin)

Firmware consumers include [`DataCache.cpp`](../firmware/src/api/DataCache.cpp),
[`ScheduleData.cpp`](../firmware/src/screens/ScheduleData.cpp), and
[`MapScreens.cpp`](../firmware/src/screens/MapScreens.cpp).

To rebuild the generated files:

```bash
cd data
python3 -m pip install msgpack pyyaml
python3 build-data.py
```
