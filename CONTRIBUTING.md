# Contributing to Strikers-WiiCompiled

Thanks for wanting to help! A few ground rules.

## The short version

- Code is judged on quality, not where it came from
- You must be able to understand and explain every line you submit.
- PR descriptions and responses must be written by you, **not** generated.
- Accuracy is the bar for anything touching game behavior.

## Code quality

We don't care how your code came into existence. What we care about is whether it meets or improves the
project's patterns and standards, and the only way we measure that is by
reading it.

Low-quality code won't be merged, regardless of origin. AI slop and human slop
get the same treatment.

## If you use AI tools

That's ok, but rules apply:

1. **You must be able to explain your changes.** If a reviewer asks why a line
   exists or what a function does and you can't answer, the PR will be closed.
   "The AI wrote it" is not an explanation.
2. **Write your own PR description.** The description exists so reviewers know
   what you changed and why, in your words. Generated descriptions tend to
   describe everything and explain nothing, and they will get your PR closed.

## Pull requests

- Keep PRs focused. try and keep it at 1 change per PR. 
  Small PRs get reviewed fast.
- Explain **what** and **why**. Reference the issue if there is one.
- For anything affecting game behavior: identical behavior to real hardware is
  the goal. Be prepared to show your change doesn't diverge from the original
  game (hardware comparison, logs, whatever fits).
- Review feedback. It's about the code, not about you ;).

## Bug reports

Say what you did and what happened, and attach `console.log` from the `Logs` folder in the
`MSCRecomp` data folder ([where that is](https://github.com/BlackAndBlue95/Strikers-WiiCompiled/wiki/Building#building-the-port)).
The goal is behavior identical to the original game, so report things where the port differs from
it.

## A note on related projects

WiiCompiled, the Mario Kart Wii project this one is forked from, and the other
projects it builds on are developed independently, each with its **own**
contribution rules, including around AI usage. What applies here does not
automatically apply there, and vice versa. Check each project's own
CONTRIBUTING file.

## Legal

- Never!!! include Nintendo code, assets, or game data in a PR, an issue, or
  anywhere else in this project. No exceptions.
- By contributing, you agree your contributions are licensed under
  [GPL v3.0](LICENSE), like the rest of the project.
