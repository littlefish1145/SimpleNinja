The production engine in `src/engine` derives from the bundled Ninja source at
commit `4e4df1e567eb3c1475a51af261cba2bfff60b4be` from the upstream Ninja repository.
Ninja's Apache License 2.0 is included in `src/engine/COPYING`; original source
copyright and license notices are retained. Changes are documented in
`src/engine/README.sninja`.

The engine includes the upstream copies of emhash and rapidhash, with their
original source notices under `src/engine/third_party`. GoogleTest 1.16.0 is
used only to build tests and is fetched by CMake with a pinned SHA256.
