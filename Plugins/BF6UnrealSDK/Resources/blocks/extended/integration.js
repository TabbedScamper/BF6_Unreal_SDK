(function (root, factory) {
  if (typeof module === "object" && module.exports)
    module.exports = factory(require("./compiler.js"));
  else root.BF6ExtendedIntegration = factory(root.BF6Extended);
})(typeof globalThis !== "undefined" ? globalThis : this, function (Compiler) {
  "use strict";
  function snapshot(Blockly, workspace, strings = {}) {
    return {
      bf6x: { version: 1, strings },
      mod: Blockly.serialization.workspaces.save(workspace),
    };
  }
  function compileWorkspace(Blockly, workspace, options = {}) {
    const doc = snapshot(Blockly, workspace, options.strings);
    return Compiler.compile(doc, options);
  }
  // Upstream fallback refreshes can redefine every observed type. Keep the
  // installed extension constructors (including dynamic serialization hooks).
  function preserveDefinitions(Blockly, refresh, extraTypes = []) {
    const saved = new Map();
    for (const type of Object.keys(Blockly.Blocks))
      if (type.startsWith("bf6x_") || extraTypes.includes(type))
        saved.set(type, Blockly.Blocks[type]);
    try {
      return refresh();
    } finally {
      for (const [type, definition] of saved) Blockly.Blocks[type] = definition;
    }
  }
  // Existing C++ exportScript keeps only leaf names. Send generated source to
  // the chosen project's src directory. Template scaffolding and build belong
  // to tools.cjs/the host; this adapter does not overwrite a project entrypoint.
  function sourceExportMessage(result, srcDir) {
    if (!result.ok) throw Error("Fix block diagnostics before exporting.");
    const files = {};
    for (const [path, body] of Object.entries(result.files)) {
      if (!path.startsWith("src/") || path.slice(4).includes("/"))
        throw Error(
          "The current Unreal exportScript bridge only accepts flat source files.",
        );
      files[path.slice(4)] = body;
    }
    return { op: "exportScript", dir: srcDir, files };
  }
  function requireNative(doc) {
    if (!Compiler.canExportNative(doc))
      throw Error(
        "This project contains extended blocks. Build TypeScript and send it through the Script editor.",
      );
    return doc;
  }
  return {
    snapshot,
    compileWorkspace,
    preserveDefinitions,
    sourceExportMessage,
    requireNative,
  };
});
