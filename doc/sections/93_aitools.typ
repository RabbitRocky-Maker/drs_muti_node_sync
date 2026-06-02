#import "../template/src/uastw-thesis-lib.typ": *
#let lightgray = rgb("#EEEEEE")

#heading(outlined: true, bookmarked: true, numbering: none)[Documentation table of AI-based tools]

#figure(
  table(
    columns: (auto, 1fr, 1fr),
    align: left,
    fill: (x, y) => if y == 0 { lightgray },
    table.header([*AI-based tool*], [*Intended use*], [*Prompt / source / scope*]),
    [*OpenCode (bigpickle session)*],
    [Initial C11 implementation of all 14 daemon modules, unit tests, tools, and Makefile.],
    [Interactive session generating the complete `bigpickle/code/daemon/` source tree from the V6 architecture specification.],

    [*Claude Code (Anthropic — Sonnet 4.6)*],
    [Bug analysis and targeted fixes: SYNC_RESP interoperability, follower UDP telemetry heartbeat, `TELEM_IP`/`IFACE` Makefile variables, systemd install improvements, and documentation rewrite.],
    [Session-based review of `main.c`, `statemachine.c`, and `Makefile`; identification of silent packet discard on `sock_mcast`; rewrite of all five Typst thesis sections to match the actual implemented codebase.],

    [*ChatGPT (4.0)*],
    [Initial Typst thesis template structure and section scaffolding.],
    [Template research for the FHTW Lab Report format.],
  ),
  caption: [AI tools used in this project.],
)
