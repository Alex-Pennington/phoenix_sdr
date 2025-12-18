┌─────────────────────────────────────────────────────────────────────────┐

│                        SDR SERVER (2MHz IQ)                             │

└────────────────────────────────┬────────────────────────────────────────┘

&nbsp;                                │

&nbsp;                                ▼

┌─────────────────────────────────────────────────────────────────────────┐

│                    WATERFALL.C - TCP RECEIVE                            │

│  - Receives MAGIC\_IQDQ frames                                           │

│  - Normalizes S16 to \[-1,1]                                             │

└────────────────────────────────┬────────────────────────────────────────┘

&nbsp;                                │

&nbsp;           ┌────────────────────┴────────────────────┐

&nbsp;           │                                         │

&nbsp;           ▼                                         ▼

┌───────────────────────┐               ┌───────────────────────┐

│   DETECTOR PATH       │               │   DISPLAY PATH        │

│   50kHz (2MHz/40)     │               │   12kHz (2MHz/167)    │

│   5kHz lowpass        │               │   5kHz lowpass        │

└───────────┬───────────┘               └───────────┬───────────┘

&nbsp;           │                                       │

&nbsp;   ┌───────┼───────┬───────┬───────┐              │

&nbsp;   │       │       │       │       │              │

&nbsp;   ▼       ▼       ▼       ▼       ▼              ▼

┌──────┐ ┌──────┐ ┌─────┐ ┌─────┐ ┌─────┐    ┌─────────┐

│TICK  │ │MARKER│ │BCD  │ │BCD  │ │BCD  │    │TONE     │

│DET   │ │DET   │ │TIME │ │FREQ │ │ENV  │    │TRACKERS │

│1000Hz│ │1000Hz│ │100Hz│ │100Hz│ │DEPR │    │DISPLAY  │

└──┬───┘ └──┬───┘ └──┬──┘ └──┬──┘ └──┬──┘    └─────────┘

&nbsp;  │        │        │       │       │

&nbsp;  │        │        │       │       ▼

&nbsp;  │        │        │       │   ┌─────────┐

&nbsp;  │        │        │       │   │BCD\_DEC  │ ← NO CALLBACK (disabled)

&nbsp;  │        │        │       │   │(counts  │   No symbol output

&nbsp;  │        │        │       │   │ only)   │

&nbsp;  │        │        │       │   └─────────┘

&nbsp;  │        │        │       │

&nbsp;  ▼        ▼        ▼       ▼

┌──────────────────────────────────────────┐

│            on\_tick\_marker()              │

│            on\_marker\_event()             │

│            on\_bcd\_time\_event()           │

│            on\_bcd\_freq\_event()           │

└────────────────┬─────────────────────────┘

&nbsp;                │

&nbsp;   ┌────────────┼────────────┐

&nbsp;   │            │            │

&nbsp;   ▼            ▼            ▼

┌───────┐  ┌───────────┐  ┌─────────────────────┐

│SYNC   │  │           │  │  BCD\_CORRELATOR     │

│DET    │  │           │  │  (window-based)     │

│       │◄─┤ LINKAGE   │◄─┤                     │

│anchor │  │           │  │  calls get\_minute   │

│minute │  │           │  │  \_anchor() from     │

│       │  │           │  │  sync\_detector      │

└───────┘  └───────────┘  └──────────┬──────────┘

&nbsp;   │                                │

&nbsp;   │ SYNC LOCKED?                   │

&nbsp;   │ yes → anchor\_ms available      │

&nbsp;   │ no  → events ignored           │

&nbsp;   │                                │

&nbsp;   ▼                                ▼

┌────────────────────────────────────────────┐

│         WINDOW-BASED INTEGRATION           │

│                                            │

│  anchor\_ms + second\*1000 = window\_start    │

│  Events accumulate in current window       │

│  At window boundary:                       │

│    - Estimate pulse duration               │

│    - Classify (0/1/P)                      │

│    - Emit EXACTLY ONE symbol               │

└─────────────────────┬──────────────────────┘

&nbsp;                     │

&nbsp;                     ▼

&nbsp;             ┌───────────────┐

&nbsp;             │ telem\_sendf() │

&nbsp;             │ TELEM\_BCDS    │

&nbsp;             │ "SYM,X,ts,dur"│

&nbsp;             └───────────────┘

