# PCB-A Fabrication Notes

`PCB-A` is the main electronics board for the Replay 2026 Badge.

Included outputs:

- [`V0-Replay-26_PCB-A_GBR_260331-R3.zip`](V0-Replay-26_PCB-A_GBR_260331-R3.zip): main-board Gerber package.
- [`V0-Replay-26_PCB-A_BOM-260331-R1.csv`](V0-Replay-26_PCB-A_BOM-260331-R1.csv): bill of materials.
- [`V0-Replay-26_PCB-A_CPL-260331-R1.csv`](V0-Replay-26_PCB-A_CPL-260331-R1.csv): component placement list.
- [`JLCPCB/V0-Replay-26_PCB-A_BOM-260331-R2.csv`](JLCPCB/V0-Replay-26_PCB-A_BOM-260331-R2.csv): JLCPCB-oriented BOM.
- [`JLCPCB/V0-Replay-26_PCB-A_CPL-260331-R2.csv`](JLCPCB/V0-Replay-26_PCB-A_CPL-260331-R2.csv): JLCPCB-oriented placement list.
- [`Elecrow_PCBA_Quote_Temporal-PCB-A_260331-R0.xlsx`](Elecrow_PCBA_Quote_Temporal-PCB-A_260331-R0.xlsx): reviewed public-safe supplier quote context.

The included main Gerber package is revision `R3`. Production notes record the
`R2` change as an increased via diameter to 0.3 mm plus silkscreen for the
SSD1309 display.

If the KiCad source changes, regenerate and review the Gerber, BOM, CPL, and
mechanical exports before publishing a new release package.
