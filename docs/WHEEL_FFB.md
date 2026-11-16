# Steering wheel force feedback

`WheelFFB` (INI, `[Controls]`) drives a DirectInput force feedback wheel the way the arcade game drove
its cabinet's steering motor. The PC release has no force feedback code; this is a port of the arcade
release's.

## Where it comes from

OutRun 2 SP SDX (Sega Lindbergh) shipped its main program (`Jennifer`) with its function names, and its
`CabinetCtrl_*` / `DrCtrl*` functions decide what the deluxe cabinet's drive board does. The game sends
the drive board three kinds of command:

- **Reaction level (0x0B), every frame** (`DrCtrlDataSet`): 4 to 15. Full while the front tyres' slip
  angle is under 6 degrees, fading along a sine to nothing at 12 (a drift lightens the wheel), x0.8 for
  each front tyre on a loose surface, out of 10, plus 1 when a load ratio of the front tyres passes 1.2.
  The level rises at rate 2 and falls at rate 31.
- **Damping (0x06)**: 2, or 1 at the lowest level.
- **A force pattern (0x7B)**, mirrored by a direction bit (`SetOutFactor`), for an event:

  | Event | When | Pattern |
  |---|---|---|
  | 0, 2 | rebounding off a wall (0 on some wall surfaces), away from the rebound | 11, 0 |
  | 6 | a physics flag (CAR_WORK +0x244 bit 3) above 5 km/h, by the wall side | 0 |
  | 8, 9 | contact on a corner of the car (four groups of three contact fields) | 0 |
  | 10 | a change of surface onto a marked one, the way the wheel is turned | 4 |
  | 12, 13 | surface bits 0x8014 under a front tyre (13: both, at 40 km/h or more) | 0, 2 |
  | 14, 15 | surface bits 0x2000008 under a front tyre (15: both, at 40 km/h or more) | 0, 0 |

  Each event holds off the next for a few frames (`out_factor_time_tbl`).

The 16 patterns are the game's own: it uploads them to the drive board at start-up (`packge_tbl`,
commands 0x1D/0x1E), 15 steps each around 0x80. `hooks_wheelffb.cpp` has all three tables.

## Mapping the arcade's fields onto the PC release

The arcade program has no structure layouts, so each field was found by matching the routine that
writes it in both programs. CAR_WORK has the same layout in both (the PC's copy of `CopyCarWork`
reads the same offsets); EVWORK_CAR differs:

| Field | Arcade | PC | Matched in (PC address) |
|---|---|---|---|
| front tyres' slip angle | 0x54 | 0x4C | CalcTireDirection (0x500700) |
| velocity heading | 0x170 | 0x160 | CopyCarWork (0x4A1140) |
| speed, km/h | 0x3AC | 0x1F4 | CopyCarWork |
| surface bits, all wheels / front wheels | 0x3FC / 0x404, 0x408 | 0x244 / 0x24C, 0x250 | CopyCarWork |
| wall side | 0x439 | 0x281 | CbwColiWall (0x5041B0) |
| rebound state / heading | 0x43B / 0x43E | 0x283 / 0x286 | CwReboundStatus (0x503A20) |
| wall surface | 0x460 | 0x2A8 | CarBodyWallColiCheck (0x504CE0) |
| crush time / flags | 0x480 / 0x4A4 | 0x2C8 / 0x2F0 | SetCrushMotion (0x4A2270) |
| contacts | 0x6C8-0x6D4, 0x528.., 0x5FC.. | 0x514-0x520, 0x374.., 0x448.. | CheckWhichSideColiHappen (0x4F6640) |

The player's CAR_WORK is at 0x82E7F0 (SetCrushMotion's call to PlWrecker). The steering direction for
event 10 comes from the game's `GetVolume(0)` (0x453720).

## What is not the game's

The drive board's firmware turns those commands into motor torque, and it is not in the game. Here:

- a level is a DirectInput spring at level / 15 of `WheelFFBSpring`;
- a rate is a change of level per tick, in sixteenths of a level (rising slowly, falling fast); that
  reading of the rate is ours;
- damping is a DirectInput damper at damping / 2 of `WheelFFBDamper`;
- a pattern step lasts one game tick, as a constant force of (step - 0x80) / 127 of `WheelFFBEffects`.

The level, the rates and the patterns are the arcade's; the scales are settings to tune by feel.

## Device

The first DirectInput device with force feedback, or the one whose name contains `WheelFFBDevice`, is
opened exclusively with the game's window and its auto-centre switched off. If none is found it is
looked for again every half minute. The log (`OutRun2006Tweaks.log`) says which device was used, or
why none was.

## Not done

- Outside a race the game's car code doesn't run, so nothing updates the wheel there (the arcade holds
  it at level 4).
- Only the player's car tick drives it (the same hook as the Xbox vibration).
