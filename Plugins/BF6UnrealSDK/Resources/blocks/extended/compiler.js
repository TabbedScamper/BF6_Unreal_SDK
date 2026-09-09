/* BF6 extended blocks. Browser + Node; no filesystem or editor dependencies.
 * The saved Blockly document is the authoring model. Storage allocation is
 * ordinary TypeScript lexical scope, never the native Portal variable table.
 * Native Portal serialization remains the upstream converter's separate target.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.BF6Extended = factory();
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  const VERSION = "0.1.0";
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const dict = () => Object.create(null);
  const q = (s) =>
    JSON.stringify(String(s))
      .replace(/\u2028/g, "\\u2028")
      .replace(/\u2029/g, "\\u2029");
  const input = (b, n) =>
    (b &&
      b.inputs &&
      b.inputs[n] &&
      (b.inputs[n].block || b.inputs[n].shadow)) ||
    null;
  const field = (b, n, fallback = "") =>
    b && b.fields && b.fields[n] !== undefined ? b.fields[n] : fallback;
  const off = (b) =>
    b &&
    (b.enabled === false ||
      b.disabled === true ||
      (Array.isArray(b.disabledReasons) && b.disabledReasons.length > 0));
  const slug = (s) =>
    String(s)
      .replace(/[^a-zA-Z0-9_]/g, "_")
      .slice(0, 70) || "unnamed";
  const primitive = {
    Number: "number",
    Bool: "boolean",
    Boolean: "boolean",
    String: "string",
    number: "number",
    boolean: "boolean",
    string: "string",
    unknown: "unknown",
    void: "void",
  };
  const engineTypes = new Set([
    "Player",
    "Team",
    "Vector",
    "UIWidget",
    "Vehicle",
    "Spawner",
    "SpawnPoint",
    "HQ",
    "AreaTrigger",
    "CapturePoint",
    "WorldIcon",
    "InteractPoint",
    "SpatialObject",
    "SFX",
    "VFX",
    "VO",
    "Message",
    "WeaponPackage",
    "Sector",
    "MCOM",
    "FixedCamera",
    "LootSpawner",
    "VehicleSpawner",
    "Object",
    "Variable",
    "Array",
  ]);
  function splitTypes(text) {
    let depth = 0,
      start = 0,
      out = [];
    for (let i = 0; i < text.length; i++) {
      if (text[i] === "<") depth++;
      if (text[i] === ">") depth--;
      if (text[i] === "," && depth === 0) {
        out.push(text.slice(start, i).trim());
        start = i + 1;
      }
    }
    out.push(text.slice(start).trim());
    return out;
  }
  function typeOf(text) {
    text = String(text || "unknown").trim();
    if (own(primitive, text)) return primitive[text];
    if (engineTypes.has(text)) return "mod." + text;
    if (/^mod\.\w+$/.test(text) && engineTypes.has(text.slice(4))) return text;
    const m = /^(List|Map|Record)<([\s\S]+)>$/.exec(text);
    if (m) {
      const args = splitTypes(m[2]).map(typeOf);
      if (m[1] === "List" && args.length === 1 && args[0] !== "void")
        return "Array<" + args[0] + ">";
      if (m[1] === "Map" && args.length === 2 && !args.includes("void"))
        return "Map<" + args.join(", ") + ">";
      if (
        m[1] === "Record" &&
        args.length === 2 &&
        ["number", "string"].includes(args[0]) &&
        args[1] !== "void"
      )
        return "Record<" + args.join(", ") + ">";
    }
    throw Error(
      'Unsupported type "' +
        text +
        '". Use a scalar, Portal type, List<T>, Map<K,V> or Record<K,V>.',
    );
  }
  function parseParameters(text) {
    if (!String(text).trim()) return [];
    return splitTypes(String(text)).map((part) => {
      const at = part.indexOf(":");
      if (at < 1)
        throw Error(
          "Parameters use name: type, for example player: Player, amount: number.",
        );
      return {
        name: part.slice(0, at).trim(),
        type: typeOf(part.slice(at + 1)),
      };
    });
  }
  function scanDocument(doc, visit) {
    const w = (doc && doc.mod) || doc;
    if (!w || !w.blocks || !Array.isArray(w.blocks.blocks))
      throw Error("Expected a Blockly workspace with blocks.blocks.");
    const seen = new Set();
    let n = 0;
    function walk(b, depth, parentDisabled = false) {
      if (!b) return;
      if (typeof b !== "object" || typeof b.type !== "string")
        throw Error("Invalid block record.");
      if (depth > 1000 || ++n > 100000)
        throw Error("Workspace exceeds the compiler traversal limit.");
      if (seen.has(b))
        throw Error(
          "Workspace contains an object cycle or reused block object.",
        );
      seen.add(b);
      const disabled = parentDisabled || off(b);
      visit(b, disabled);
      for (const name of Object.keys(b.inputs || {}))
        walk(input(b, name), depth + 1, disabled);
      walk(b.next && b.next.block, depth + 1, parentDisabled);
    }
    w.blocks.blocks.forEach((b) => walk(b, 0));
    return w;
  }
  function compile(doc, options = {}) {
    options = Object.assign({}, options, {
      strings: options.strings || (doc && doc.bf6x && doc.bf6x.strings) || {},
    });
    const diagnostics = [],
      lines = [],
      mapping = [],
      consumed = new Set();
    const catalog = options.catalog || {},
      events = options.events || {};
    const states = new Map(),
      functions = new Map(),
      registrations = [];
    let serial = 0,
      depth = 0,
      workspace;
    const extensions = options.extensions || {};
    function diagnostic(b, code, message, severity = "error") {
      diagnostics.push({
        severity,
        code,
        blockId: (b && b.id) || null,
        blockType: (b && b.type) || null,
        message,
      });
    }
    function error(b, code, message) {
      diagnostic(b, code, message);
      return { code: "undefined", type: "unknown" };
    }
    function line(b, text) {
      lines.push("    ".repeat(depth) + text);
      if (b && b.id)
        mapping.push({
          file: "src/index.ts",
          line: lines.length,
          blockId: b.id,
        });
    }
    function safeType(b, t) {
      try {
        return typeOf(t);
      } catch (e) {
        error(b, "E_TYPE", e.message);
        return "unknown";
      }
    }
    function count(b) {
      const n = Number(
        field(b, "COUNT", (b.extraState && b.extraState.count) || 0),
      );
      if (!Number.isInteger(n) || n < 0 || n > 64) {
        error(b, "E_ARITY", "Input count must be an integer from 0 to 64.");
        return 0;
      }
      return n;
    }
    function name(b) {
      const n = String(field(b, "NAME")).trim();
      if (!n) error(b, "E_NAME", "Give this item a name.");
      return n;
    }
    function scope(parent, overrides = {}) {
      return Object.assign(
        {
          parent,
          bindings: new Map(),
          allowAwait: true,
          returnType: "void",
          loop: 0,
          eventParams: null,
          params: [],
          stateInit: false,
          inFunction: false,
        },
        parent
          ? {
              allowAwait: parent.allowAwait,
              returnType: parent.returnType,
              loop: parent.loop,
              eventParams: parent.eventParams,
              params: parent.params,
              stateInit: parent.stateInit,
              inFunction: parent.inFunction,
            }
          : null,
        overrides,
      );
    }
    function lookup(ctx, n, b) {
      for (let c = ctx; c; c = c.parent)
        if (c.bindings.has(n)) return c.bindings.get(n);
      if (states.has(n)) return states.get(n);
      error(b, "E_UNBOUND", '"' + n + '" is not declared in this scope.');
      return { code: "undefined", type: "unknown", mutable: false };
    }
    function bind(ctx, b, n, type, mutable = true) {
      if (!n) {
        error(b, "E_NAME", "A declaration needs a name.");
        n = "unnamed";
      }
      if (ctx.bindings.has(n))
        error(
          b,
          "E_DUPLICATE",
          '"' + n + '" is already declared in this scope.',
        );
      const rec = { code: "bf6x_v" + serial++ + "_" + slug(n), type, mutable };
      ctx.bindings.set(n, rec);
      return rec;
    }
    function requireType(b, value, expected) {
      if (value.type === "unknown" || expected === "unknown") return;
      if (value.type !== expected)
        error(
          b,
          "E_TYPE",
          "Expected " + expected + " but got " + value.type + ".",
        );
    }
    function value(b, n, ctx) {
      const v = input(b, n);
      if (!v || off(v))
        return error(b, "E_INPUT", "Connect the " + n + " input.");
      return expr(v, ctx);
    }
    function nativeArgs(b, ctx) {
      const names = Object.keys(b.inputs || {}).filter((n) =>
        /^VALUE-\d+$/.test(n),
      );
      let max = -1;
      names.forEach((n) => {
        if (input(b, n)) max = Math.max(max, Number(n.slice(6)));
      });
      const args = [];
      for (let i = 0; i <= max; i++) args.push(value(b, "VALUE-" + i, ctx));
      return args;
    }
    function generic(b, ctx, kind) {
      const entry = catalog[b.type];
      if (!entry || entry.kind !== kind)
        return error(
          b,
          "E_UNSUPPORTED",
          "No " + kind + " generator is registered for " + b.type + ".",
        );
      if (
        [
          "Skip",
          "SkipIf",
          "Abort",
          "AbortIf",
          "WaitUntil",
          "ChaseVariableAtRate",
          "ChaseVariableOverTime",
          "StopChasingVariable",
        ].includes(b.type)
      )
        return error(
          b,
          "E_NATIVE_CONTROL",
          b.type +
            " requires native Portal execution semantics; use extended control flow or the native export target.",
        );
      if (!/^[A-Za-z_]\w*$/.test(b.type))
        return error(b, "E_API", "Invalid API name.");
      const args = nativeArgs(b, ctx);
      const indices = entry.inputs || [];
      if (args.length > indices.length)
        error(b, "E_ARITY", "Too many inputs for " + b.type + ".");
      const ret = own(primitive, entry.ret)
        ? primitive[entry.ret]
        : engineTypes.has(entry.ret)
          ? "mod." + entry.ret
          : "unknown";
      return {
        code: "mod." + b.type + "(" + args.map((a) => a.code).join(", ") + ")",
        type: ret,
      };
    }
    function call(b, ctx) {
      const target = functions.get(name(b));
      if (!target)
        return error(b, "E_FUNCTION", 'Unknown function "' + name(b) + '".');
      if (!ctx.allowAwait)
        return error(
          b,
          "E_INITIALIZER",
          "Function calls cannot run inside shared-state initializers. Initialize asynchronously in an event.",
        );
      const n = count(b);
      if (n !== target.params.length)
        error(
          b,
          "E_ARITY",
          "Function " +
            name(b) +
            " expects " +
            target.params.length +
            " arguments.",
        );
      const args = [];
      for (let i = 0; i < n; i++) {
        const v = value(b, "ARG" + i, ctx);
        if (target.params[i]) requireType(b, v, target.params[i].type);
        args.push(v.code);
      }
      return {
        code: "(await " + target.code + "(" + args.join(", ") + "))",
        type: target.returnType,
      };
    }
    function expr(b, ctx) {
      if (!b) return error(null, "E_INPUT", "An expression is missing.");
      consumed.add(b.id);
      if (off(b))
        return error(b, "E_DISABLED", "A required value is disabled.");
      if (b.next && b.next.block)
        error(b, "E_SHAPE", "A value block cannot have a next statement.");
      const t = b.type;
      let a, c;
      switch (t) {
        case "Number": {
          const n = Number(field(b, "NUM", 0));
          if (!Number.isFinite(n))
            return error(b, "E_NUMBER", "Use a finite number.");
          return { code: String(n), type: "number" };
        }
        case "Text":
          return { code: q(field(b, "TEXT")), type: "string" };
        case "Boolean":
          return {
            code:
              String(field(b, "BOOL", "FALSE")).toUpperCase() === "TRUE"
                ? "true"
                : "false",
            type: "boolean",
          };
        case "bf6x_get":
          return lookup(ctx, name(b), b);
        case "bf6x_call_value":
          return call(b, ctx);
        case "bf6x_binary": {
          a = value(b, "A", ctx);
          c = value(b, "B", ctx);
          const op = String(field(b, "OP"));
          const ops = {
            ADD: "+",
            SUB: "-",
            MUL: "*",
            DIV: "/",
            MOD: "%",
            EQ: "===",
            NE: "!==",
            LT: "<",
            LE: "<=",
            GT: ">",
            GE: ">=",
            AND: "&&",
            OR: "||",
            COALESCE: "??",
          };
          if (!own(ops, op)) return error(b, "E_OPERATOR", "Unknown operator.");
          if (["AND", "OR"].includes(op)) {
            requireType(b, a, "boolean");
            requireType(b, c, "boolean");
          } else if (
            ["SUB", "MUL", "DIV", "MOD", "LT", "LE", "GT", "GE"].includes(op)
          ) {
            requireType(b, a, "number");
            requireType(b, c, "number");
          } else if (
            op === "ADD" &&
            a.type !== "unknown" &&
            c.type !== "unknown" &&
            !(a.type === "number" && c.type === "number") &&
            !(a.type === "string" && c.type === "string")
          )
            error(
              b,
              "E_TYPE",
              "Add two numbers or join two strings; convert values explicitly.",
            );
          const type = [
            "EQ",
            "NE",
            "LT",
            "LE",
            "GT",
            "GE",
            "AND",
            "OR",
          ].includes(op)
            ? "boolean"
            : op === "ADD" && a.type === "string"
              ? "string"
              : op === "COALESCE"
                ? c.type
                : "number";
          return {
            code: "(" + a.code + " " + ops[op] + " " + c.code + ")",
            type,
          };
        }
        case "bf6x_choose": {
          const cond = value(b, "TEST", ctx),
            yes = value(b, "YES", ctx),
            no = value(b, "NO", ctx);
          requireType(b, cond, "boolean");
          if (
            yes.type !== no.type &&
            yes.type !== "unknown" &&
            no.type !== "unknown"
          )
            error(b, "E_TYPE", "Both branches must have the same type.");
          return {
            code: "(" + cond.code + " ? " + yes.code + " : " + no.code + ")",
            type: yes.type,
          };
        }
        case "bf6x_not":
          a = value(b, "VALUE", ctx);
          requireType(b, a, "boolean");
          return { code: "(!" + a.code + ")", type: "boolean" };
        case "bf6x_list": {
          const type = safeType(b, field(b, "TYPE", "number"));
          const args = [];
          for (let i = 0; i < count(b); i++) {
            a = value(b, "ITEM" + i, ctx);
            requireType(b, a, type);
            args.push(a.code);
          }
          return {
            code: "([" + args.join(", ") + "] as Array<" + type + ">)",
            type: "Array<" + type + ">",
          };
        }
        case "bf6x_list_get":
          a = value(b, "LIST", ctx);
          c = value(b, "INDEX", ctx);
          requireType(b, c, "number");
          if (!/^Array</.test(a.type) && a.type !== "unknown")
            error(b, "E_TYPE", "List get needs a script list.");
          return {
            code: "(" + a.code + ").at(" + c.code + ")",
            type: "unknown",
          };
        case "bf6x_length":
          a = value(b, "LIST", ctx);
          if (
            !/^Array</.test(a.type) &&
            !["unknown", "string"].includes(a.type)
          )
            error(b, "E_TYPE", "Length needs a script list or string.");
          return { code: "(" + a.code + ").length", type: "number" };
        case "bf6x_map": {
          const k = safeType(b, field(b, "KEYTYPE", "number")),
            v = safeType(b, field(b, "VALUETYPE", "number"));
          return {
            code: "new Map<" + k + ", " + v + ">()",
            type: "Map<" + k + ", " + v + ">",
          };
        }
        case "bf6x_map_get": {
          const map = value(b, "MAP", ctx),
            key = value(b, "KEY", ctx),
            fallback = value(b, "DEFAULT", ctx);
          const types = mapTypes(b, map);
          requireType(b, key, types[0]);
          requireType(b, fallback, types[1]);
          return {
            code:
              "((" +
              map.code +
              ").get(" +
              key.code +
              ") ?? " +
              fallback.code +
              ")",
            type: types[1],
          };
        }
        case "bf6x_map_has": {
          const map = value(b, "MAP", ctx),
            key = value(b, "KEY", ctx);
          requireType(b, key, mapTypes(b, map)[0]);
          return {
            code: "(" + map.code + ").has(" + key.code + ")",
            type: "boolean",
          };
        }
        case "bf6x_record": {
          const keys = String(field(b, "KEYS"))
            .split(",")
            .map((s) => s.trim())
            .filter(Boolean);
          if (new Set(keys).size !== keys.length)
            error(b, "E_DUPLICATE", "Record keys must be unique.");
          const args = keys.map(
            (k, i) => "[" + q(k) + "]: " + value(b, "ITEM" + i, ctx).code,
          );
          return { code: "({" + args.join(", ") + "})", type: "unknown" };
        }
        case "bf6x_field": {
          a = value(b, "OBJECT", ctx);
          const key = String(field(b, "KEY"));
          if (!key) error(b, "E_NAME", "Choose a record field.");
          return { code: "(" + a.code + ")[" + q(key) + "]", type: "unknown" };
        }
        case "bf6x_string": {
          const key = String(field(b, "KEY"));
          if (!own(options.strings || {}, key))
            error(b, "E_STRING", 'Missing text string "' + key + '".');
          return { code: "mod.stringkeys[" + q(key) + "]", type: "unknown" };
        }
        case "bf6x_to_string":
          a = value(b, "VALUE", ctx);
          return { code: "String(" + a.code + ")", type: "string" };
        case "subroutineArgumentBlock": {
          const n = Number(field(b, "ARGUMENT_INDEX", 0));
          if (!ctx.params[n])
            return error(
              b,
              "E_ARGUMENT",
              "This function has no argument " + n + ".",
            );
          return ctx.params[n];
        }
      }
      if (/^Event[A-Z]/.test(t)) {
        const eventName = "event" + t.slice(5);
        if (!ctx.eventParams || !ctx.eventParams.has(eventName))
          return error(
            b,
            "E_EVENT_VALUE",
            t + " is unavailable here. Pass it into a function explicitly.",
          );
        return ctx.eventParams.get(eventName);
      }
      if (/Item$/.test(t) && field(b, "VALUE-0") && field(b, "VALUE-1")) {
        if (!catalog[t])
          return error(b, "E_ENUM", "Unknown Portal enum block " + t + ".");
        const ns = String(field(b, "VALUE-0")),
          member = String(field(b, "VALUE-1"));
        if (!/^\w+$/.test(ns) || !/^\w+$/.test(member))
          return error(b, "E_ENUM", "Invalid Portal enum value.");
        return { code: "mod." + ns + "." + member, type: "unknown" };
      }
      if (extensions[t]) return extension(b, ctx, "value");
      if (
        [
          "variableReferenceBlock",
          "GetVariable",
          "GlobalVariable",
          "ObjectVariable",
          "subroutineInstanceBlock",
          "CurrentArrayElement",
          "FilteredArray",
          "MappedArray",
          "SortedArray",
          "IsTrueForAll",
          "IsTrueForAny",
        ].includes(t)
      )
        return error(
          b,
          "E_NATIVE_STATE",
          "Use extended variables, lists and functions in a script project. Native workspace semantics remain on the compatibility target.",
        );
      return generic(b, ctx, "value");
    }
    function mapTypes(b, map) {
      const m = /^Map<([\s\S]+)>$/.exec(map.type);
      if (!m) {
        if (map.type !== "unknown")
          error(b, "E_TYPE", "This operation needs a script Map.");
        return ["unknown", "unknown"];
      }
      return splitTypes(m[1]);
    }
    function extension(b, ctx, kind) {
      const def = extensions[b.type];
      if (!def || def.kind !== kind || typeof def.emit !== "function")
        return error(
          b,
          "E_EXTENSION",
          "Extension " + b.type + " has no " + kind + " generator.",
        );
      try {
        const result = def.emit(b, {
          field: (n, d) => field(b, n, d),
          value: (n) => value(b, n, ctx),
          line: (text) => line(b, text),
          body: (n) => statements(input(b, n), scope(ctx)),
          typeOf,
        });
        if (
          kind === "value" &&
          (!result ||
            typeof result.code !== "string" ||
            typeof result.type !== "string")
        )
          return error(
            b,
            "E_EXTENSION",
            "Value generator must return {code,type}.",
          );
        return result;
      } catch (e) {
        return error(b, "E_EXTENSION", e.message);
      }
    }
    function statements(first, ctx) {
      for (let b = first; b; b = b.next && b.next.block) {
        if (off(b)) continue;
        consumed.add(b.id);
        const t = b.type;
        line(b, "// block " + String(b.id || "").replace(/[\r\n]/g, " "));
        switch (t) {
          case "bf6x_let": {
            const init = value(b, "VALUE", ctx),
              declared = String(field(b, "TYPE", "auto"));
            const type =
              declared === "auto" ? init.type : safeType(b, declared);
            if (type === "void")
              error(b, "E_TYPE", "A local cannot have type void.");
            if (declared !== "auto") requireType(b, init, type);
            const v = bind(ctx, b, name(b), type);
            line(
              b,
              "let " +
                v.code +
                (declared === "auto" ? "" : ": " + type) +
                " = " +
                init.code +
                ";",
            );
            break;
          }
          case "bf6x_set": {
            const target = lookup(ctx, name(b), b),
              v = value(b, "VALUE", ctx);
            if (!target.mutable)
              error(b, "E_ASSIGN", "This binding is read-only.");
            requireType(b, v, target.type);
            line(b, target.code + " = " + v.code + ";");
            break;
          }
          case "bf6x_return": {
            if (!ctx.inFunction)
              error(b, "E_RETURN", "Return values belong inside functions.");
            const v = value(b, "VALUE", ctx);
            requireType(b, v, ctx.returnType);
            line(b, "return " + v.code + ";");
            break;
          }
          case "bf6x_call":
            line(b, call(b, ctx).code + ";");
            break;
          case "Wait":
          case "bf6x_wait": {
            const v = value(b, t === "Wait" ? "VALUE-0" : "SECONDS", ctx);
            requireType(b, v, "number");
            line(b, "await mod.Wait(bf6x_nonnegative(" + v.code + "));");
            break;
          }
          case "bf6x_if": {
            const v = value(b, "TEST", ctx);
            requireType(b, v, "boolean");
            line(b, "if (" + v.code + ") {");
            depth++;
            statements(input(b, "DO"), scope(ctx));
            depth--;
            line(b, "} else {");
            depth++;
            statements(input(b, "ELSE"), scope(ctx));
            depth--;
            line(b, "}");
            break;
          }
          case "bf6x_while": {
            const v = value(b, "TEST", ctx);
            requireType(b, v, "boolean");
            line(b, "while (" + v.code + ") {");
            depth++;
            statements(input(b, "DO"), scope(ctx, { loop: ctx.loop + 1 }));
            depth--;
            line(b, "}");
            break;
          }
          case "bf6x_for": {
            const from = value(b, "FROM", ctx),
              to = value(b, "TO", ctx),
              step = value(b, "STEP", ctx);
            [from, to, step].forEach((v) => requireType(b, v, "number"));
            const child = scope(ctx, { loop: ctx.loop + 1 });
            const it = bind(child, b, name(b), "number");
            const start = "bf6x_start" + serial++,
              end = "bf6x_end" + serial++,
              delta = "bf6x_step" + serial++;
            line(b, "{");
            depth++;
            line(b, "const " + start + " = bf6x_finite(" + from.code + ");");
            line(b, "const " + end + " = bf6x_finite(" + to.code + ");");
            line(b, "const " + delta + " = bf6x_step(" + step.code + ");");
            line(
              b,
              "for (let " +
                it.code +
                " = " +
                start +
                "; " +
                delta +
                " > 0 ? " +
                it.code +
                " < " +
                end +
                " : " +
                it.code +
                " > " +
                end +
                "; " +
                it.code +
                " += " +
                delta +
                ") {",
            );
            depth++;
            statements(input(b, "DO"), child);
            depth--;
            line(b, "}");
            depth--;
            line(b, "}");
            break;
          }
          case "bf6x_each": {
            const list = value(b, "LIST", ctx);
            const m = /^Array<(.+)>$/.exec(list.type);
            if (!m && list.type !== "unknown")
              error(b, "E_TYPE", "For each needs a script list.");
            const child = scope(ctx, { loop: ctx.loop + 1 }),
              it = bind(child, b, name(b), m ? m[1] : "unknown", false);
            line(b, "for (const " + it.code + " of " + list.code + ") {");
            depth++;
            statements(input(b, "DO"), child);
            depth--;
            line(b, "}");
            break;
          }
          case "Break":
          case "Continue":
          case "bf6x_break":
          case "bf6x_continue":
            if (!ctx.loop)
              error(
                b,
                "E_LOOP",
                "Break and continue require an enclosing loop.",
              );
            line(b, /continue/i.test(t) ? "continue;" : "break;");
            break;
          case "bf6x_list_push": {
            const list = value(b, "LIST", ctx),
              v = value(b, "VALUE", ctx),
              m = /^Array<(.+)>$/.exec(list.type);
            if (m) requireType(b, v, m[1]);
            else if (list.type !== "unknown")
              error(b, "E_TYPE", "Append needs a script list.");
            line(b, "(" + list.code + ").push(" + v.code + ");");
            break;
          }
          case "bf6x_map_set": {
            const map = value(b, "MAP", ctx),
              key = value(b, "KEY", ctx),
              v = value(b, "VALUE", ctx),
              types = mapTypes(b, map);
            requireType(b, key, types[0]);
            requireType(b, v, types[1]);
            line(
              b,
              "(" + map.code + ").set(" + key.code + ", " + v.code + ");",
            );
            break;
          }
          case "bf6x_map_delete": {
            const map = value(b, "MAP", ctx),
              key = value(b, "KEY", ctx);
            requireType(b, key, mapTypes(b, map)[0]);
            line(b, "(" + map.code + ").delete(" + key.code + ");");
            break;
          }
          case "bf6x_field_set": {
            const object = value(b, "OBJECT", ctx),
              v = value(b, "VALUE", ctx);
            line(
              b,
              "(" +
                object.code +
                ")[" +
                q(field(b, "KEY")) +
                "] = " +
                v.code +
                ";",
            );
            break;
          }
          case "actionComment":
            line(b, "// " + String(field(b, "TEXT")).replace(/[\r\n]/g, " "));
            break;
          default:
            if (extensions[t]) extension(b, ctx, "statement");
            else if (
              /^bf6x_/.test(t) ||
              [
                "If",
                "ForVariable",
                "While",
                "SetVariable",
                "SetVariableAtIndex",
                "subroutineInstanceBlock",
              ].includes(t)
            )
              error(
                b,
                "E_UNSUPPORTED",
                "Use the extended control/state blocks here; " +
                  t +
                  " is not an extended statement.",
              );
            else line(b, generic(b, ctx, "statement").code + ";");
        }
      }
    }
    try {
      const ids = new Set();
      workspace = scanDocument(doc, (b, disabled) => {
        if (!b.id || typeof b.id !== "string")
          diagnostic(b, "E_ID", "Every block needs a stable string ID.");
        else if (ids.has(b.id)) diagnostic(b, "E_ID", "Duplicate block ID.");
        ids.add(b.id);
        const comment = b.icons && b.icons.comment && b.icons.comment.text;
        if (!disabled && comment && /NOT CONVERTED/.test(comment))
          diagnostic(
            b,
            "E_INCOMPLETE",
            "This block carries unresolved source conversion.",
          );
      });
      const tops = workspace.blocks.blocks.filter((b) => !off(b));
      const extended = tops.some((b) => /^bf6x_/.test(b.type));
      if (!extended) {
        if (!options.legacy)
          return {
            ok: false,
            diagnostics: [
              {
                severity: "error",
                code: "E_LEGACY",
                message:
                  "Pass the existing BF6Convert instance to export a native Portal workspace.",
              },
            ],
            files: {},
          };
        scanDocument(doc, (b, disabled) => {
          if (disabled) return;
          if (/^bf6x_/.test(b.type))
            diagnostic(
              b,
              "E_TARGET",
              "Extended blocks need extended event/function roots.",
            );
          if (
            /^missing/.test(b.type) ||
            (!catalog[b.type] &&
              ![
                "modBlock",
                "ruleBlock",
                "subroutineBlock",
                "subroutineInstanceBlock",
                "subroutineArgumentBlock",
                "conditionBlock",
                "variableReferenceBlock",
                "Number",
                "Text",
                "Boolean",
              ].includes(b.type) &&
              !/^Event[A-Z]/.test(b.type))
          )
            diagnostic(
              b,
              "E_UNSUPPORTED",
              "Unknown native block " + b.type + ".",
            );
        });
        if (diagnostics.some((d) => d.severity === "error"))
          return { ok: false, diagnostics, files: {} };
        const legacy = options.legacy.blocksToTs(doc, {});
        for (const u of legacy.report.unconvertible || [])
          diagnostic(null, "E_LEGACY", u.construct);
        for (const w of legacy.report.warnings || [])
          diagnostic(null, "W_LEGACY", w, "warning");
        const files = {};
        for (const n of Object.keys(legacy.files))
          files["src/" + n] = legacy.files[n];
        if (diagnostics.some((d) => d.severity === "error"))
          return { ok: false, diagnostics, files: {} };
        return {
          ok: true,
          mode: "portal-compatibility",
          files,
          diagnostics,
          sourceMap: [],
          version: VERSION,
        };
      }
      if (doc.bf6x && doc.bf6x.version !== 1)
        diagnostic(
          null,
          "E_VERSION",
          "Unsupported extended project schema version.",
        );
      for (const b of tops) {
        consumed.add(b.id);
        if (b.next && b.next.block)
          error(b, "E_ROOT", "Declarations cannot be chained.");
        if (b.type === "bf6x_function") {
          let params = [];
          try {
            params = parseParameters(field(b, "PARAMS"));
          } catch (e) {
            error(b, "E_PARAMETERS", e.message);
          }
          const n = name(b);
          if (functions.has(n))
            error(b, "E_DUPLICATE", "Duplicate function " + n + ".");
          functions.set(n, {
            block: b,
            code: "bf6x_f" + serial++ + "_" + slug(n),
            params,
            returnType: safeType(b, field(b, "RETURNS", "void")),
          });
        } else if (b.type === "bf6x_event") {
          const event = String(field(b, "EVENT"));
          if (!own(events, event) || !/^On[A-Z]/.test(event))
            error(
              b,
              "E_EVENT",
              "Choose a known On... Portal event. Ongoing rules use the native target; extended repeating tasks use an event with a waiting loop.",
            );
          else
            registrations.push({
              block: b,
              event,
              params: events[event].params || [],
            });
        } else if (b.type !== "bf6x_state")
          error(
            b,
            "E_ROOT",
            "Place executable blocks inside an extended event or function. Native and extended roots cannot be mixed.",
          );
      }
      line(
        null,
        "// Generated by BF6 Extended Blocks " +
          VERSION +
          ". Edit the saved visual project.",
      );
      line(
        null,
        "// Engine calls retain their documented semantics. Script state uses ordinary TypeScript storage.",
      );
      line(
        null,
        'function bf6x_finite(value: number): number { if (!Number.isFinite(value)) throw new Error("Expected a finite number"); return value; }',
      );
      line(
        null,
        'function bf6x_step(value: number): number { bf6x_finite(value); if (value === 0) throw new Error("Loop step cannot be zero"); return value; }',
      );
      line(
        null,
        'function bf6x_nonnegative(value: number): number { bf6x_finite(value); if (value < 0) throw new Error("Wait duration cannot be negative"); return value; }',
      );
      for (const b of tops.filter((b) => b.type === "bf6x_state")) {
        const n = name(b);
        if (states.has(n))
          error(b, "E_DUPLICATE", "Duplicate shared state " + n + ".");
        const initial = value(
          b,
          "VALUE",
          scope(null, { allowAwait: false, stateInit: true }),
        );
        const type = safeType(b, field(b, "TYPE", "number"));
        if (type === "void")
          error(b, "E_TYPE", "Shared state cannot have type void.");
        requireType(b, initial, type);
        const rec = {
          code: "bf6x_s" + serial++ + "_" + slug(n),
          type,
          mutable: true,
        };
        states.set(n, rec);
        line(b, "let " + rec.code + ": " + type + " = " + initial.code + ";");
      }
      for (const f of functions.values()) {
        const ctx = scope(null, { returnType: f.returnType, inFunction: true });
        ctx.params = f.params.map((p) => bind(ctx, f.block, p.name, p.type));
        line(
          f.block,
          "async function " +
            f.code +
            "(" +
            ctx.params.map((p) => p.code + ": " + p.type).join(", ") +
            "): Promise<" +
            f.returnType +
            "> {",
        );
        depth++;
        statements(input(f.block, "DO"), ctx);
        depth--;
        line(f.block, "}");
      }
      const grouped = new Map();
      for (const reg of registrations) {
        if (!grouped.has(reg.event)) grouped.set(reg.event, []);
        grouped.get(reg.event).push(reg);
      }
      for (const [event, regs] of grouped) {
        const ps = regs[0].params;
        line(
          regs[0].block,
          "export async function " +
            event +
            "(" +
            ps
              .map((p) => p.name + ": " + safeType(regs[0].block, p.type))
              .join(", ") +
            "): Promise<void> {",
        );
        depth++;
        for (const reg of regs) {
          line(reg.block, "{");
          depth++;
          const ctx = scope(null, { eventParams: new Map() });
          for (const p of ps) {
            const rec = {
              code: p.name,
              type: safeType(reg.block, p.type),
              mutable: false,
            };
            ctx.bindings.set(p.name, rec);
            ctx.eventParams.set(p.name, rec);
          }
          statements(input(reg.block, "DO"), ctx);
          depth--;
          line(reg.block, "}");
        }
        depth--;
        line(regs[0].block, "}");
      }
      function coverage(b) {
        if (!b) return;
        if (!off(b)) {
          if (!consumed.has(b.id))
            diagnostic(
              b,
              "E_UNUSED_INPUT",
              "This connected block is not consumed by its parent.",
            );
          for (const n of Object.keys(b.inputs || {})) coverage(input(b, n));
        }
        coverage(b.next && b.next.block);
      }
      tops.forEach(coverage);
      if (!registrations.length)
        diagnostic(
          null,
          "E_EVENT",
          "Add at least one event so the project can execute.",
        );
    } catch (e) {
      diagnostic(null, "E_WORKSPACE", e.message);
    }
    const ok = !diagnostics.some((d) => d.severity === "error");
    const files = ok
      ? {
          "src/index.ts": lines.join("\n") + "\n",
          "src/strings.json":
            JSON.stringify(options.strings || {}, null, 2) + "\n",
        }
      : {};
    return {
      ok,
      mode: "extended-typescript",
      version: VERSION,
      files,
      diagnostics,
      sourceMap: mapping,
      statistics: {
        scriptStates: states.size,
        functions: functions.size,
        eventHandlers: registrations.length,
        portalVariableSlots: 0,
      },
    };
  }
  function canExportNative(doc) {
    let found = false;
    try {
      scanDocument(doc, (b) => {
        if (/^bf6x_/.test(b.type)) found = true;
      });
    } catch (e) {
      return false;
    }
    return !found;
  }
  return {
    VERSION,
    compile,
    canExportNative,
    typeOf,
    parseParameters,
    scanDocument,
  };
});
