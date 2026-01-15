#!/usr/bin/env pwsh
# Iterative Task Runner
# Runs Claude Code in a loop to complete tasks from a taskplanner-created folder
#
# Usage: .\docs\ai\iterate.ps1 <featurename> [-MaxIterations N] [-Interactive] [-DryRun]
#
# Arguments:
#   featurename     Name of the folder in docs/ai/work/ containing prompt.md and tasks.json
#   -MaxIterations  Maximum iterations before stopping (default: 50)
#   -Interactive    Pause between iterations for user confirmation (default: auto/no pause)
#   -DryRun         Show what would be executed without running

param(
    [Parameter(Position=0, Mandatory=$true)]
    [string]$FeatureName,

    [int]$MaxIterations = 50,
    [switch]$Interactive,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..\..")
$WorkDir = Join-Path $ScriptDir "work\$FeatureName"
$PromptMd = Join-Path $WorkDir "prompt.md"
$TasksJson = Join-Path $WorkDir "tasks.json"

function Show-ClaudeStream {
    param([string]$Line)

    try {
        $obj = $Line | ConvertFrom-Json -ErrorAction Stop

        switch ($obj.type) {
            "assistant" {
                if ($obj.message.content) {
                    foreach ($block in $obj.message.content) {
                        if ($block.type -eq "text") {
                            Write-Host $block.text -ForegroundColor White
                        }
                        elseif ($block.type -eq "tool_use") {
                            $summary = ""
                            if ($block.input) {
                                if ($block.input.file_path) {
                                    $summary = $block.input.file_path
                                } elseif ($block.input.pattern) {
                                    $summary = $block.input.pattern
                                } elseif ($block.input.command) {
                                    $cmd = $block.input.command
                                    if ($cmd.Length -gt 60) { $cmd = $cmd.Substring(0, 60) + "..." }
                                    $summary = $cmd
                                } else {
                                    $inputStr = $block.input | ConvertTo-Json -Compress -Depth 1
                                    if ($inputStr.Length -gt 60) { $inputStr = $inputStr.Substring(0, 60) + "..." }
                                    $summary = $inputStr
                                }
                            }
                            Write-Host "[Tool: $($block.name)] $summary" -ForegroundColor Yellow
                        }
                    }
                }
            }
            "user" {
                # Tool results - skip verbose output
            }
            "result" {
                Write-Host "`n--- Session Complete ---" -ForegroundColor Cyan
                if ($obj.cost_usd) {
                    Write-Host "Cost: `$$($obj.cost_usd)" -ForegroundColor DarkCyan
                }
            }
            "system" {
                # System messages - skip
            }
        }
    }
    catch {
        # Not valid JSON, skip
    }
}

# Verify feature folder exists
if (-not (Test-Path $WorkDir)) {
    Write-Error "Feature folder not found: $WorkDir`nRun '/taskplanner $FeatureName' first to create it."
    exit 1
}

# Verify required files exist
foreach ($file in @($PromptMd, $TasksJson)) {
    if (-not (Test-Path $file)) {
        Write-Error "Required file not found: $file"
        exit 1
    }
}

$Prompt = @"
You are an autonomous coding agent working on: $FeatureName

Read these files for context:
- docs/ai/work/$FeatureName/prompt.md - Detailed instructions and architecture
- docs/ai/work/$FeatureName/tasks.json - Task list with completion status

Do exactly ONE task per iteration.

## Steps

1. Read tasks.json and find the next task to implement (respecting dependencies)
2. Use /ultrathink to plan the implementation carefully
3. Implement that ONE task only
4. After successful implementation:
   - Mark the task completed in tasks.json ("completed": true)
   - Commit your changes
   - If new tasks emerged, add them to tasks.json

## Critical Rules

- Only mark a task complete if you verified the work is done (build passes, etc.)
- If stuck, document the issue in the task's notes field and move on
- Do ONE task per iteration, then stop
- NEVER commit files in docs/ai/ unless explicitly required by the task

## Completion Signal

If ALL tasks in tasks.json have "completed": true, output exactly:
===ALL_TASKS_COMPLETE===
"@

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Iterative Task Runner" -ForegroundColor Cyan
Write-Host "  Feature: $FeatureName" -ForegroundColor Cyan
Write-Host "  Max iterations: $MaxIterations" -ForegroundColor Cyan
Write-Host "  Mode: $(if ($Interactive) { 'Interactive' } else { 'Auto' })" -ForegroundColor Cyan
Write-Host "  Working directory: $RepoRoot" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

if ($DryRun) {
    Write-Host "[DRY RUN] Would execute with prompt:" -ForegroundColor Yellow
    Write-Host $Prompt
    Write-Host ""
    Write-Host "Feature folder: $WorkDir" -ForegroundColor Yellow
    Write-Host "Prompt file: $PromptMd" -ForegroundColor Yellow
    Write-Host "Tasks file: $TasksJson" -ForegroundColor Yellow
    exit 0
}

Push-Location $RepoRoot

try {
    for ($i = 1; $i -le $MaxIterations; $i++) {
        Write-Host ""
        Write-Host "========================================" -ForegroundColor Yellow
        Write-Host "  Iteration $i of $MaxIterations" -ForegroundColor Yellow
        Write-Host "========================================" -ForegroundColor Yellow
        Write-Host ""

        claude --dangerously-skip-permissions --verbose -p $Prompt --output-format stream-json 2>&1 | ForEach-Object {
            Show-ClaudeStream $_
        }

        # Check task status after each run
        $tasks = Get-Content $TasksJson | ConvertFrom-Json
        $incomplete = @($tasks.tasks | Where-Object { -not $_.completed })
        $inProgress = @($tasks.tasks | Where-Object { $_.started -and -not $_.completed })

        if ($incomplete.Count -eq 0) {
            Write-Host ""
            Write-Host "========================================" -ForegroundColor Green
            Write-Host "  ALL TASKS COMPLETE!" -ForegroundColor Green
            Write-Host "  Feature: $FeatureName" -ForegroundColor Green
            Write-Host "  Finished after $i iterations" -ForegroundColor Green
            Write-Host "========================================" -ForegroundColor Green
            Write-Host ""
            exit 0
        }

        Write-Host ""
        Write-Host "Remaining tasks: $($incomplete.Count)" -ForegroundColor Cyan
        if ($inProgress.Count -gt 0) {
            Write-Host "In progress: $($inProgress[0].title)" -ForegroundColor Yellow
        }

        if ($Interactive) {
            Write-Host "Press Enter to continue, Ctrl+C to stop..." -ForegroundColor Cyan
            Read-Host
        } else {
            Start-Sleep -Seconds 2
        }
    }

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "  Max iterations ($MaxIterations) reached" -ForegroundColor Red
    Write-Host "  Check tasks.json for remaining tasks" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    Write-Host ""
    exit 1
}
finally {
    Pop-Location
}
