---
description: Implement a feature or fix using multi-agent workflow with fresh context at each phase
allowed-tools: Read, Write, Edit, Glob, Grep, Bash, Task, AskUserQuestion, TodoWrite
---

# Task - Multi-Agent Implementation Workflow

You orchestrate a multi-phase implementation workflow that uses fresh agent spawns to work within context window limits on a large codebase.

**Arguments:** `$ARGUMENTS` = "$ARGUMENTS"

If `$ARGUMENTS` is provided, it's the task description. If empty, ask the user what they want implemented.

## Overview

The workflow produces `.ai/<feature-name>/` containing:
- `context.md` - Gathered codebase context relevant to the task
- `plan.md` - Detailed implementation plan with phases

Then spawns implementation agents to execute each phase, and finally verifies the build.

## Phase 0: Setup

1. Understand the task from `$ARGUMENTS` or ask the user.
2. Check existing folders in `.ai/` to pick a unique short name (1-2 lowercase words, hyphen-separated).
3. Create the folder `.ai/<feature-name>/`.

## Phase 1: Context Gathering

Spawn an agent (Task tool, subagent_type=`general-purpose`) with this prompt structure:

```
You are a context-gathering agent for a large C++ codebase (Telegram Desktop).

TASK: <paste the user's task description here>

YOUR JOB: Read CLAUDE.md, inspect the codebase, find ALL files and code relevant to this task, and write a comprehensive context document.

Steps:
1. Read CLAUDE.md for project conventions and build instructions.
2. Search the codebase for files, classes, functions, and patterns related to the task.
3. Read all potentially relevant files. Be thorough - read more rather than less.
4. For each relevant file, note:
   - File path
   - Relevant line ranges
   - What the code does and how it relates to the task
   - Key data structures, function signatures, patterns used
5. Look for similar existing features that could serve as a reference implementation.
6. Check api.tl if the task involves Telegram API.
7. Check .style files if the task involves UI.
8. Check lang.strings if the task involves user-visible text.

Write your findings to: .ai/<feature-name>/context.md

The context.md should contain:
- **Task Description**: The full task restated clearly
- **Relevant Files**: Every file path with line ranges and descriptions of what's there
- **Key Code Patterns**: How similar things are done in the codebase (with code snippets)
- **Data Structures**: Relevant types, structs, classes
- **API Methods**: Any TL schema methods involved (copied from api.tl)
- **UI Styles**: Any relevant style definitions
- **Localization**: Any relevant string keys
- **Build Info**: Build command and any special notes
- **Reference Implementations**: Similar features that can serve as templates

Be extremely thorough. Another agent with NO prior context will read this file and must be able to understand everything needed to implement the task.
```

After this agent completes, read `context.md` to verify it was written properly.

## Phase 2: Planning

Spawn an agent (Task tool, subagent_type=`general-purpose`) with this prompt structure:

```
You are a planning agent. You must create a detailed implementation plan.

Read these files:
- .ai/<feature-name>/context.md - Contains all gathered context
- Then read the specific source files referenced in context.md to understand the code deeply.

Use /ultrathink to reason carefully about the implementation approach.

Create a detailed plan in: .ai/<feature-name>/plan.md

The plan.md should contain:

## Task
<one-line summary>

## Approach
<high-level description of the implementation approach>

## Files to Modify
<list of files that will be created or modified>

## Files to Create
<list of new files, if any>

## Implementation Steps

Each step must be specific enough that an agent can execute it without ambiguity:
- Exact file paths
- Exact function names
- What code to add/modify/remove
- Where exactly in the file (after which function, in which class, etc.)

Number every step. Group steps into phases if there are more than ~8 steps.

### Phase 1: <name>
1. <specific step>
2. <specific step>
...

### Phase 2: <name> (if needed)
...

## Build Verification
- Build command to run
- Expected outcome

## Status
- [ ] Phase 1: <name>
- [ ] Phase 2: <name> (if applicable)
- [ ] Build verification
```

After this agent completes, read `plan.md` to verify it was written properly.

