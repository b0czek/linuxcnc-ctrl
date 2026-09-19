# LinuxCNC status delta boundaries

This tree defines the smallest status values we intentionally subscribe to and
relay as deltas.

- `*` marks a minimum delta unit.
- Live consumers subscribe only to marked values, not their ancestors.
- Children named inside a marked record describe its contents; they are not
  separate subscription or delta units. Reads inside one normalize to the
  marked parent.

```text
LinuxCNCStat
|-- echoSerialNumber *
|-- state *
|-- task
|   |-- mode *
|   |-- state *
|   |-- execState *
|   |-- interpState *
|   |-- stopState *
|   |-- callLevel *
|   |-- motionLine *
|   |-- currentLine *
|   |-- readLine *
|   |-- optionalStopState *
|   |-- blockDeleteState *
|   |-- inputTimeout *
|   |-- file *
|   |-- command *
|   |-- iniFilename *
|   |-- g5xOffset *                  Position [X Y Z A B C U V W]
|   |-- g5xIndex *
|   |-- g5xOffsets *                 complete coordinate-system array
|   |-- g5xRotations *               complete rotation array
|   |-- g92Offset *                  Position [X Y Z A B C U V W]
|   |-- g28Position *                Position [X Y Z A B C U V W]
|   |-- g30Position *                Position [X Y Z A B C U V W]
|   |-- rotationXY *
|   |-- toolOffset *                 Position [X Y Z A B C U V W]
|   |-- activeGCodes *               complete ActiveGCodes record
|   |-- activeMCodes *               complete ActiveMCodes record
|   |-- activeSettings *             complete ActiveSettings record
|   |-- programUnits *
|   |-- interpreterErrorCode *
|   |-- taskPaused *
|   |-- delayLeft *
|   `-- queuedMdiCommands *
|-- motion
|   |-- traj
|   |   |-- linearUnits *
|   |   |-- angularUnits *
|   |   |-- cycleTime *
|   |   |-- joints *
|   |   |-- spindles *
|   |   |-- availableAxes *          complete axis-name array
|   |   |-- mode *
|   |   |-- enabled *
|   |   |-- inPosition *
|   |   |-- queue *
|   |   |-- activeQueue *
|   |   |-- queueFull *
|   |   |-- id *
|   |   |-- paused *
|   |   |-- singleStepping *
|   |   |-- feedRateOverride *
|   |   |-- rapidRateOverride *
|   |   |-- position *               Position [X Y Z A B C U V W]
|   |   |-- actualPosition *         Position [X Y Z A B C U V W]
|   |   |-- acceleration *
|   |   |-- maxVelocity *
|   |   |-- maxAcceleration *
|   |   |-- probedPosition *         Position [X Y Z A B C U V W]
|   |   |-- probeTripped *
|   |   |-- probing *
|   |   |-- probeVal *
|   |   |-- kinematicsType *
|   |   |-- motionType *
|   |   |-- distanceToGo *
|   |   |-- dtg *                    Position [X Y Z A B C U V W]
|   |   |-- currentVelocity *
|   |   |-- feedOverrideEnabled *
|   |   |-- adaptiveFeedEnabled *
|   |   `-- feedHoldEnabled *
|   |-- joint
|   |   `-- [index] *                complete JointStat record
|   |-- axis
|   |   `-- [index] *                complete AxisStat record
|   |-- spindle
|   |   `-- [index] *                complete SpindleStat record
|   |-- digitalInput *               complete array
|   |-- digitalOutput *              complete array
|   |-- analogInput *                complete array
|   `-- analogOutput *               complete array
|-- io
|   |-- tool *                       complete ToolIoStat record
|   |-- coolant *                    complete CoolantIoStat record
|   `-- estop *
|-- debug *
`-- toolTable *                      complete ordered tool table
```

## What we value

- A delta unit is the smallest value that is independently meaningful and safe
  to replace atomically.
- Positions stay complete vectors so consumers never observe axes from
  different samples.
- Joint, axis, spindle, tool-I/O, coolant, modal-code, and settings records are
  small coherent values; splitting their fields is not worth the merge cost.
- Digital and analog I/O arrays are intentionally coarse. They do not justify
  per-channel subscription or delta machinery.
- Full snapshots remain valid for initial synchronization and replay recovery.

Applying a delta sequence to its starting snapshot must produce exactly the
same status as a fresh snapshot at the final sequence. Valid zero, false, empty
string, and empty collection values must remain distinguishable from an absent
change.
