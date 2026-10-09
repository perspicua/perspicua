Perspicua Documentation
=======================

This directory holds the project's design and process docs. Start here, then
read the file that matches what you are trying to do.

--------------------------------------------------------------------------------
THE MAP  (what to read, and when)
--------------------------------------------------------------------------------

  architecture.txt
      How the system fits together: the source-tree layout, the boot sequence,
      and a tour of the major subsystems (mm, sched, fs, drivers, libc).
      READ THIS FIRST if you are new to the codebase.

  vision.txt
      The NORTH STAR (the "why" and the "where"). Perspicua's long-term
      identity — the self-healing, flight-recorder kernel — its four pillars
      (V1-V4), the design rules they impose today, and how the roadmap serves
      them. Read it before proposing a new direction or a large feature.

  order.txt
      The build ORDER (the "when"). A dependency-sequenced, phased roadmap of
      what to build next and what "done" means for each step. This is the
      working plan, and its last section is the backlog of what is wanted but
      not yet sequenced.

  coding_style_guidelines.txt
      House style: naming, braces, header/source structure, include order, and
      the commenting philosophy. Follow it for every change.

  branch_workflow.txt
      Git branch model: feature/* -> dev -> main, hotfixes, tags, and the
      branch-protection rules. How code physically moves toward a release.

  testing.txt
      How the in-kernel test suites are built and run, what a passing run looks
      like, and the hardware-validation gate before anything reaches main.

  hardware.txt
      Running on a real Raspberry Pi 4B: wiring a serial console (USB-to-TTL
      adapter or tcpuart over Wi-Fi), the remote-reset pin, preparing the SD
      card, and what to check when it does not boot.

Process files that live at the repository ROOT (by convention, not here):

  ../README.md          Project overview, hardware support, build commands.
  ../CONTRIBUTING.txt    The single entry point for making a change end to end.
  ../CREDITS.txt         Who built Perspicua, and their roles.
  ../LICENSE             Licensing terms.

--------------------------------------------------------------------------------
NAMING CONVENTION FOR THESE DOCS
--------------------------------------------------------------------------------
ALL-CAPS names are reserved for the "meta" files a repository root is expected
to have (README, LICENSE, CONTRIBUTING, CREDITS, and a future CHANGELOG).
Everything inside docs/ is lowercase. New docs follow the same rule.
