(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.BF6ExtendedBlocks = factory();
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  function install(B, options = {}) {
    const definitions = [];
    const text = (name, value = "") => ({
      type: "field_input",
      name,
      text: value,
    });
    const dropdown = (name, values) => ({
      type: "field_dropdown",
      name,
      options: values.map((v) => (Array.isArray(v) ? v : [v, v])),
    });
    const val = (name, check) =>
      Object.assign({ type: "input_value", name }, check ? { check } : {});
    const body = (name) => ({ type: "input_statement", name });
    function add(
      type,
      message,
      args,
      kind = "statement",
      colour = 210,
      tooltip = "",
    ) {
      const d = {
        type,
        message0: message,
        args0: args,
        colour,
        tooltip,
        inputsInline: false,
      };
      if (kind === "statement") {
        d.previousStatement = null;
        d.nextStatement = null;
      } else if (kind === "value") d.output = null;
      definitions.push(d);
    }
    const eventNames = Object.keys(options.events || {}).filter((n) =>
      /^On[A-Z]/.test(n),
    );
    if (options.standalone) {
      add(
        "Number",
        "%1",
        [{ type: "field_number", name: "NUM", value: 0 }],
        "value",
        65,
        "A finite number.",
      );
      add(
        "Text",
        "%1",
        [text("TEXT", "text")],
        "value",
        160,
        "A script string literal.",
      );
      add(
        "Boolean",
        "%1",
        [
          dropdown("BOOL", [
            ["true", "TRUE"],
            ["false", "FALSE"],
          ]),
        ],
        "value",
        210,
        "A boolean value.",
      );
    }
    if (!eventNames.length)
      eventNames.push(
        "OnGameModeStarted",
        "OnPlayerDeployed",
        "OnPlayerJoinGame",
      );
    add(
      "bf6x_event",
      "When %1 %2",
      [dropdown("EVENT", eventNames), body("DO")],
      "root",
      30,
      "Run this body when Portal raises the event. Event values use their eventPlayer-style names. Multiple handlers for one event run in workspace order and await each other.",
    );
    add(
      "bf6x_state",
      "Shared state %1 type %2 starts as %3",
      [text("NAME", "cash"), text("TYPE", "Map<number, number>"), val("VALUE")],
      "root",
      290,
      "State lives in TypeScript for this script session. Initializers run in saved top-level order; place dependencies first.",
    );
    add(
      "bf6x_function",
      "Function %1 parameters %2 returns %3 %4",
      [
        text("NAME", "award"),
        text("PARAMS", "amount: number"),
        text("RETURNS", "number"),
        body("DO"),
      ],
      "root",
      260,
      "Parameters use name: type, separated by commas. Each call owns its locals. Functions are awaited, including recursion.",
    );
    add(
      "bf6x_let",
      "Local %1 type %2 = %3",
      [text("NAME", "amount"), text("TYPE", "auto"), val("VALUE")],
      "statement",
      290,
      "Declare a local for this invocation and lexical scope. auto asks TypeScript to infer its type.",
    );
    add(
      "bf6x_get",
      "Value of %1",
      [text("NAME", "amount")],
      "value",
      290,
      "Read the nearest local/parameter, or shared state with this name.",
    );
    add(
      "bf6x_set",
      "Set %1 to %2",
      [text("NAME", "amount"), val("VALUE")],
      "statement",
      290,
      "Assign an existing local or shared state.",
    );
    add(
      "bf6x_return",
      "Return %1",
      [val("VALUE")],
      "statement",
      260,
      "Return a value from this function, including from inside a loop or branch.",
    );
    add(
      "bf6x_wait",
      "Wait %1 seconds",
      [val("SECONDS", "Number")],
      "statement",
      30,
      "Await a finite, nonnegative delay. Other event invocations keep their own local values.",
    );
    add(
      "bf6x_if",
      "If %1 then %2 otherwise %3",
      [val("TEST", "Boolean"), body("DO"), body("ELSE")],
      "statement",
      210,
      "Only the selected branch runs. Locals declared inside a branch stay inside it.",
    );
    add(
      "bf6x_while",
      "While %1 do %2",
      [val("TEST", "Boolean"), body("DO")],
      "statement",
      120,
      "Repeat while true. Add a wait when the loop must continue over time; a busy loop can block the game script.",
    );
    add(
      "bf6x_for",
      "Count %1 from %2 to exclusive %3 by %4 do %5",
      [
        text("NAME", "i"),
        val("FROM", "Number"),
        val("TO", "Number"),
        val("STEP", "Number"),
        body("DO"),
      ],
      "statement",
      120,
      "Bounds and step are evaluated once. Positive and negative steps are supported. A zero step throws.",
    );
    add(
      "bf6x_each",
      "For each %1 in %2 do %3",
      [text("NAME", "item"), val("LIST"), body("DO")],
      "statement",
      120,
      "Iterate a TypeScript list in order. The item is scoped to the loop body.",
    );
    add(
      "bf6x_break",
      "Break out of loop",
      [],
      "statement",
      120,
      "Exit the nearest loop.",
    );
    add(
      "bf6x_continue",
      "Continue next iteration",
      [],
      "statement",
      120,
      "Continue the nearest loop, including the counter update in a counting loop.",
    );
    add(
      "bf6x_binary",
      "%1 %2 %3",
      [
        val("A"),
        dropdown("OP", [
          ["+", "ADD"],
          ["−", "SUB"],
          ["×", "MUL"],
          ["÷", "DIV"],
          ["remainder", "MOD"],
          ["equals", "EQ"],
          ["not equal", "NE"],
          ["<", "LT"],
          ["≤", "LE"],
          [">", "GT"],
          ["≥", "GE"],
          ["and", "AND"],
          ["or", "OR"],
          ["otherwise if missing", "COALESCE"],
        ]),
        val("B"),
      ],
      "value",
      210,
      "Script operators. And, or, and otherwise-if-missing evaluate the right input only when needed.",
    );
    add(
      "bf6x_choose",
      "If %1 choose %2 otherwise %3",
      [val("TEST", "Boolean"), val("YES"), val("NO")],
      "value",
      210,
      "Only the chosen expression executes, including any function calls it contains.",
    );
    add(
      "bf6x_not",
      "Not %1",
      [val("VALUE", "Boolean")],
      "value",
      210,
      "Invert a boolean.",
    );
    add(
      "bf6x_to_string",
      "Convert %1 to text",
      [val("VALUE")],
      "value",
      160,
      "Explicitly convert a script value to text.",
    );
    add(
      "bf6x_list_get",
      "List %1 at index %2",
      [val("LIST"), val("INDEX", "Number")],
      "value",
      65,
      "Uses Array.at. Out of range gives undefined; use otherwise-if-missing to supply a fallback.",
    );
    add(
      "bf6x_length",
      "Length of %1",
      [val("LIST")],
      "value",
      65,
      "Length of a TypeScript list or string.",
    );
    add(
      "bf6x_list_push",
      "Append %1 to list %2",
      [val("VALUE"), val("LIST")],
      "statement",
      65,
      "Append one item to a TypeScript list. Nested lists remain nested.",
    );
    add(
      "bf6x_map",
      "New lookup with keys %1 and values %2",
      [text("KEYTYPE", "number"), text("VALUETYPE", "number")],
      "value",
      65,
      "Create a TypeScript Map. This uses no Portal GlobalVariable slots.",
    );
    add(
      "bf6x_map_get",
      "Lookup %1 key %2 default %3",
      [val("MAP"), val("KEY"), val("DEFAULT")],
      "value",
      65,
      "Read a key, or evaluate the fallback if the value is null or undefined.",
    );
    add(
      "bf6x_map_has",
      "Lookup %1 contains key %2",
      [val("MAP"), val("KEY")],
      "value",
      65,
      "Check whether a key exists, even if its value is zero or false.",
    );
    add(
      "bf6x_map_set",
      "In lookup %1 set key %2 to %3",
      [val("MAP"), val("KEY"), val("VALUE")],
      "statement",
      65,
      "Insert or replace one entry.",
    );
    add(
      "bf6x_map_delete",
      "In lookup %1 delete key %2",
      [val("MAP"), val("KEY")],
      "statement",
      65,
      "Remove an entry, for example when a player leaves.",
    );
    add(
      "bf6x_field",
      "Record %1 field %2",
      [val("OBJECT"), text("KEY", "cash")],
      "value",
      65,
      "Read a field. TypeScript validates the record and field type during build.",
    );
    add(
      "bf6x_field_set",
      "In record %1 set field %2 to %3",
      [val("OBJECT"), text("KEY", "cash"), val("VALUE")],
      "statement",
      65,
      "Set an existing typed record field.",
    );
    add(
      "bf6x_string",
      "Message string key %1",
      [text("KEY", "welcome")],
      "value",
      160,
      "Use a key from the project text strings. The build includes the strings bundle.",
    );
    // Reinstallation is intentional and also supports standalone literal
    // definitions. Remove the old constructor before Blockly registers it.
    for (const definition of definitions) delete B.Blocks[definition.type];
    B.defineBlocksWithJsonArray(definitions);
    function dynamic(type, kind, header, fieldName, initial, prefix) {
      B.Blocks[type] = {
        init: function () {
          const self = this;
          this._dynamicReady = false;
          this._dynamicCount = 0;
          const validator = function (v) {
            if (self._dynamicReady) self.resize_(v);
            return v;
          };
          this.appendDummyInput("HEADER").appendField(header);
          if (type === "bf6x_call" || type === "bf6x_call_value")
            this.getInput("HEADER").appendField(
              new B.FieldTextInput("award"),
              "NAME",
            );
          if (type === "bf6x_list")
            this.getInput("HEADER")
              .appendField("item type")
              .appendField(new B.FieldTextInput("number"), "TYPE");
          this.getInput("HEADER")
            .appendField(fieldName === "KEYS" ? "fields" : "inputs")
            .appendField(
              fieldName === "KEYS"
                ? new B.FieldTextInput(initial, validator)
                : new B.FieldNumber(initial, 0, 64, 1, validator),
              fieldName,
            );
          if (kind === "value") this.setOutput(true);
          else {
            this.setPreviousStatement(true);
            this.setNextStatement(true);
          }
          this.setColour(kind === "value" ? 65 : 260);
          this.setTooltip(
            type === "bf6x_record"
              ? "Comma-separated record field names. Each input supplies that field."
              : "Set the input count, then connect each value.",
          );
          this._dynamicReady = true;
          this.resize_(initial);
        },
        resize_: function (raw) {
          const names =
            fieldName === "KEYS"
              ? String(raw)
                  .split(",")
                  .map((s) => s.trim())
                  .filter(Boolean)
              : Array.from(
                  { length: Math.max(0, Math.min(64, Number(raw) || 0)) },
                  (_, i) => String(i + 1),
                );
          for (let i = this._dynamicCount - 1; i >= names.length; i--)
            this.removeInput(prefix + i);
          for (let i = this._dynamicCount; i < names.length; i++)
            this.appendValueInput(prefix + i).appendField(
              names[i],
              "LABEL" + i,
            );
          for (let i = 0; i < Math.min(this._dynamicCount, names.length); i++)
            this.setFieldValue(names[i], "LABEL" + i);
          this._dynamicCount = names.length;
        },
        saveExtraState: function () {
          return { value: this.getFieldValue(fieldName) };
        },
        loadExtraState: function (s) {
          this.resize_(s && s.value !== undefined ? s.value : initial);
        },
      };
    }
    dynamic("bf6x_list", "value", "Script list", "COUNT", 0, "ITEM");
    dynamic("bf6x_record", "value", "Record", "KEYS", "cash, kills", "ITEM");
    dynamic("bf6x_call", "statement", "Call and await", "COUNT", 1, "ARG");
    dynamic("bf6x_call_value", "value", "Result of", "COUNT", 1, "ARG");
    return definitions
      .map((d) => d.type)
      .concat(["bf6x_list", "bf6x_record", "bf6x_call", "bf6x_call_value"]);
  }
  function toolbox() {
    const cat = (name, colour, types) => ({
      kind: "category",
      name,
      colour,
      contents: types.map((type) => ({ kind: "block", type })),
    });
    return {
      kind: "categoryToolbox",
      contents: [
        cat("Events and functions", "30", [
          "bf6x_event",
          "bf6x_function",
          "bf6x_call",
          "bf6x_call_value",
          "bf6x_return",
          "bf6x_wait",
        ]),
        cat("Variables", "290", [
          "bf6x_state",
          "bf6x_let",
          "bf6x_get",
          "bf6x_set",
        ]),
        cat("Logic and loops", "210", [
          "bf6x_if",
          "bf6x_choose",
          "bf6x_binary",
          "bf6x_not",
          "bf6x_while",
          "bf6x_for",
          "bf6x_each",
          "bf6x_break",
          "bf6x_continue",
        ]),
        cat("Lists and records", "65", [
          "bf6x_list",
          "bf6x_list_get",
          "bf6x_list_push",
          "bf6x_length",
          "bf6x_map",
          "bf6x_map_get",
          "bf6x_map_has",
          "bf6x_map_set",
          "bf6x_map_delete",
          "bf6x_record",
          "bf6x_field",
          "bf6x_field_set",
        ]),
        cat("Values and text", "160", [
          "Number",
          "Boolean",
          "Text",
          "bf6x_to_string",
          "bf6x_string",
        ]),
        cat("Portal actions", "10", [
          "GetObjId",
          "GetPlayer",
          "GetTeam",
          "GetUIWidgetName",
          "SetGameModeTargetScore",
          "SetGameModeScore",
          "Message",
          "DisplayNotificationMessage",
          "AddUIText",
          "SetUITextLabel",
          "CreateVector",
          "RandomReal",
        ]),
      ],
    };
  }
  return { install, toolbox };
});
