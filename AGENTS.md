# AGENTS.md — F4VR-CommonFramework

The shared brunocatani framework checkout, kept for PAPER_Toolkit, legacy SCISSORS and native secondary plugins that still consume it.

- Remote: `origin` at `https://github.com/brunocatani/F4VR-CommonFramework`. It has its own history and is not a drop-in replacement for other framework checkouts (FRIK pins its own submodule).
- ROCK and PAPER do not consume this framework; never reintroduce it as their dependency for convenience.
- A framework edit is its own repository change, validated in every affected owned consumer.
- The bounded, level-aware logging change is intentional local work pending upstreaming; preserve it unless the task changes it.
