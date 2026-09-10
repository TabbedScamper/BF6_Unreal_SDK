# Exporting blocks to a Portal script

1. Open your mode in the Blocks editor and choose **Export for Portal**.
2. Choose an output folder. The tool checks the source, builds the combined script, and checks the combined result.
3. When the completed export folder opens, upload **bundle.ts** in Portal's script editor and **bundle.strings.json** through its strings upload.

Portal receives one TypeScript script. The generated source modules are working files; they are not separate uploads. Each successful export gets its own folder containing the matching script and strings. A failed export leaves earlier successful folders intact.

The first export requires Node.js 24 or newer and internet access to install the pinned build dependencies. Later exports reuse those dependencies. If Node was installed while Unreal was open, retry the export; if it still cannot be found, restart Unreal so it receives the updated environment.

The export includes every file tab and preserves the rest of the project when editing one rule at a time. Wait until a workspace finishes opening before exporting.

## Source projects

**Export source files** is an advanced option for editing the generated modules yourself. It does not produce an uploadable artifact. Use **Export for Portal** for the combined upload, or work in a scripting project with its build tools installed.

The SDK includes a small scripting starter following Michael De Luca's template layout. A configured full community template remains supported. The regular Script editor checks both the source and the emitted bundle; a bundler exit code alone does not make a build ready to send.

## When export or conversion stops

- Missing Node or npm: install Node.js 24 or newer with npm included, then retry.
- Dependency download failure: restore network access to the npm registry and retry. An interrupted installation is recovered when its old installer process is no longer running.
- Type errors or unresolved imports: use the file and line in the diagnostic. A combined script with unresolved references is not offered as ready.
- Conflicting strings: give each distinct message a distinct key. Two source files cannot assign different text to the same output key.
- Incomplete source export: check folder write access and free space. The status identifies which files could not be written.
- Unsupported TS-to-native-block conversion: keep the mode in the TypeScript editor. Recursive functions and locals shared across overlapping suspended handlers are not safely represented by this native converter. The current block workspace is preserved when conversion stops.

Compilation catches structural and type errors. Test gameplay, UI behavior and engine API calls in a hosted Portal match before sharing a mode broadly.
