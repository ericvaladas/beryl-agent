# Third-Party Components

This project bundles or fetches the following third-party code.

## Mongoose (vendored)

- Files: `src/mongoose.c`, `src/mongoose.h`
- Source: https://github.com/cesanta/mongoose
- License: GPLv2 (or commercial license from Cesanta)

Mongoose's GPLv2 licensing is the reason this project is distributed under GPLv2.

## Microsoft Detours (fetched at build time)

- Location: `deps/Detours/` (cloned by `make deps`)
- Source: https://github.com/microsoft/Detours
- License: MIT
