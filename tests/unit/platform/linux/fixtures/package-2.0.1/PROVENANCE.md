These are verbatim public Arch `.INSTALL` files from the repositories' exact
`2.0.1` tags. Keep them unchanged when migrating shared implementation/tests.

- `vibepollo.install`: Nonary/Vibepollo commit
  `66e169c325dab73e4e780fe34ac3c613af4cdf52`.
- `vibeshine.install`: Vibeshine commit
  `f078104b2b9224f080e503271dcea2a96fb573c1`.

Tests extract only known function definitions and redirect their filesystem
operations to disposable fixtures. No installed historical hook is executed.
