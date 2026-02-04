---
description: Plan and create a repetitive task automation (prompt.md + tasks.json pair)
allowed-tools: Read, Write, Edit, Glob, Grep, Bash(mkdir:*), Bash(ls:*), AskUserQuestion
---

# Task Planner - Create Automated Task Workflows

You are setting up a new **repetitive task automation** for Claude Code. The goal is to create a folder in `docs/ai/work/<featurename>/` containing:
- `prompt.md` - Detailed instructions for the autonomous agent
- `tasks.json` - List of tasks with completion tracking

This pair can then be executed via `docs/ai/iterate.ps1 <featurename>`.

## Your Workflow

### 1. Understand the Goal

First, understand what the user wants to automate. Ask clarifying questions using AskUserQuestion if needed:
- What is the overall goal/feature being implemented?
- What are the individual tasks involved?
- Are there dependencies between tasks?
- What files/areas of the codebase are involved?
- Are there any reference examples or patterns to follow?

### 2. Choose a Feature Name

The `<featurename>` should be:
- Short (1-2 words, lowercase, hyphen-separated)
- Easy to type on command line
- Descriptive of the work being done
- Not already used in `docs/ai/work/`

Check existing folders:
```bash
ls docs/ai/work/
```

Suggest a name to the user or let them specify one directly via $ARGUMENTS.

### 3. Use /ultrathink for Planning

Before writing the prompt, use `/ultrathink` to carefully plan:
- The structure of the prompt
- What context the autonomous agent needs
- How tasks should be broken down
- What patterns/examples to include
- Edge cases and error handling

### 4. Create the Folder and Files

Create `docs/ai/work/<featurename>/`:

**prompt.md** should include:
- Overview of what we're doing
- Architecture/context needed
- Step-by-step instructions for each task type
- Code patterns and examples
- Build/test commands
- Commit message format (see below)

### Commit Message Guidelines

All prompts should specify commit message length requirements:
- **Soft limit**: ~50 characters (ideal length for first line)
- **Hard limit**: 76 characters (must not exceed)

Example instruction for prompt.md:
```
## Commit Format

First line: Short summary ending with a dot (aim for ~50 chars, max 76 chars)

<Optional body with details, also ending with a dot.>

IMPORTANT: Never commit files in docs/ai/ unless explicitly required by a task.
```

**tasks.json** format:
```json
{
  "tasks": [
    {
      "id": "task-id",
      "title": "Short task title",
      "description": "Detailed description of what to do",
      "started": false,
      "completed": false,
      "dependencies": ["other-task-id"]
    }
  ]
}
```

### 5. Iterate with the User

After creating initial files, the user may want to:
- Add more tasks to tasks.json
- Refine the prompt with more details
- Add examples or patterns
- Clarify instructions

Keep refining until the user is satisfied.

## Arguments

If `$ARGUMENTS` is provided, it's the feature name to use:
- `$ARGUMENTS` = "$ARGUMENTS"

If empty, you'll need to determine/suggest a name based on the discussion.

## Examples

### Example 1: Settings Migration
```
/taskplanner settings-upgrade
```
Creates `docs/ai/work/settings-upgrade/` with prompt and tasks for migrating settings sections.

### Example 2: Open-ended
```
/taskplanner
```
Starts a conversation to understand what needs to be automated, then creates the appropriate folder.

## Starting Point

Let's begin! Please describe:
1. What repetitive coding task do you want to automate?
2. What is the end goal?
3. Do you have initial tasks in mind, or should we discover them together?
