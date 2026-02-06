# Phase Prompts

Use these templates for `codex exec --json` child runs. Replace `<TASK>`, `<SLUG>`, and `<REPO_ROOT>`.

## Phase 1: Context

```text
You are the context phase for task "<TASK>" in repository <REPO_ROOT>.

Read CLAUDE.md for the basic coding rules and guidelines.

Read AGENTS.md and all relevant source files. Write a focused context doc:
- .ai/<SLUG>/context.md

Include:
1. Relevant files and why they matter
2. Existing patterns to follow
3. Risks and unknowns
4. Verification hooks (what to build/test later)

Do not implement code in this phase.
```

## Phase 2: Plan

```text
You are the planning phase for task "<TASK>" in repository <REPO_ROOT>.

Read CLAUDE.md for the basic coding rules and guidelines.

Read:
- .ai/<SLUG>/inputs.md
- .ai/<SLUG>/context.md

Create:
- .ai/<SLUG>/plan.md

Plan requirements:
1. Concrete file-level edits
2. Ordered phases
3. Verification commands
4. Rollback/risk notes
```

## Phase 3: Implement

```text
You are the implementation phase for task "<TASK>" in repository <REPO_ROOT>.

Read CLAUDE.md for the basic coding rules and guidelines.

Read:
- .ai/<SLUG>/inputs.md
- .ai/<SLUG>/context.md
- .ai/<SLUG>/plan.md

Implement the plan in code. Then write:
- .ai/<SLUG>/implementation.md

Include:
1. Files changed
2. What was implemented
3. Any deviations from plan and why
```

## Phase 4: Verify

```text
You are the verification phase for task "<TASK>" in repository <REPO_ROOT>.

Read CLAUDE.md for the basic coding rules and guidelines.

Read:
- .ai/<SLUG>/plan.md
- .ai/<SLUG>/implementation.md

Run the relevant build/test commands from AGENTS.md and plan.md.
Append results to:
- .ai/<SLUG>/implementation.md

If blocked by locked files or access errors, stop and report exact blocker.
```

## Phase 5: Review

```text
You are the review phase for task "<TASK>" in repository <REPO_ROOT>.

Read CLAUDE.md for the basic coding rules and guidelines.

Read:
- .ai/<SLUG>/context.md
- .ai/<SLUG>/plan.md
- .ai/<SLUG>/implementation.md

Perform a code review focused on regressions, thread-safety, performance, and missing tests.
Write:
- .ai/<SLUG>/review.md

If issues are found, implement fixes and update implementation.md/review.md with final status.
```

## Example Runner Commands

```powershell
codex exec --json -C <REPO_ROOT> "<PHASE_PROMPT>" | Tee-Object .ai/<SLUG>/logs/phase-1-context.jsonl
codex exec --json -C <REPO_ROOT> "<PHASE_PROMPT>" | Tee-Object .ai/<SLUG>/logs/phase-2-plan.jsonl
codex exec --json -C <REPO_ROOT> "<PHASE_PROMPT>" | Tee-Object .ai/<SLUG>/logs/phase-3-implement.jsonl
codex exec --json -C <REPO_ROOT> "<PHASE_PROMPT>" | Tee-Object .ai/<SLUG>/logs/phase-4-verify.jsonl
codex exec --json -C <REPO_ROOT> "<PHASE_PROMPT>" | Tee-Object .ai/<SLUG>/logs/phase-5-review.jsonl
```
