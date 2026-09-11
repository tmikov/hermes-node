# Documentation

```
docs/
  DESIGN.md, DEBUGGER.md,   how the runtime, the debugger and Intl
  INTL.md                   support work
  superpowers/
    specs/                  design documents -- what a feature is and why
    plans/                  implementation plans, and the progress file each one tracks
  notes/                    everything that is neither: spikes, findings,
                            analyses, handoff briefs and their results
```

`superpowers/specs/` and `superpowers/plans/` follow the layout the
brainstorming and writing-plans skills expect, so a plan written by those
skills lands where the rest already are. A design doc is
`YYYY-MM-DD-<topic>-design.md` and its plan is `YYYY-MM-DD-<topic>-plan.md`;
the two names line up so `ls` pairs them.

Progress files sit beside the plans rather than in a directory of their own,
because each names the plan it tracks in its first line and is useless apart
from it.

**None of this is current documentation, apart from `DESIGN.md`,
`DEBUGGER.md` and `INTL.md` at the top.** A spec describes what was intended
at the time it was written, and a plan describes how it was going to be
built; neither is updated when the code moves on. `CLAUDE.md` and the
comments in the code are what stay true otherwise. The three exceptions
document present behavior a user depends on -- how the runtime, the debugger
and Intl support work right now -- and are meant to be kept in sync with the
code the way `CLAUDE.md` is. The rest of `docs/` is here to answer "why is
it like this?", which it does well, and "how does it work now?", which it
does not.

The oldest material -- the plans dated 2026-02 and several notes -- is from
building the project itself rather than a feature of it.
