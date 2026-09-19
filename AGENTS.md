# LinuxCNC Ctrl Agent Guidance

## Native formatting and linting

- Treat native formatting and linting as part of completing every change under
  `native/` or to native build configuration. Run the checks proactively; do
  not wait for the user or CI to report failures.
- Run `./scripts/native-format.sh check` before declaring native work complete.
  If it fails, run `./scripts/native-format.sh format`, review the resulting
  diff, and rerun the check.
- Run `./scripts/native-lint.sh <build-dir>` with a compilation database made
  using `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Fix all diagnostics introduced or
  exposed by the change, then rerun the linter.
- Use the tool versions required by the scripts and CI. Install missing tools
  in a temporary environment when practical.
- If a check cannot run because a required external dependency or checkout is
  unavailable, use the relevant CI logs to fix known diagnostics and state
  exactly which command remains unverified and why.
- Never describe native work as complete while known formatter or linter
  failures remain.
