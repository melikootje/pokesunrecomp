# Debugging guide

- Build with `-DCMAKE_BUILD_TYPE=Debug`; warnings are errors.
- `romtool info` prints container, ExHeader segments and the service list for a dump.
- `ctest --test-dir build --output-on-failure` runs unit and CLI tests.
- Analysis with the REA MCP server (planned, #4): derive only addresses/names/hashes; never commit bytes.
- Logging/tracing for the runtime will be documented with the HLE work (#9, #10).