## Phase 3: Plan Assessment

Spawn an agent (Task tool, subagent_type=`general-purpose`) with this prompt structure:

```
You are a plan assessment agent. Review and refine an implementation plan.

Read these files:
- .ai/<feature-name>/context.md
- .ai/<feature-name>/plan.md
- Then read the actual source files referenced to verify the plan makes sense.

Use /ultrathink to assess the plan:

1. **Correctness**: Are the file paths and line references accurate? Does the plan reference real functions and types?
2. **Completeness**: Are there missing steps? Edge cases not handled?
3. **Code quality**: Will the plan minimize code duplication? Does it follow existing codebase patterns from CLAUDE.md?
4. **Design**: Could the approach be improved? Are there better patterns already used in the codebase?
5. **Phase sizing**: Each phase should be implementable by a single agent in one session. If a phase has more than ~8-10 substantive code changes, split it further.

Update plan.md with your refinements. Keep the same structure but:
- Fix any inaccuracies
- Add missing steps
- Improve the approach if you found better patterns
- Ensure phases are properly sized for single-agent execution
- Add a line at the top of the Status section: `Phases: <N>` indicating how many implementation phases there are
- Add `Assessed: yes` at the bottom of the file

If the plan is small enough for a single agent (roughly <=8 steps), mark it as a single phase.
```

After this agent completes, read `plan.md` to verify it was assessed.

## Phase 4: Implementation

Now read `plan.md` yourself to understand the phases.

For each phase in the plan that is not yet marked as done, spawn an implementation agent (Task tool, subagent_type=`general-purpose`):

```
You are an implementation agent working on phase <N> of an implementation plan.

Read these files first:
- .ai/<feature-name>/context.md - Full codebase context
- .ai/<feature-name>/plan.md - Implementation plan

Then read the source files you'll be modifying.

YOUR TASK: Implement ONLY Phase <N> from the plan:
<paste the specific phase steps here>

Rules:
- Follow the plan precisely
- Follow CLAUDE.md coding conventions (no comments except complex algorithms, use auto, empty line before closing brace, etc.)
- Do NOT modify .ai/ files except to update the Status section in plan.md
- When done, update plan.md Status section: change `- [ ] Phase <N>: ...` to `- [x] Phase <N>: ...`
- Do NOT work on other phases

When finished, report what you did and any issues encountered.
```

After each implementation agent returns:
1. Read `plan.md` to check the status was updated.
2. If more phases remain, spawn the next implementation agent.
3. If all phases are done, proceed to build verification.

## Phase 5: Build Verification

Only run this phase if the task involved modifying project source code (not just docs or config).

Spawn a build verification agent (Task tool, subagent_type=`general-purpose`):

```
You are a build verification agent.

Read these files:
- .ai/<feature-name>/context.md
- .ai/<feature-name>/plan.md

The implementation is complete. Your job is to build the project and fix any build errors.

Steps:
1. Run: cmake --build "c:\Telegram\tdesktop\out" --config Debug --target Telegram
2. If the build succeeds, update plan.md: change `- [ ] Build verification` to `- [x] Build verification`
3. If the build fails:
   a. Read the error messages carefully
   b. Read the relevant source files
   c. Fix the errors in accordance with the plan and CLAUDE.md conventions
   d. Rebuild and repeat until the build passes
   e. Update plan.md status when done

Rules:
- Only fix build errors, do not refactor or improve code
- Follow CLAUDE.md conventions
- If build fails with file-locked errors (C1041, LNK1104), STOP and report - do not retry

When finished, report the build result.
```

After the build agent returns, read `plan.md` to confirm the final status.

## Completion

When all phases including build verification are done:
1. Read the final `plan.md` and report the summary to the user.
2. Show which files were modified/created.
3. Note any issues encountered during implementation.

## Error Handling

- If any agent fails or gets stuck, report the issue to the user and ask how to proceed.
- If context.md or plan.md is not written properly by an agent, re-spawn that agent with more specific instructions.
- If build errors persist after the build agent's attempts, report the remaining errors to the user.
