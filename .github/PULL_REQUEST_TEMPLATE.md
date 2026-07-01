<!-- Thanks for contributing to LoRa Serial Link! A few quick things before you open the PR. -->

## What & why

<!-- What does this change do, and what problem does it solve?
     Link any related issue, e.g. "Closes #123". -->

## Scope check

<!-- This project is a reliable point-to-point serial link over LoRa — not a mesh
     or network stack (see CONTRIBUTING.md → Scope). Confirm your change fits that
     scope, or explain why it belongs here. -->

## Testing

- [ ] `pio test -e native` passes
- [ ] Added or updated a link-layer test for any behavior change (or N/A)
- [ ] `pio run -e node_raw` builds
- [ ] Tested on hardware — board + host OS: `______` (or N/A)
- [ ] Our source stays within 80 columns (see CONTRIBUTING.md → Style)

## Sign-off

- [ ] All my commits are signed off with `git commit -s` per the
      [DCO](../CONTRIBUTING.md#sign-your-work-developer-certificate-of-origin)
