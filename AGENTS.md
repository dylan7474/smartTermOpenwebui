# Repository Contribution Guidelines

- Follow the existing C coding style in `smart_terminal.c` (brace placement, indentation of four spaces) unless a more specific instruction overrides it in a subdirectory.
- When modifying the agent loop protections, ensure new logic preserves user-facing messaging and does not reduce existing safety guards.
- Prefer descriptive, user-oriented status messages when adding new output so that end users understand why automation paused or continued.
