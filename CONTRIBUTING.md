# Contributing

## Git Hooks

After cloning, enable the git hooks:

```bash
git config core.hooksPath .githooks
```

This blocks accidental commits of compiled `.a` files and ghostty submodule pointer changes. Use `git commit --no-verify` for intentional submodule upgrades.

## Submitting Changes

1. Fork and create a branch from `main`
2. Make your changes and ensure tests pass (`cd tests && cmake . && make && ctest`)
3. Submit a pull request with a clear description of what and why

By submitting a pull request you agree your contributions are licensed under [MIT](LICENSE).

If your change requires setup or verification steps outside the app itself, add a script in `scripts/` to automate it.
