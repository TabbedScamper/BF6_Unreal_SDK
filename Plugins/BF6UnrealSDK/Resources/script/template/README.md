# Portal script project

Edit `src/index.ts` and add source modules under `src/`.

In the SDK Script editor, install dependencies once and press Build. From a
terminal, run `npm ci`, `npm run check`, then `npm run build`.
Upload `dist/bundle.ts` and its accompanying `dist/bundle.strings.json` to Portal.
Source modules are combined during the build and are not uploaded separately.

This small starter uses Michael De Luca's MIT-licensed
[bf6-portal-bundler](https://github.com/deluca-mike/bf6-portal-bundler)
and `bf6-portal-mod-types`. It follows the `src/` and `dist/` layout of his
scripting template. The full community template remains supported.
