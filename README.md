# smartTerm

smartTerm is a lightweight terminal client that augments the traditional Linux command line with AI-assisted features. It communicates with web APIs to provide contextual suggestions, summaries, and automation directly in the shell, helping users work faster without leaving their terminal workflows.

## Building on Linux

The project ships with a standard `Makefile`. To build the executable, ensure the required development packages are installed (see `./configure`). Then run:

```bash
./configure
make
```

This will compile the `smart_terminal` binary. To remove build artifacts, run `make clean`. You can install the binary system-wide with `sudo make install`.

## Basic Controls

Once the application is running:

- **Command Input** – Type commands as you would in a normal terminal session.
- **AI Suggestions** – Trigger contextual assistance using the program's configured hotkeys or prompt commands.
- **Session Management** – Exit the application with `Ctrl+C` or `Ctrl+D`.

## Roadmap

- Expand AI integrations with additional providers.
- Provide configuration options for custom key bindings and prompts.
- Add unit tests and continuous integration workflows.
- Package releases for popular Linux distributions.

## License

See [LICENSE](./LICENSE) for full licensing information.
