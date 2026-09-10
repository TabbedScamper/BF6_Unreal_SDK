// ============================================================================
// BF6 BLOCKS - the tool's own Blockly workspace.
//
// One file, two homes: the editor page inside the SDK (browser, full UI) and
// node, where the round-trip test loads it headless to prove that a workspace
// saved here is the same workspace the Portal site saves. Everything above the
// UI section must therefore stay free of document/window.
//
// The five mutator types (modBlock, ruleBlock, subroutineBlock,
// subroutineInstanceBlock, variableReferenceBlock) plus If's else/elseif and
// subroutineArgumentBlock are written by hand here, because the site builds
// them in code and a captured definition object cannot carry their behaviour.
// Their extraState and their field and input names are copied from a real
// Portal project, not invented.
// ============================================================================
(function (global, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else global.BF6Blocks = factory();
})(typeof window !== 'undefined' ? window : globalThis, function () {
  'use strict';

  var B = null;              // the Blockly namespace, handed in by attach()
  var API = {};

  var state = {
    version: '1.0.0',
    loading: 0,              // >0 while we deserialize: no model side effects
    applying: 0,             // >0 while we apply a remote change: emit nothing
    seqOut: 0,
    seqIn: 0,
    lastError: '',
    connected: false,        // the site page answered
    definitionsSource: 'none',   // live | cache | fallback | none
    types: {},               // type -> definition used
    units: {},               // sync unit id -> serialized string
    objIds: [],              // placed objects from Unreal: {id,name,type}
    sceneSelected: [],       // ObjIds selected in the Unreal viewport
    catalogue: null,         // the site's mod catalogue, whole and unconverted
    snippets: []
  };
  API.state = state;

  // ---- the Get<Kind> blocks whose Number input is an ObjId -----------------
  var OBJID_BLOCKS = {
    GetCapturePoint: 'CapturePoint',
    GetSpawnPoint: 'SpawnPoint',
    GetHQ: 'HQ',
    GetVehicleSpawner: 'VehicleSpawner',
    GetSector: 'Sector',
    GetAreaTrigger: 'AreaTrigger',
    GetMCOM: 'MCOM',
    GetSpatialObject: 'SpatialObject'
  };
  API.OBJID_BLOCKS = OBJID_BLOCKS;

  function attach(blockly) { B = blockly; }
  API.attach = attach;

  // =========================================================================
  // Fields
  // =========================================================================

  // The site serializes a variable reference as {"id":"<varId>"} and nothing
  // else. Blockly's own field adds name and type under full serialization, so
  // the field is subclassed to hold the site's shape exactly. initModel is also
  // suppressed while loading, or every reference block would mint a stray
  // "item" variable before its real one is read back.
  function defineFields() {
    if (API._fieldsDone) return;
    var Base = B.FieldVariable;
    class BF6FieldVariable extends Base {
      initModel() {
        if (state.loading > 0 && !this.getVariable()) return;
        super.initModel();
      }
      saveState() {
        var v = this.getVariable();
        return { id: v ? v.getId() : null };
      }
      static fromJson(options) {
        return new BF6FieldVariable(options.variable, undefined,
          options.variableTypes, options.defaultType);
      }
    }
    API.FieldVariable = BF6FieldVariable;
    try {
      B.registry.register(B.registry.Type.FIELD, 'field_variable',
        BF6FieldVariable, true);
    } catch (e) { /* older registry: our own blocks still use the class */ }
    API._fieldsDone = true;
  }

  // A dropdown that never refuses a value it has not heard of. Portal ships
  // 629 block types whose option lists we only see when the site is open, and
  // a strict dropdown would throw the whole workspace away over one unknown
  // enum. Unknown values are added to the list instead.
  function looseDropdown(options, value) {
    var opts = (options && options.length) ? options.slice() : [];
    var rows = opts.map(function (o) {
      return Array.isArray(o) ? [String(o[0]), String(o[1])] : [String(o), String(o)];
    });
    var f = new B.FieldDropdown(function () {
      var list = rows.slice();
      var cur = f && f.getValue();
      if (cur && !list.some(function (r) { return r[1] === cur; })) list.unshift([cur, cur]);
      if (!list.length) list.push(['', '']);
      return list;
    });
    f.doClassValidation_ = function (v) { return v === undefined ? null : String(v); };
    if (value !== undefined && value !== null) { try { f.setValue(String(value)); } catch (e) {} }
    return f;
  }
  API.looseDropdown = looseDropdown;

  // =========================================================================
  // The hand-written types
  // =========================================================================

  // Option lists come from the live definitions when we have them, so the tool
  // shows exactly the events the site shows, and fall back to the values the
  // fixture proves are real.
  var FALLBACK_EVENTTYPES = ['Ongoing', 'OnGameModeStarted', 'OnPlayerJoinGame',
    'OnPlayerDeployed', 'OnPlayerUndeploy', 'OnPlayerDied', 'OnPlayerDamaged',
    'OnPlayerEarnedKill', 'OnPlayerEarnedKillAssist', 'OnRevived',
    'OnPlayerEnterAreaTrigger', 'OnPlayerExitAreaTrigger',
    'OnPlayerEnterCapturePoint', 'OnPlayerExitCapturePoint',
    'OnCapturePointCaptured', 'OnCapturePointLost', 'OnPlayerEnterVehicle',
    'OnPlayerExitVehicle', 'OnPlayerInteract', 'OnSpawnerSpawned',
    'OnAIMoveToSucceeded', 'OnAIMoveToFailed'];
  var FALLBACK_OBJECTTYPES = ['Global', 'Player', 'Team', 'HQ', 'Sector',
    'CapturePoint', 'VehicleSpawner', 'EmplacementSpawner', 'AreaTrigger'];

  function optionsFor(type, fieldName, fallback) {
    var d = state.rawDefs && state.rawDefs[type];
    if (d) {
      for (var k = 0; k < 10; k++) {
        var args = d['args' + k];
        if (!args) continue;
        for (var i = 0; i < args.length; i++) {
          var a = args[i];
          if (a && a.name === fieldName && a.options && a.options.length) {
            return a.options.map(function (o) {
              return Array.isArray(o) ? [String(o[0]), String(o[1])] : [String(o), String(o)];
            });
          }
        }
      }
    }
    return (fallback || []).map(function (o) { return [o, o]; });
  }

  // ---- the site's own block palette ---------------------------------------
  //
  // A creator reads a block's colour before its text: gold is an action, green
  // is a value, blue is a condition, purple is a rule. A palette of our own
  // invention makes every block say the wrong thing, so there is no palette of
  // our own. These eleven are the site's, read off the live page, and they are
  // only the floor: when a capture is loaded its theme wins, and a block is
  // painted through setStyle exactly as the site paints it, so the secondary
  // and tertiary shades come along too.
  var SITE_STYLE_COLOUR = {
    'mod-block-style': '#222b2d',
    'rule-block-style': '#682177',
    'condition-block-style': '#0a4f78',
    'value-block-style': '#0f5736',
    'action-block-style': '#847000',
    'comment-block-style': '#141414',
    'variable-block-style': '#0f5736',
    'subroutine-block-style': '#622c07',
    'control-block-style': '#351f95',
    'control-block-alt-style': '#2b1b71',
    'unsupported-value-block-style': '#262626'
  };
  API.SITE_STYLE_COLOUR = SITE_STYLE_COLOUR;

  function styleColour(styleName) {
    var st = state.style;
    var bs = st && st.theme && st.theme.blockStyles && st.theme.blockStyles[styleName];
    if (bs && bs.colourPrimary) return bs.colourPrimary;
    var bc = st && st.blockStyleColours && st.blockStyleColours[styleName];
    if (typeof bc === 'string' && bc) return bc;
    return SITE_STYLE_COLOUR[styleName] || SITE_STYLE_COLOUR['action-block-style'];
  }
  API.styleColour = styleColour;

  // Paint one block the way the site does. setStyle when the workspace theme
  // carries the style, because that is the site's own call and it brings the
  // whole shade set; the flat colour only when there is no theme to ask.
  function paintStyle(block, styleName) {
    try {
      var th = block.workspace && block.workspace.getTheme && block.workspace.getTheme();
      if (th && th.blockStyles && th.blockStyles[styleName] && block.setStyle) {
        block.setStyle(styleName);
        return styleName;
      }
    } catch (e) {}
    try { block.setColour(styleColour(styleName)); } catch (e) {}
    return null;
  }
  API.paintStyle = paintStyle;

  // Which style each block the tool defines itself wears. Taken from the
  // site's own answer for those types, not chosen here.
  var MUTATOR_STYLE = {
    modBlock: 'mod-block-style',
    ruleBlock: 'rule-block-style',
    conditionBlock: 'condition-block-style',
    subroutineBlock: 'subroutine-block-style',
    subroutineInstanceBlock: 'action-block-style',
    subroutineArgumentBlock: 'value-block-style',
    variableReferenceBlock: 'variable-block-style',
    actionComment: 'comment-block-style',
    If: 'control-block-style',
    While: 'control-block-style',
    Break: 'control-block-style',
    Continue: 'control-block-style'
  };
  API.MUTATOR_STYLE = MUTATOR_STYLE;

  function defineMutators() {
    defineFields();

    // ---- modBlock: fields none, input RULES, never deletable --------------
    B.Blocks['modBlock'] = {
      init: function () {
        this.appendDummyInput().appendField(new B.FieldLabel('MOD'), 'NAME');
        this.appendStatementInput('RULES').appendField('RULES');
        paintStyle(this, 'mod-block-style');
        this.setDeletable(false);
        this.setTooltip('The mod root. Every rule hangs off this block.');
      }
    };

    // ---- ruleBlock -------------------------------------------------------
    // Evidence (fixture): fields NAME, EVENTTYPE, and OBJECTTYPE only while
    // extraState.isOngoingEvent is true; inputs CONDITIONS then ACTIONS;
    // chained one under the next inside modBlock.RULES.
    B.Blocks['ruleBlock'] = {
      init: function () {
        var self = this;
        this.appendDummyInput()
          .appendField('RULE')
          .appendField(new B.FieldTextInput('Rule'), 'NAME');
        this.appendDummyInput('EVENT')
          .appendField('EVENT')
          .appendField(looseDropdown(optionsFor('ruleBlock', 'EVENTTYPE', FALLBACK_EVENTTYPES),
            'Ongoing'), 'EVENTTYPE');
        this.appendStatementInput('CONDITIONS').appendField('CONDITIONS');
        this.appendStatementInput('ACTIONS').appendField('ACTIONS');
        this.setPreviousStatement(true);
        this.setNextStatement(true);
        paintStyle(this, 'rule-block-style');
        this.isOngoing_ = true;
        this.updateShape_();
        var f = this.getField('EVENTTYPE');
        if (f && f.setValidator) {
          f.setValidator(function (v) {
            self.isOngoing_ = (v === 'Ongoing');
            self.updateShape_();
            return v;
          });
        }
      },
      updateShape_: function () {
        var has = !!this.getField('OBJECTTYPE');
        var ev = this.getInput('EVENT');
        if (this.isOngoing_ && !has && ev) {
          ev.appendField('OBJECT')
            .appendField(looseDropdown(optionsFor('ruleBlock', 'OBJECTTYPE', FALLBACK_OBJECTTYPES),
              'Global'), 'OBJECTTYPE');
        } else if (!this.isOngoing_ && has && ev) {
          ev.removeField('OBJECTTYPE', true);
          ev.removeField('OBJECT', true);
        }
      },
      saveExtraState: function () { return { isOngoingEvent: !!this.isOngoing_ }; },
      loadExtraState: function (s) {
        this.isOngoing_ = !!(s && s.isOngoingEvent);
        this.updateShape_();
      }
    };

    // ---- subroutineBlock -------------------------------------------------
    // Free floating stack. extraState {subroutineName, parameters:[{types,name}]}
    // and the serialized field SUBROUTINE_NAME carrying the same name.
    B.Blocks['subroutineBlock'] = {
      init: function () {
        this.appendDummyInput()
          .appendField('SUBROUTINE')
          .appendField(new B.FieldTextInput('Subroutine'), 'SUBROUTINE_NAME');
        this.appendDummyInput('PARAMS');
        this.appendStatementInput('CONDITIONS').appendField('CONDITIONS');
        this.appendStatementInput('ACTIONS').appendField('ACTIONS');
        paintStyle(this, 'subroutine-block-style');
        this.params_ = [];
      },
      saveExtraState: function () {
        return {
          subroutineName: this.getFieldValue('SUBROUTINE_NAME') || '',
          parameters: (this.params_ || []).map(function (p) {
            return { types: p.types, name: p.name };
          })
        };
      },
      loadExtraState: function (s) {
        this.params_ = (s && s.parameters ? s.parameters : []).map(function (p) {
          return { types: p.types, name: p.name };
        });
        this.subName_ = (s && s.subroutineName) || '';
        renderParamLabels(this, 'PARAMS', this.params_);
      }
    };

    // ---- subroutineInstanceBlock ----------------------------------------
    // One PARAM-<i> value input per parameter, in order.
    B.Blocks['subroutineInstanceBlock'] = {
      init: function () {
        this.appendDummyInput()
          .appendField('CALL')
          .appendField(new B.FieldLabelSerializable('Subroutine'), 'SUBROUTINE_NAME');
        this.setPreviousStatement(true);
        this.setNextStatement(true);
        paintStyle(this, 'action-block-style');
        this.params_ = [];
      },
      saveExtraState: function () {
        return {
          subroutineName: this.getFieldValue('SUBROUTINE_NAME') || '',
          parameters: (this.params_ || []).map(function (p) {
            return { types: p.types, name: p.name };
          })
        };
      },
      loadExtraState: function (s) {
        this.params_ = (s && s.parameters ? s.parameters : []).map(function (p) {
          return { types: p.types, name: p.name };
        });
        this.updateShape_();
      },
      updateShape_: function () {
        var i = 0;
        while (this.getInput('PARAM-' + i)) { this.removeInput('PARAM-' + i); i++; }
        for (var k = 0; k < this.params_.length; k++) {
          this.appendValueInput('PARAM-' + k)
            .appendField(String(this.params_[k].name || ('arg' + k)));
        }
      }
    };

    // ---- subroutineArgumentBlock ----------------------------------------
    // A value block that reads one argument of the subroutine it sits in.
    // The fixture serializes only the field ARGUMENT_INDEX ("0", "1", ...).
    B.Blocks['subroutineArgumentBlock'] = {
      init: function () {
        this.appendDummyInput()
          .appendField('ARG')
          .appendField(looseDropdown([['0', '0'], ['1', '1'], ['2', '2'], ['3', '3']], '0'),
            'ARGUMENT_INDEX');
        this.setOutput(true);
        paintStyle(this, 'value-block-style');
      }
    };

    // ---- variableReferenceBlock -----------------------------------------
    // extraState {isObjectVar}. isObjectVar true adds the OBJECT value input
    // that carries the owning object, and the fixture confirms the two always
    // travel together (219 of 219).
    B.Blocks['variableReferenceBlock'] = {
      init: function () {
        var self = this;
        var head = this.appendDummyInput('HEAD')
          .appendField(looseDropdown(optionsFor('variableReferenceBlock', 'OBJECTTYPE',
            FALLBACK_OBJECTTYPES), 'Global'), 'OBJECTTYPE');
        // The site writes the WORD "Variable" between the scope and the name,
        // not a symbol: "Global | Variable | GameModeStarted". A glyph there
        // was my invention and it did not match, so the word is used.
        head.appendField(new B.FieldLabel('Variable'), 'BF6VARLABEL');
        head.appendField(new API.FieldVariable(null, null, null, null), 'VAR');
        this.setOutput(true);
        // One line, as the site draws it.
        this.setInputsInline(true);
        paintStyle(this, 'variable-block-style');
        this.isObjectVar_ = false;
        var f = this.getField('OBJECTTYPE');
        if (f && f.setValidator) {
          f.setValidator(function (v) {
            self.isObjectVar_ = (v !== 'Global');
            self.updateShape_();
            return v;
          });
        }
      },
      updateShape_: function () {
        var has = !!this.getInput('OBJECT');
        // THE SITE'S WORD, COUNTED, NOT GUESSED. Every one of the 219
        // object-scoped references on the site writes 'For' here and none
        // writes anything else. Ours said 'of', which is 8.55px narrower and
        // was the whole of the difference on all 218 of ours.
        if (this.isObjectVar_ && !has) this.appendValueInput('OBJECT').appendField('For');
        else if (!this.isObjectVar_ && has) this.removeInput('OBJECT', true);
      },
      saveExtraState: function () { return { isObjectVar: !!this.isObjectVar_ }; },
      loadExtraState: function (s) {
        this.isObjectVar_ = !!(s && s.isObjectVar);
        this.updateShape_();
      }
    };

    // ---- If --------------------------------------------------------------
    // Base inputs VALUE-0 and DO. Each else-if adds IF<n>/DO<n> from 1 up, and
    // else adds ELSE. extraState is {} when there is neither, which is what the
    // site writes, so it is reproduced exactly rather than omitted.
    B.Blocks['If'] = {
      init: function () {
        this.appendValueInput('VALUE-0').appendField('If');
        this.appendStatementInput('DO');
        this.setPreviousStatement(true);
        this.setNextStatement(true);
        paintStyle(this, 'control-block-style');
        this.elseifCount_ = 0;
        this.elseCount_ = 0;
      },
      saveExtraState: function () {
        if (!this.elseifCount_ && !this.elseCount_) return {};
        return { elseif: this.elseifCount_, else: this.elseCount_ };
      },
      loadExtraState: function (s) {
        this.elseifCount_ = (s && s.elseif) || 0;
        this.elseCount_ = (s && s['else']) || 0;
        this.updateShape_();
      },
      updateShape_: function () {
        var i = 1;
        while (this.getInput('IF' + i)) {
          this.removeInput('IF' + i);
          if (this.getInput('DO' + i)) this.removeInput('DO' + i);
          i++;
        }
        if (this.getInput('ELSE')) this.removeInput('ELSE');
        for (var k = 1; k <= this.elseifCount_; k++) {
          this.appendValueInput('IF' + k).appendField('Else If');
          this.appendStatementInput('DO' + k);
        }
        if (this.elseCount_) this.appendStatementInput('ELSE').appendField('Else');
      },
      bf6AddElseIf: function () { this.elseifCount_++; this.updateShape_(); },
      bf6ToggleElse: function () { this.elseCount_ = this.elseCount_ ? 0 : 1; this.updateShape_(); }
    };

    // AND THE SITE'S SHAPE GOES STRAIGHT BACK ON TOP.
    //
    // defineMutators is the OFFLINE fallback for these seven, and it is
    // called from more places than the one that installs the site versions -
    // installFallback calls it, and growDefinitions calls installFallback
    // every time a file is opened. So opening a project quietly replaced all
    // seven site-built blocks with the hand-written ones again, and the only
    // visible trace was an argument block offering 0,1,2,3 instead of the
    // parameter names of the subroutine it sits in.
    //
    // Reapplying here means it does not matter who redefines them or when:
    // the hand-written shape never outlives the call that made it. It is a
    // no-op when the site was never read, which is the offline case this
    // function exists for.
    var readings = state.rawReadings;
    if (readings) {
      MUTATOR_TYPES.forEach(function (t) {
        var r = readings[t];
        if (r && isProbeRecord(r)) {
          try { installMutatorFromSite(t, synthToJson(t, r)); } catch (e) {}
        }
      });
    }

    API._mutatorsDone = true;
  }
  API.defineMutators = defineMutators;

  // ONE FIELD, THE BARE NAMES, JOINED - WHICH IS WHAT THE SITE WRITES.
  //
  // Read off the live page and counted: the site carries a single field named
  // PARAMETER_LABELS holding 'Spawner, Team', 'Player, Captured', 'Team1UI,
  // Team2UI'. Ours split them into one field per parameter, named BF6P0 and
  // BF6P1, and appended the type to each - 'Spawner : Number'. So a two
  // parameter subroutine was drawn as two labels with a full spacer between
  // them, each carrying words the site does not print at all.
  //
  // The type belongs to the socket, which already draws its own symbol, so
  // printing it here said the same thing twice and made every subroutine
  // header wider than the site's.
  var PARAM_FIELD = 'PARAMETER_LABELS';
  function renderParamLabels(block, inputName, params) {
    var inp = block.getInput(inputName);
    if (!inp) return;
    // Both names are cleared: a workspace built before this change carries
    // BF6P0/BF6P1 fields, and leaving them would draw the old words next to
    // the new ones.
    for (var i = inp.fieldRow.length - 1; i >= 0; i--) {
      var nm = inp.fieldRow[i].name;
      if (nm && (nm.indexOf('BF6P') === 0 || nm === PARAM_FIELD)) inp.removeField(nm, true);
    }
    // THE FIELD IS ALWAYS THERE, EVEN WITH NOTHING IN IT.
    //
    // A subroutine with no parameters still carries the field on the site,
    // holding an empty string: it measures 3.13 wide and takes a full 24
    // spacer beside it, so leaving it out made every parameterless
    // subroutine header 27.13 narrow than the site's.
    /* A ONE LETTER NAME MEANS NOTHING ON ITS OWN, SO SAY WHAT IT IS.
     *
     * Parameter names come from the author's own TypeScript, and plenty of them
     * are w, a, b, v, x, y, z: 228 of the 1,931 parameters in Undead Ground
     * Zero are one or two letters. On the page that is a header reading
     * "SUBROUTINE spawnAt w, a, b" and a reader has no way to tell what any of
     * them takes.
     *
     * The converter already records the type beside the name, and it was being
     * thrown away here. Showing "w: Vector" costs nothing and turns the header
     * into something that can be read without opening the body. Names that are
     * already descriptive are left alone, since "player: Player" is noise.
     */
    var words = [];
    for (var k = 0; k < params.length; k++) {
      var p = params[k] || {};
      var nm = String(p.name == null ? '' : p.name);
      /* `types` is a STRING here, not a list. Treating it as a list took the
       * first CHARACTER, so every parameter came out reading "w: A" instead of
       * "w: Any", which is worse than showing nothing. Both shapes are handled
       * because the site's own readings use an array in places. */
      var ty = p.types;
      if (ty && typeof ty !== 'string' && ty.length) { ty = ty[0]; }
      ty = (typeof ty === 'string') ? ty : '';
      /* "Any" is what the converter writes when it could not tell, so it says
       * nothing a reader does not already know. */
      if (ty === 'Any') { ty = ''; }
      /* Only where the name does not already carry the meaning: "player: Player"
       * is noise, "w: Vector" is the whole point. */
      var vague = nm.length <= 2 ||
                  (ty && nm.toLowerCase().indexOf(ty.toLowerCase()) < 0 && nm.length <= 4);
      words.push(vague && ty ? (nm + ': ' + ty) : nm);
    }
    inp.appendField(new B.FieldLabel(words.join(', ')), PARAM_FIELD);
  }

  // =========================================================================
  // Type glyphs in the sockets
  //
  // The site draws a small symbol inside every empty socket saying what goes
  // there, and the same symbol on a variable. It is the fastest thing on the
  // page to read: a creator holding a green block looks for the green hole.
  // Without it a socket is an anonymous notch and the block's own text says
  // VALUE-0, which is the site's internal socket name and tells nobody
  // anything.
  //
  // All 43 symbols are in the capture already, under type-<lowercased type>.
  // Nothing here is drawn by us; the site's own svg is put where the site puts
  // it.
  // =========================================================================

  // Portal build 15249075, getIconPathForBlocklyType. Several type names do
  // not match their SVG filename; enum values share the item-index symbol.
  var SITE_ICON_TYPES = ('Array Boolean Global Message Number Player SquadId String TeamId FactionId ' +
    'Vector DeathType WeaponUnlock Vehicle Objective CapturePoint AreaTrigger Camera DamageType ' +
    'EmplacementSpawner HQ InteractPoint LootSpawner MCOM RingOfFire ScreenEffect Sector SFX ' +
    'SpatialObject Spawner SpawnPoint Transform UIButtonEvent UIWidget Variables VehicleSpawner ' +
    'VFX VO WaypointPath WorldIcon').toLowerCase().split(' ');
  var SITE_ICON_ALIASES = { squad: 'squadid', team: 'teamid', prefabspawner: 'lootspawner', variable: 'variables' };
  function typeIconName(typeName) {
    var t = String(typeName || '').trim();
    if (!t) return '';
    if (/^any(?: type)?$/i.test(t)) return 'type-any';
    if (t.indexOf('Enum_') === 0) return 'type-itemindex';
    t = t.toLowerCase();
    t = SITE_ICON_ALIASES[t] || t;
    return SITE_ICON_TYPES.indexOf(t) >= 0 ? 'type-' + t : 'type-null';
  }
  API.typeIconName = typeIconName;

  // The url out of a merged icon record, whichever tier supplied it.
  function iconUrlOf(name) {
    var icons = (state.style && state.style.categoryIcons) || {};
    var rec = icons['class:' + name] || icons[name];
    if (!rec) return '';
    var src = rec.dataUrl || rec.url || rec.image || '';
    if (!src && rec.backgroundImage) {
      var m = /url\(\s*(['"]?)([\s\S]*?)\1\s*\)/i.exec(rec.backgroundImage);
      if (m) src = m[2];
    }
    return src || '';
  }
  API.iconUrlOf = iconUrlOf;

  function typeIconUrl(typeName) {
    var u = iconUrlOf(typeIconName(typeName));
    return u || iconUrlOf('type-any');
  }
  API.typeIconUrl = typeIconUrl;

  var SOCKET_ICON_PX = 16;
  var QUOTE_PX = 12;

  // =========================================================================
  // THE SYMBOL INSIDE AN EMPTY SOCKET
  //
  // This is the difference the screenshots kept showing, and no amount of
  // reading the definitions was ever going to close it. The readings taken off
  // the site's own blocks show every empty value input with fields: [] - the
  // site puts NOTHING in the socket. The symbol a creator sees inside the
  // bubble is drawn by the site's renderer, one svg group per empty socket,
  // and it is not in any definition anywhere.
  //
  // We were appending a FieldImage to the input's field row instead, which
  // puts the symbol BESIDE the socket. Right picture, wrong place.
  //
  // The site's own svg, dumped off the live page and measured across 13
  // sockets on four different blocks, is exactly this, every time:
  //
  //   <path class="blocklyOutlinePath" fill="#0a0a0a" stroke="<block border>"
  //         d="rounded rect, height 32, corner radius 16">
  //   <g class="blocklyValueIcon" transform="translate(holeX - 16, holeY + 16)">
  //     <image width="24.615" height="24.615"
  //            transform="translate(24, -12.307)" href="...type-player.svg">
  //
  // The outline is Blockly's own, at the site's constants. The group is the
  // site's, and the numbers behind it are constant ratios of the hole:
  //
  //   the group sits at the hole's left edge less its radius, on its centre line
  //   the image is offset by one and a half radii, and half its own height up
  //   the image is 10/13 of the hole's height, square
  //
  // Written as ratios rather than as 16 / 24 / 24.615 so it still lines up if
  // the site ever changes its corner radius.
  var VALUE_ICON = { SIZE: 10 / 13, INSET: 1.5, CLASS: 'blocklyValueIcon' };

  // The rule on its own, so it can be checked against the site's numbers
  // without a browser. x and y are the socket connection point: the hole's
  // left edge, on its centre line.
  // THE GROUP SITS ON THE CONNECTION POINT ITSELF.
  //
  // This subtracted a radius, which put the symbol half outside the pill on
  // its left. The site's own numbers settle it: on the one block in the dump
  // with a single socket and nothing else to confuse the reading, the
  // connection offset is (422.6875, 25) and the group transform is
  // translate(422.6875, 25). The same, to the last digit. The hole then starts
  // one padding to the right of both, and the image's 1.5r inset is what lands
  // it inside the pill.
  function valueIconPlacement(radius, x, y) {
    var r = radius > 0 ? radius : 16;
    var size = 2 * r * VALUE_ICON.SIZE;
    return { gx: x, gy: y, ix: r * VALUE_ICON.INSET, iy: -size / 2, size: size };
  }
  API.valueIconPlacement = valueIconPlacement;
  var SVG_NS = 'http://www.w3.org/2000/svg';
  var XLINK_NS = 'http://www.w3.org/1999/xlink';

  // Preserve every accepted type in source order. A union is not Any Type.
  // The connection is asked first: it carries the site's real check list,
  // which is the truth. The mined signature is the offline fallback.
  function socketTypeNames(block, input) {
    var checks = null;
    try { checks = input.connection && input.connection.getCheck(); } catch (e) {}
    if (!checks || !checks.length) checks = slotWants(block && block.type, input && input.name);
    if (!checks || !checks.length) return ['Any Type'];
    return typeof checks === 'string' ? [checks] : checks.slice();
  }
  API.socketTypeNames = socketTypeNames;
  function socketTypeName(block, input) {
    var names = socketTypeNames(block, input);
    return names.length === 1 ? names[0] : 'Any Type';
  }
  API.socketTypeName = socketTypeName;

  // Every empty value input on this block, with the symbol drawn inside its
  // hole. Called after the renderer has drawn, because the hole's position is
  // only known once the block has been measured.
  function paintValueIcons(block) {
    if (!block || !block.getSvgRoot) return 0;
    var root = null;
    try { root = block.getSvgRoot(); } catch (e) { return 0; }
    if (!root) return 0;

    // Whatever was drawn last time goes: a socket that has since been filled
    // must not keep its symbol, and one that moved must not leave a ghost.
    var old = root.querySelectorAll(':scope > g.' + VALUE_ICON.CLASS);
    for (var i = 0; i < old.length; i++) {
      if (old[i].parentNode) old[i].parentNode.removeChild(old[i]);
    }

    // THE HOLE'S RADIUS IS HALF THE EMPTY SOCKET'S HEIGHT, NOT THE BLOCK'S
    // CORNER RADIUS.
    //
    // This read CORNER_RADIUS, which is the corner of the BLOCK. On the site
    // that constant is 1, and on ours it is 2, so the symbol came out three
    // pixels across: drawn, present in the svg, and invisible. Every probe said
    // the group and the image were there, and the screenshots showed nothing,
    // and both were telling the truth.
    //
    // The site's own sockets measure 32 tall with a 16 radius, and its
    // EMPTY_INLINE_INPUT_HEIGHT is 32. That is the number.
    var r = 16;
    try {
      var cp = block.workspace.getRenderer().getConstants();
      if (cp && cp.EMPTY_INLINE_INPUT_HEIGHT > 0) r = cp.EMPTY_INLINE_INPUT_HEIGHT / 2;
    } catch (e) {}

    var drawn = 0;
    (block.inputList || []).forEach(function (input) {
      if (!input || !input.connection) return;
      if (input.connection.type !== B.INPUT_VALUE) return;
      if (input.connection.targetConnection) return;      // filled: no symbol
      var pos = null;
      try { pos = input.connection.getOffsetInBlock(); } catch (e) {}
      if (!pos) return;

      var names = socketTypeNames(block, input);

      // The connection point sits on the hole's LEFT edge, on its centre line,
      // which is the same pair of numbers the site's transform is built from.
      var p = valueIconPlacement(r, pos.x, pos.y);
      var g = document.createElementNS(SVG_NS, 'g');
      g.setAttribute('class', VALUE_ICON.CLASS);
      g.setAttribute('transform', 'translate(' + p.gx + ',' + p.gy + ')');
      names.forEach(function (name, index) {
        var url = typeIconUrl(name);
        if (!url) return;
        var img = document.createElementNS(SVG_NS, 'image');
        img.setAttribute('width', p.size + 'px');
        img.setAttribute('height', p.size + 'px');
        // The captured widths grow by one icon plus a 2px gap at radius 16.
        img.setAttribute('transform', 'translate(' + (p.ix + index * (p.size + r / 8)) + ',' + p.iy + ')');
        img.setAttributeNS(XLINK_NS, 'xlink:href', url);
        img.setAttribute('href', url);
        img.setAttribute('data-bf6-type', name);
        var title = document.createElementNS(SVG_NS, 'title');
        title.textContent = name;
        img.appendChild(title);
        img.style.cursor = 'pointer';
        g.appendChild(img);
      });
      // Every symbol opens the same socket help with its complete type list.
      // Blockly handles pointerdown first and can redraw/remove this group
      // before mousedown arrives. Claim that initial event so help is usable.
      g.addEventListener(typeof PointerEvent === 'undefined' ? 'mousedown' : 'pointerdown', function (e) {
        if (typeof API.onSocketClick !== 'function') return;
        e.stopPropagation();
        if (e.preventDefault) e.preventDefault();
        try { API.onSocketClick(block, input); } catch (e2) {}
      });
      if (!g.childNodes.length) return;
      root.appendChild(g);
      drawn++;
    });
    return drawn;
  }
  API.paintValueIcons = paintValueIcons;

  // THE SITE'S OWN CLASS, PUT BACK ON THE FIELD IT BELONGS TO.
  //
  // The class only exists once the field has been drawn, so this runs with
  // the socket symbols rather than at init: at init there is no text element
  // to put it on. Marked per field so a block that renders a hundred times
  // pays for it once.
  function dressFields(block) {
    var byType = state.style && state.style.fieldClasses;
    if (!byType) return 0;
    var want = byType[block.type];
    if (!want) return 0;
    var n = 0;
    (block.inputList || []).forEach(function (inp) {
      (inp.fieldRow || []).forEach(function (f) {
        if (f.bf6Dressed) return;
        var words = '';
        try { words = String(f.getText ? f.getText() : ''); } catch (e) {}
        var cls = want[f.name] || want['#' + words];
        if (!cls) return;
        var g = null, t = null;
        try { g = f.getSvgRoot ? f.getSvgRoot() : null; } catch (e) {}
        try { t = g && g.querySelector('text'); } catch (e) {}
        if (!t) return;
        cls.split(/\s+/).forEach(function (c) { if (c) t.classList.add(c); });
        f.bf6Dressed = true;
        n++;
      });
    });
    return n;
  }
  API.dressFields = dressFields;

  // Installed once, on the renderer's drawer, because that is the only place
  // that runs after a block has been measured AND after its path has been
  // written. A change listener cannot do it: it fires before the render.
  // =========================================================================
  // THE BLOCK'S LAYOUT, MEASURED THE WAY THE SITE MEASURES IT
  //
  // Two numbers decide most of the difference between a Portal block and ours,
  // and both live in METHODS on the render info, which is why neither ever
  // crossed with the capture:
  //
  //   1. the spacing between things in a row. Measured element by element on
  //      both sides, every interior spacer in an input row is 24 on the site
  //      and 12 in stock Blockly - twelve pixels lost on BOTH sides of every
  //      socket on every block. On AIBattlefieldBehavior that is the entire
  //      36 pixel difference, to the pixel.
  //   2. how wide an empty socket is. The site's measures 72.62 where the same
  //      socket with the same constants measures 48 without, and the 24.615
  //      between them is the width of the symbol drawn inside it. The site
  //      keeps room for it; we drew the symbol and kept no room, which is a
  //      socket too small for its own contents.
  //
  // Both are applied from the capture. Nothing here is a number somebody
  // decided looked right.
  var GLayoutHooked = false;

  // An icon measurable whose icon asked for no room. Blockly builds the
  // measurable's width straight from icon.getSize(), so an icon that answers
  // zero is already 0 wide here; this only says the spacers should go too.
  function isAnnotationIcon(el) {
    if (!el) return false;
    try {
      if (!B || !B.blockRendering) return false;
      if ((el.type & B.blockRendering.Types.ICON) !== B.blockRendering.Types.ICON) return false;
      return !!(el.icon && el.icon.bf6Annotation);
    } catch (e2) { return false; }
  }

  function spacerKindOf(B, e) {
    if (!e) return 'EDGE';
    var T = B.blockRendering.Types;
    try {
      if (T.isSpacer(e)) return 'SPACER';
      // The three kinds of input are spaced differently and must be named
      // apart, in the same words the tape uses. Statement first, then
      // external, then inline: an inline input is also an input, so a general
      // test placed first would swallow the other two.
      if (T.isStatementInput && T.isStatementInput(e)) return 'STATEMENT';
      if (T.isExternalInput && T.isExternalInput(e)) return 'EXTERNAL';
      if (T.isInlineInput && T.isInlineInput(e)) return 'INLINE';
      if (T.isInput(e)) return 'INPUT';
      // ICON before FIELD: an icon is its own measurable and the tape measure
      // names it separately, so naming it FIELD here would look up a key that
      // the captured table never contains.
      if ((e.type & T.ICON) === T.ICON) return 'ICON';
      if (T.isField(e)) return 'FIELD';
      // ANYTHING ELSE GETS THE NAME BLOCKLY GIVES IT, WHICH IS WHAT THE TAPE
      // WROTE DOWN.
      //
      // This returned 'OTHER' for everything it had no test for, while the
      // tape names the measurable from Blockly's own Types enum. So the site
      // taught us 'S|FIELD>JAGGED_EDGE' and the renderer asked for
      // 'S|FIELD>OTHER', every time, and the rule sat in the table doing
      // nothing. The two halves have to name things the same way BY
      // CONSTRUCTION, not by two lists kept in step by hand.
      return typeNameOf(T, e.type);
    } catch (e2) {}
    return 'OTHER';
  }

  // The same naming the tape's nameType does: the exact name if the number is
  // one, otherwise the lowest bit that is set - which is what taking the first
  // part of its 'A|B|C' name comes to.
  var GTypeNames = null;
  function typeNameOf(T, n) {
    if (!GTypeNames) {
      GTypeNames = {};
      try {
        Object.keys(T).forEach(function (k) {
          if (typeof T[k] === 'number' && k !== 'nextTypeValue_') GTypeNames[T[k]] = k;
        });
      } catch (e2) {}
    }
    if (GTypeNames[n]) return GTypeNames[n];
    var bits = Object.keys(GTypeNames).map(Number).sort(function (a, b) { return a - b; });
    for (var i = 0; i < bits.length; i++) {
      if (bits[i] && (n & bits[i]) === bits[i]) return GTypeNames[bits[i]];
    }
    return 'OTHER';
  }

  function installLayoutRules() {
    if (GLayoutHooked || !B) return false;
    var rules = (state.style && state.style.socketRules) || { iconRoom: 32 * VALUE_ICON.SIZE };
    var RI = null;
    try { RI = B.zelos && B.zelos.RenderInfo; } catch (e) {}
    if (!RI || !RI.prototype) return false;

    // THE TABLE IS READ WHEN THE QUESTION IS ASKED, NOT WHEN THE HOOK GOES IN.
    //
    // The method can only be replaced once, so holding the table in the
    // closure meant the FIRST capture won for the life of the page: capturing
    // the site again picked up new numbers, wrote them to state, and the
    // renderer went on answering out of the old ones. That is the same shape
    // of bug as the site script being read once at startup, and it hides the
    // same way - everything reports success and the pixels do not move.
    if (rules.spacing) {
      var origSpacing = RI.prototype.getInRowSpacing_;
      RI.prototype.getInRowSpacing_ = function (prev, next) {
        var base = origSpacing.call(this, prev, next);
        try {
          var spacing = (state.style && state.style.socketRules &&
                         state.style.socketRules.spacing) || null;
          if (!spacing) return base;
          // The block's own kind is part of the key: measured on the site, the
          // leading spacer before a first field is 0 on a value block and 12 on
          // a statement block. One table for both averages them and is wrong
          // for both, which is a value block twelve pixels too narrow and a
          // dropdown block twenty four.
          // OUR OWN BADGE MUST NOT MOVE THE BLOCK.
          //
          // The stale ObjId warning is a thing WE hang on a block; the site
          // has no reason to draw it and does not. Left in the row it cost
          // 50px on all 21 blocks carrying one - 17 for the icon and a full
          // spacer either side - so the block was a different size from the
          // site's for a reason that has nothing to do with the site.
          //
          // An icon that reports no size is an annotation, not content, so it
          // gets no spacer. It is put back on screen by the drawer, outside
          // the block, where it cannot push anything.
          if (isAnnotationIcon(prev) || isAnnotationIcon(next)) return 0;
          var kind = (this.block_ && this.block_.outputConnection) ? 'V' : 'S';
          var key = kind + '|' + spacerKindOf(B, prev) + '>' + spacerKindOf(B, next);
          var rec = spacing[key];
          // Only where the site AGREED with itself. A context measured once,
          // or measured differently every time, is alignment padding leaking
          // into the reading and must not become a rule.
          if (rec && rec.agreed >= 2 && typeof rec.px === 'number') return rec.px;
        } catch (e) {}
        return base;
      };
    }

    // AND TAKE IT OUT OF THE ROW ALTOGETHER, NOT JUST MAKE IT NARROW.
    //
    // Zeroing the icon's size and skipping its spacers recovered 41 of the
    // 50px it cost, and left 9. The last 9 is zelos's own negative spacer at
    // the row's edge, which it works out AFTER the fact from what is in the
    // row - a row starting with a zero width icon gets -4 where the site's
    // gets -13. A measurable that is present but empty is still present.
    //
    // Stripping it at the end of createRows_ is before any spacing is
    // computed, so every number after that is worked out on exactly the row
    // the site has. The icon still exists on the block, keeps its warning
    // bubble and its click, and the drawer puts it back on screen.
    var origCreateRows = RI.prototype.createRows_;
    if (origCreateRows) {
      RI.prototype.createRows_ = function () {
        origCreateRows.call(this);
        try {
          var rows = this.rows || [];
          for (var r = 0; r < rows.length; r++) {
            var els = rows[r].elements;
            if (!els || !els.length) continue;
            var kept = [];
            for (var i = 0; i < els.length; i++) {
              if (!isAnnotationIcon(els[i])) kept.push(els[i]);
            }
            if (kept.length !== els.length) rows[r].elements = kept;
          }
        } catch (e2) {}
      };
    }

    var room = rules.iconRoom;
    if (room > 0) {
      var origAddInput = RI.prototype.addInput_;
      RI.prototype.addInput_ = function (input, activeRow) {
        origAddInput.call(this, input, activeRow);
        try {
          var els = activeRow.elements || [];
          var last = els[els.length - 1];
          if (!last) return;
          if (!B.blockRendering.Types.isInlineInput(last)) return;
          // Left on the measurable so the tape can read back what was
          // decided here. Deciding wrongly and drawing the result is the
          // failure this whole file keeps having, and a decision nobody can
          // see is one nobody can check.
          // ASK THE MEASURABLE, NOT THE INPUT.
          //
          // This asked input.connection.targetConnection, and on the types
          // built from the site's readings that object is not there at all -
          // measured, hasConn came back false on every subroutineInstanceBlock
          // parameter. A missing connection is not an empty socket, but the
          // test could not tell the two apart, so room for a symbol was kept
          // in 147 sockets that already had a block sitting in them, and every
          // one came out 24.62 too wide.
          //
          // The measurable knows: Blockly puts the connected block on it while
          // it is being built, which is why the width already included the
          // child. That is the thing being measured, so that is the thing to
          // ask.
          var full = !!(last.connectedBlock ||
                        (input.connection && input.connection.targetConnection));
          last.bf6Filled = full;
          last.bf6HasConn = !!input.connection;
          last.bf6HasKid = !!last.connectedBlock;
          if (full) return;

          // ONE SYMBOL PER TYPE THE SOCKET ACCEPTS.
          //
          // Measured on the site: an empty socket is 72.62 wide when it takes
          // one type, 99.23 when it takes two and 125.85 when it takes three.
          // It is drawing a symbol for each of them. Adding a single symbol's
          // worth of room left every multi-type socket 26.61 or 53.23 short.
          //
          // The width is asked of the MEASURABLE's own input, not of the
          // input handed to this method: on the types built from the site's
          // readings those are not always the same object, which is the bug
          // that put room into 147 sockets that already had blocks in them.
          var currentRules = (state.style && state.style.socketRules) || rules;
          var byChecks = currentRules.emptyByChecks;
          var measuredInput = last.input || input;
          var sourceBlock = measuredInput.getSourceBlock ? measuredInput.getSourceBlock() : this.block_;
          var typeCount = socketTypeNames(sourceBlock, measuredInput).length;
          if (byChecks) {
            var rec = byChecks[typeCount];
            // Only a width the site produced more than once. A socket measured
            // a single time is as likely to be an odd one as a rule.
            if (rec && rec.agreed >= 2 && rec.px > last.width) {
              last.bf6Room = rec.px - last.width;
              last.width = rec.px;
              return;
            }
          }
          var iconRoom = currentRules.iconRoom > 0 ? currentRules.iconRoom : room;
          last.bf6Room = iconRoom * typeCount + Math.max(0, typeCount - 1) * iconRoom / (16 * VALUE_ICON.SIZE);
          // Widened BEFORE the spacing and the finalising run, so everything
          // downstream sees the true width. Adjusting it afterwards would move
          // the socket without moving anything around it.
          last.width += last.bf6Room;
        } catch (e) {}
      };
    }

    GLayoutHooked = true;
    return true;
  }
  API.installLayoutRules = installLayoutRules;
  API.layoutRulesInstalled = function () { return GLayoutHooked; };

  var GDrawerHooked = false;
  var GIconWarned = false;
  // AN ANNOTATION SITS JUST OFF THE BLOCK'S RIGHT EDGE.
  //
  // Taken out of the row it has no position of its own, so the renderer
  // leaves it at the start of the row on top of the first field. Just past
  // the right edge is the one place that is reliably empty: a stack is
  // vertical, so nothing follows a statement block across, and a value block
  // sits in a socket whose next element is a full spacer away.
  // THE CLASS THE SITE PUTS ON EVERY BLOCK'S BODY.
  //
  // A census of every class name on both pages found exactly one the site has
  // and we do not: blocklyBlockBackground, on all 5088 blocks. Nothing looked
  // wrong, because the only rules that use it also require .blocklyDisabled:
  //
  //   .blocklyDisabled .blocklyBlockBackground { stroke: var(--tmln-colors-primary); fill: rgb(54,64,64) }
  //
  // and nothing in this project is disabled. So it is a difference that shows
  // up the first time somebody disables a block and never before - which is
  // exactly the kind a comparison of the current screen cannot find, and a
  // census of what EXISTS can.
  function markBlockBackground(block) {
    try {
      var p = block.pathObject && block.pathObject.svgPath;
      if (p && !p.classList.contains('blocklyBlockBackground')) {
        p.classList.add('blocklyBlockBackground');
        return 1;
      }
    } catch (e) {}
    return 0;
  }
  API.markBlockBackground = markBlockBackground;

  var ANNOTATION_GAP = 4;
  function placeAnnotationIcons(block) {
    var icons = [];
    try { icons = block.getIcons ? block.getIcons() : []; } catch (e) { return 0; }
    var n = 0;
    for (var i = 0; i < icons.length; i++) {
      var ic = icons[i];
      if (!ic || !ic.bf6Annotation) continue;
      var g = null;
      try { g = ic.getSvgRoot ? ic.getSvgRoot() : null; } catch (e) {}
      if (!g) continue;
      var w = 0, h = 0;
      try { w = block.width || 0; h = block.height || 0; } catch (e) {}
      if (!w) continue;
      var size = ic.bf6RealSize || { width: 17, height: 17 };
      var y = Math.max(0, Math.round((Math.min(h, 48) - size.height) / 2));
      try {
        g.setAttribute('transform',
          'translate(' + (w + ANNOTATION_GAP) + ',' + y + ')');
      } catch (e) {}
      n++;
    }
    return n;
  }
  API.placeAnnotationIcons = placeAnnotationIcons;

  // Mark an icon as ours: it keeps its bubble and its click, and stops taking
  // part in the block's measurement.
  function markAnnotationIcon(ic) {
    if (!ic || ic.bf6Annotation) return false;
    try {
      var real = ic.getSize ? ic.getSize() : null;
      ic.bf6RealSize = real ? { width: real.width, height: real.height }
                            : { width: 17, height: 17 };
    } catch (e) { ic.bf6RealSize = { width: 17, height: 17 }; }
    ic.bf6Annotation = true;
    try {
      ic.getSize = function () { return new B.utils.Size(0, 0); };
    } catch (e) {
      ic.getSize = function () { return { width: 0, height: 0 }; };
    }
    return true;
  }
  API.markAnnotationIcon = markAnnotationIcon;

  function installValueIcons() {
    if (GDrawerHooked || !B) return false;
    var D = null;
    try { D = B.zelos && B.zelos.Drawer; } catch (e) {}
    if (!D || !D.prototype || !D.prototype.draw) return false;
    var orig = D.prototype.draw;
    D.prototype.draw = function () {
      orig.call(this);
      // NEVER LET THIS TAKE THE BLOCK DOWN WITH IT. A throw here lands inside
      // Blockly's own render, which is called from a click handler, and the
      // whole toolbox stops opening with no message anywhere. That exact shape
      // of failure cost most of a day on 2026-09-06.
      try {
        paintValueIcons(this.block_);
        dressFields(this.block_);
        placeAnnotationIcons(this.block_);
        markBlockBackground(this.block_);
      }
      catch (e) {
        if (!GIconWarned) {
          GIconWarned = true;
          try { console.log('BF6 value icons: ' + (e && e.message)); } catch (e2) {}
        }
      }
    };
    GDrawerHooked = true;
    return true;
  }
  API.installValueIcons = installValueIcons;
  API.valueIconsInstalled = function () { return GDrawerHooked; };

  // The site's own block art, mirrored to disk: 1x1, quote0, quote1. VARICON0
  // is the opening mark and VARICON1 the closing one, which is the order the
  // catalogue records them in.
  function blockImageFor(fieldName) {
    var imgs = (state.style && state.style.blockImages) || {};
    var want = /1$/.test(String(fieldName)) ? 'quote1.png' : 'quote0.png';
    var v = imgs[want] || imgs[want.replace('.png', '')];
    if (typeof v === 'string' && v) return v;
    if (v && typeof v === 'object' && v.dataUrl) return v.dataUrl;
    return '';
  }
  API.blockImageFor = blockImageFor;

  // One socket. The glyph first, then the type in words, because the symbol is
  // read at a glance and the word settles it. A socket the capture has no
  // symbol for still gets its word, so nothing is ever left saying VALUE-0.
  // A GLYPH IS AN SVG IMAGE PER SOCKET, AND SOCKETS ARE THE COMMONEST THING
  // ON THE CANVAS. On this project that is three thousand extra images, which
  // the site never pays for because it draws the bubble in its renderer rather
  // than as a field. Below the threshold they are worth it and the canvas is
  // easier to read; above it the editor is better off fast, so they are left
  // off and the type is still one hover away on the socket's tooltip.
  // A GLYPH IS ONLY EVER MADE FOR A SOCKET THAT IS ACTUALLY EMPTY.
  //
  // The first version made one for every value socket and then hid the ones
  // that turned out to be filled. On this project that is three thousand SVG
  // images created and immediately hidden, which is what made a big workspace
  // crawl. Turning them off above a block count fixed the speed by removing
  // the feature, which is not a fix.
  //
  // So the socket remembers what it wants, and the picture is made the moment
  // it is empty and thrown away the moment it is filled. A filled socket costs
  // nothing, which on this project is most of them, and the glyphs are always
  // on however large the workspace is.
  function glyphsAffordable() { return true; }
  API.glyphsAffordable = glyphsAffordable;

  // What this socket accepts, remembered so the picture can be made later.
  function noteSocketWants(input, wants) {
    if (!input) return;
    input.bf6Wants = (wants && wants.length) ? wants.slice() : null;
  }
  API.noteSocketWants = noteSocketWants;

  function isSocketGlyphField(field) { return /^BF6T(?:N|\d+)?$/.test(field.name || ''); }

  function removeSocketGlyph(input) {
    var row = input.fieldRow || [];
    var keep = [], doomed = [];
    row.forEach(function (f) {
      if (isSocketGlyphField(f)) doomed.push(f); else keep.push(f);
    });
    if (!doomed.length) return 0;
    input.fieldRow.length = 0;
    keep.forEach(function (f) { input.fieldRow.push(f); });
    doomed.forEach(function (f) { try { f.dispose(); } catch (e) {} });
    return doomed.length;
  }

  // Told before a workspace is built, so the very first block already knows.
  function noteWorkspaceSize(json) {
    var n = 0;
    (function walk(x) {
      if (!x || typeof x !== 'object') return;
      if (Array.isArray(x)) { x.forEach(walk); return; }
      if (x.type && x.id) n++;
      for (var k in x) if (Object.prototype.hasOwnProperty.call(x, k)) walk(x[k]);
    })(json);
    state.blockCount = n;
    return n;
  }
  API.noteWorkspaceSize = noteWorkspaceSize;

  function labelSocket(input, wantTypes, fallbackName) {
    if (!input) return false;
    // Never twice on the same socket.
    if ((input.fieldRow || []).some(function (f) {
      return isSocketGlyphField(f);
    })) return false;
    var names = wantTypes && wantTypes.length ? wantTypes : null;

    // The field fallback uses the same ordered icons as the SVG drawer.
    var symbols = names || ['Any Type'];
    var drew = false;

    symbols.forEach(function (name, index) {
      var url = typeIconUrl(name);
      if (!url || !B.FieldImage) return;
      try {
        input.appendField(
          new B.FieldImage(url, SOCKET_ICON_PX, SOCKET_ICON_PX, name, function () {
            if (typeof API.onSocketClick === 'function') {
              try { API.onSocketClick(input.getSourceBlock(), input); } catch (e) {}
            }
          }), index ? 'BF6T' + index : 'BF6T');
        drew = true;
      } catch (e) {}
    });

    if (!drew && names && names.length > 1) {
      input.appendField(new B.FieldLabel(names.join(' or ')), 'BF6TN');
    } else if (!drew && fallbackName) {
      input.appendField(new B.FieldLabel(fallbackName));
    }
    return drew;
  }
  API.labelSocket = labelSocket;

  // A block built from the site's own definition already carries whatever
  // labels the site put on it. Only a socket left completely bare is filled in
  // here, so nothing is ever doubled and nothing is ever anonymous.
  // A label that is really the socket's own internal name. The site's stored
  // definitions carry these, which is why sockets read VALUE-0 and VALUE-1
  // rather than saying what goes in them. They are not writing, they are
  // plumbing, and they are replaced by the type symbol.
  function isInternalSocketLabel(text, inputName) {
    var t = String(text || '').trim();
    if (!t) return false;
    if (t === inputName) return true;
    // A DIGIT IS WHAT MAKES IT PLUMBING. Without one this also matched the
    // word "If", which is the If block's own name and the site's real writing,
    // so the site's If lost the only word on it. Socket names always carry
    // their number: VALUE-0, PARAM 1, IF0. Words do not.
    return /^(VALUE|PARAM|IF|ARG|INPUT)[\s_-]*\d+$/i.test(t);
  }

  function decorateSockets(block, type) {
    if (!block || !block.inputList) return 0;
    var n = 0;
    for (var i = 0; i < block.inputList.length; i++) {
      var inp = block.inputList[i];
      if (!inp || !inp.connection) continue;
      if (inp.connection.type !== B.INPUT_VALUE) continue;

      var row = inp.fieldRow || [];
      // Already carries a picture: the site drew it, leave it alone.
      if (row.some(function (f) { return f instanceof B.FieldImage; })) continue;

      var wants = slotWants(type || block.type, inp.name);
      // The plumbing labels still go, whatever draws the symbol: VALUE-0 is
      // never writing. Only the GLYPH is the drawer's job.
      if (!wants || !wants.length) { if (!GDrawerHooked) continue; wants = null; }

      // The decision is PER FIELD, not per row. A block's own name is a field
      // of its first socket, so judging the whole row keeps the plumbing label
      // sitting next to it and the socket still reads VALUE-0. Real writing is
      // kept in its original order; only the plumbing goes.
      var keep = [];
      var doomed = [];
      row.forEach(function (f) {
        var txt = f.getText ? f.getText() : '';
        if (!f.EDITABLE && isInternalSocketLabel(txt, inp.name)) doomed.push(f);
        else keep.push(f);
      });

      // These labels are unnamed, so removeField cannot reach them. The row is
      // rebuilt and the discarded fields disposed, which is what Blockly's own
      // Input teardown does.
      inp.fieldRow.length = 0;
      keep.forEach(function (f) { inp.fieldRow.push(f); });
      doomed.forEach(function (f) { try { f.dispose(); } catch (e) {} });

      noteSocketWants(inp, wants);
      n++;
    }
    return n;
  }
  API.decorateSockets = decorateSockets;

  // THE GLYPH BELONGS TO THE EMPTY SOCKET, NOT TO THE BLOCK.
  //
  // On the site an empty socket is a small dark bubble with the type's symbol
  // in it, and the moment something is plugged in - a variable, a value, a
  // whole expression - the bubble is gone, because the thing that filled it is
  // drawn there instead. Ours were fields on the block, so they sat there
  // beside a filled socket saying a Player goes here next to the Player that
  // already does.
  //
  // Blockly draws an input's fields whether or not the input is connected, so
  // the visibility has to be maintained: shown while the socket is empty,
  // hidden the instant it is filled.
  function syncSocketGlyphs(block, bDefer) {
    if (!block || !block.inputList) return 0;
    // THE RENDERER DRAWS IT NOW, AND ONLY ONE OF THEM MAY.
    //
    // paintValueIcons puts the symbol INSIDE the socket, where the site puts
    // it. The field-row glyph below puts a second copy beside the socket,
    // which is where ours used to be and is the thing that did not match. When
    // the drawer hook is in, the row is left alone and any glyph already on it
    // is taken off.
    if (GDrawerHooked) {
      var off = 0;
      for (var g = 0; g < block.inputList.length; g++) off += removeSocketGlyph(block.inputList[g]);
      if (off && !bDefer) { try { block.render(); } catch (e) {} }
      return off;
    }
    var n = 0;
    for (var i = 0; i < block.inputList.length; i++) {
      var inp = block.inputList[i];
      if (!inp || !inp.connection) continue;
      if (inp.connection.type !== B.INPUT_VALUE) continue;
      var filled = !!inp.connection.targetBlock();
      var has = (inp.fieldRow || []).some(function (f) {
        return isSocketGlyphField(f);
      });
      if (filled && has) { n += removeSocketGlyph(inp) ? 1 : 0; }
      else if (!filled && !has) {
        // EVERY empty socket, not only the ones with a known type. A socket
        // the signature says nothing about still needs something to click.
        labelSocket(inp, inp.bf6Wants || null, null);
        n++;
      }
    }
    // ONE RENDER PER BLOCK IS FINE FOR ONE BLOCK AND RUINOUS FOR FIVE
    // THOUSAND. A single connection change renders immediately, because that
    // is one block and it has to look right at once. A whole workspace defers
    // and renders once at the end.
    if (n && !bDefer) { try { block.render(); } catch (e) {} }
    return n;
  }
  API.syncSocketGlyphs = syncSocketGlyphs;

  // Every block on the workspace, for a load or a wholesale change. Events off
  // and rendering off for the sweep: a 5,192 block workspace is 5,192 renders
  // otherwise, which is most of the wait when a big experience opens.
  function syncAllSocketGlyphs(ws) {
    if (!ws) return 0;
    var n = 0;
    var all = ws.getAllBlocks(false);
    var wasGroup = false;
    try { wasGroup = B.Events.getGroup(); B.Events.disable(); } catch (e) {}
    try {
      for (var i = 0; i < all.length; i++) n += syncSocketGlyphs(all[i], true);
    } finally {
      try { B.Events.enable(); } catch (e) {}
      if (wasGroup === false) { /* nothing to restore */ }
    }
    if (n) {
      try { if (ws.render) ws.render(); } catch (e) {}
    }
    return n;
  }
  API.syncAllSocketGlyphs = syncAllSocketGlyphs;


  var MUTATOR_TYPES = ['modBlock', 'ruleBlock', 'subroutineBlock',
    'subroutineInstanceBlock', 'subroutineArgumentBlock',
    'variableReferenceBlock', 'If'];
  API.MUTATOR_TYPES = MUTATOR_TYPES;

  // =========================================================================
  // Definitions
  // =========================================================================

  // THE READING TAKEN OFF THE SITE'S OWN BLOCKS, TURNED INTO BLOCKLY JSON.
  //
  // The capture builds one real block of every type on the site and writes
  // down what it is made of: for each input, its kind and name, and the fields
  // sitting in front of it, each field with its value, its option list and,
  // for a picture, its address and the size the site draws it at. That is not
  // a description of a block, it is the block, and it carries the three things
  // the mined catalogue could never know:
  //
  //   the round type symbol at the head of every value block (ICON-0),
  //   the words BETWEEN sockets - From, To and By on ForVariable,
  //   and the order all of it sits in.
  //
  // None of it is Blockly JSON, so jsonInit threw on every one of the 604
  // types and every block quietly fell back to the approximation. This is the
  // conversion, and it is why the editor can finally draw what the site draws.
  var IN_VALUE = 1, IN_STATEMENT = 3;

  // An address the site wrote, resolved to the copy we hold. The mirror is
  // keyed by file name, so the name is the only part used:
  // '/bf6/15249075/assets/blockly/icons/type-number.svg' -> 'type-number'.
  function siteAssetUrl(src) {
    var s = String(src || '');
    if (!s) return '';
    if (/^data:/.test(s)) return s;
    var file = s.split('?')[0].split('#')[0].split('/').pop();
    if (!file) return '';
    var imgs = (state.style && state.style.blockImages) || {};
    var v = imgs[file];
    if (typeof v === 'string' && v) return v;
    if (v && v.dataUrl) return v.dataUrl;
    return iconUrlOf(file.replace(/\.[a-z0-9]+$/i, ''));
  }
  API.siteAssetUrl = siteAssetUrl;

  function looksLikeImage(v) {
    return typeof v === 'string' && /\.(png|svg|gif|jpe?g)(\?|$)/i.test(v);
  }

  // One field of one input. Judged on what it HOLDS, never on its class name:
  // the site's bundle is minified, so its classes are called lo, uo, mp, ha
  // and xs this build and something else the next.
  function probeFieldToJson(f) {
    if (!f) return null;
    var src = f.src || (looksLikeImage(f.value) ? f.value : '');
    if (src) {
      var url = siteAssetUrl(src);
      // No copy of the picture means no picture. A field_image with an address
      // that resolves to nothing draws a broken-image box on every block.
      //
      // BUT A MISS HERE IS OFTEN ONLY A MISS FOR NOW. The definitions arrive
      // before the style does, and the style is what brings the mirrored art,
      // so on the first pass nearly every symbol resolves to nothing and the
      // block is built without it - permanently, because the conversion has
      // already happened. That is why blocks were missing their head badge on
      // the canvas while passing every offline check. Counted here so the
      // arrival of the style can put it right.
      if (!url) { state.iconsDropped = (state.iconsDropped || 0) + 1; return null; }
      var w = f.w > 0 ? f.w : SOCKET_ICON_PX;
      var h = f.h > 0 ? f.h : w;
      var j = { type: 'field_image', src: url, width: w, height: h, alt: '' };
      if (f.name) j.name = f.name;
      return j;
    }
    if (f.options && f.options.length) {
      return {
        type: 'field_dropdown',
        name: f.name || 'OPT',
        options: f.options.map(function (o) {
          return Array.isArray(o) ? [String(o[0]), String(o[1])] : [String(o), String(o)];
        })
      };
    }
    // A field with no name is not saved and not edited: it is writing. That is
    // what From, To and By are, and what a value block's own name is.
    if (!f.name) {
      var lab = { type: 'field_label', text: String(f.value == null ? '' : f.value) };
      // field_label takes a css class, and the site paints particular labels
      // through one. Without it the label matches no rule and comes out plain.
      if (f.cssClass) lab['class'] = String(f.cssClass);
      return lab;
    }

    // A NAME THE BLOCK REMEMBERS IS NOT A BOX TO TYPE IN.
    //
    // The site's SUBROUTINE_NAME is a serializable LABEL: saved, but not
    // editable and drawn with no border. Building every named string field as
    // a text input gave it a box the site does not draw, 13 pixels taller and
    // 16 wider. EDITABLE is public on every Blockly field and survives the
    // minifier, so it is asked rather than the class name.
    if (f.editable === false) {
      // SAVED OR NOT SAVED IS A SEPARATE QUESTION FROM EDITABLE OR NOT.
      //
      // Every non-editable field was built as field_label_SERIALIZABLE, which
      // made it appear in the saved file. The site's own readings say
      // otherwise, and the split is lopsided: of the fields it marks
      // uneditable, 932 are serializable:false and exactly FOUR are true
      // (the two SUBROUTINE_NAMEs and a deleted-block placeholder).
      //
      // Measured against the site's own export of the same experience: ours
      // carried NAME on 62 conditionBlocks and on modBlock where the site's
      // carried neither. A field the site does not save is a field that has
      // no business in a file the site is going to read back.
      var kind = (f.serializable === false) ? 'field_label' : 'field_label_serializable';
      var ser = { type: kind, name: f.name,
                  text: String(f.value == null ? '' : f.value) };
      if (f.cssClass) ser['class'] = String(f.cssClass);
      return ser;
    }
    if (typeof f.value === 'number') return { type: 'field_number', name: f.name, value: f.value };
    return { type: 'field_input', name: f.name, text: String(f.value == null ? '' : f.value) };
  }

  // Is this a reading off a real block rather than a Blockly definition?
  function isProbeRecord(rec) {
    return !!(rec && typeof rec === 'object' && !Array.isArray(rec)
      && Array.isArray(rec.inputs) && rec.message0 === undefined);
  }
  API.isProbeRecord = isProbeRecord;

  function synthToJson(type, rec) {
    var def = { type: type, inputsInline: true };
    var n = 0;
    (rec.inputs || []).forEach(function (inp) {
      if (!inp) return;
      var args = [];
      (inp.fields || []).forEach(function (f) {
        var j = probeFieldToJson(f);
        if (j) args.push(j);
      });
      var token = null;
      if (inp.type === IN_VALUE) token = { type: 'input_value', name: inp.name || ('VALUE-' + n) };
      else if (inp.type === IN_STATEMENT) token = { type: 'input_statement', name: inp.name || ('DO' + n) };
      // A DUMMY ROW'S NAME IS NOT DECORATION.
      //
      // The site uses named empty rows as markers - ENDPARAMS is where a
      // subroutine call's parameters stop, OBJECTTYPE_DUMMY is the row a rule
      // hides when its event is not Ongoing - and it uses PARAMETERS to hold
      // the labels. Dropping the name threw all three away, and an empty one
      // was dropped whole, so the behaviour had nothing to aim at.
      else if (args.length || inp.name) {
        token = { type: 'input_dummy' };
        if (inp.name) token.name = inp.name;
      }
      if (!token) return;                      // a nameless empty row draws nothing
      if (inp.check) token.check = inp.check;
      args.push(token);
      def['message' + n] = args.map(function (_, i) { return '%' + (i + 1); }).join(' ');
      def['args' + n] = args;
      n++;
    });
    if (rec.output) def.output = rec.output;
    if (rec.previousStatement) def.previousStatement = rec.previousStatement;
    if (rec.nextStatement) def.nextStatement = rec.nextStatement;
    // The style name is the site's own and carries the colour with it. The
    // recorded colour is what the theme resolved it to, which for nearly every
    // type is #000000 and would paint the whole canvas black.
    if (rec.style) def.style = rec.style;
    else if (rec.colour && rec.colour !== '#000000') def.colour = rec.colour;
    if (rec.tooltip) def.tooltip = rec.tooltip;
    return def;
  }
  API.synthToJson = synthToJson;

  // Live definitions: the object the site's own bundle hands to console.debug,
  // keyed by type, each value a Blockly JSON definition. The hand-written types
  // above always win; their option lists are still read out of the live object.
  function installDefinitions(defs, source) {
    // Kept unconverted, because the conversion resolves pictures against the
    // style and the style can arrive afterwards.
    state.rawReadings = defs || {};
    state.iconsDropped = 0;
    // Converted ONCE, here, and kept converted. Everything downstream reads
    // definitions as Blockly JSON - optionsFor goes looking for argsN - so
    // leaving the raw reading in place would hand it a shape it cannot read.
    var ready = {};
    for (var t in (defs || {})) {
      if (!Object.prototype.hasOwnProperty.call(defs, t)) continue;
      var raw = defs[t];
      if (!raw || typeof raw !== 'object' || Array.isArray(raw)) continue;
      ready[t] = isProbeRecord(raw) ? synthToJson(t, raw) : raw;
    }
    state.rawDefs = ready;
    var installed = 0;
    for (var type in ready) {
      if (!Object.prototype.hasOwnProperty.call(ready, type)) continue;
      if (MUTATOR_TYPES.indexOf(type) >= 0) continue;
      installJsonBlock(type, ready[type]);
      installed++;
    }

    // THE HAND-WRITTEN SEVEN FIRST, THEN THE SITE OVER THE TOP.
    //
    // defineMutators is what an editor with no site reading has to fall back
    // on, so it still runs and still defines all seven. Where the site DID
    // hand us a reading, that reading then replaces the shape and keeps only
    // the behaviour, because a hand-written init is a guess at a block
    // somebody else designed and every one of the seven measured wrong.
    defineMutators();
    var fromSite = 0;
    MUTATOR_TYPES.forEach(function (t) {
      if (!ready[t]) return;
      if (installMutatorFromSite(t, ready[t])) { fromSite++; installed++; }
    });
    state.mutatorsFromSite = fromSite;
    // Defining the blocks again threw away the colour pass that had been
    // wrapped onto them, so a definition refresh used to silently put the
    // invented palette back. It is re-applied here, on every path in.
    try { applyBlockColours(); } catch (e) {}
    state.definitionsSource = source || 'live';
    return installed;
  }
  API.installDefinitions = installDefinitions;

  // =========================================================================
  // THE SEVEN BLOCKS WE WRITE OURSELVES, BUILT FROM THE SITE'S READING
  //
  // These carry mutators: a rule changes shape when its event stops being
  // Ongoing, a subroutine call grows a socket per parameter, an If grows an
  // ELSE IF. Blockly cannot express any of that in json, so their init was
  // written by hand - and a hand-written init is a guess at a block somebody
  // else designed. Measured against the site, every one of them was wrong:
  //
  //   modBlock              a RULES label the site does not draw
  //   ruleBlock             EVENT/CONDITIONS/ACTIONS shouted in capitals where
  //                         the site writes Event, Conditions, Actions, and the
  //                         object dropdown crammed into the event row instead
  //                         of its own. 590 wide on the site, 316 here
  //   subroutineBlock       the parameter row named PARAMS, not PARAMETERS,
  //                         and no PARAMETER_LABELS field at all
  //   subroutineInstance    a CALL label the site does not draw, and no
  //                         ENDPARAMS marker to put the parameters in front of
  //   subroutineArgument    labelled ARG where the site says
  //                         GetSubroutineArgument. 297 wide on the site, 121
  //   If                    a DO label the site does not draw
  //
  // So the SHAPE now comes from the site's own reading, exactly as it does for
  // the other 597 types, and only the BEHAVIOUR is ours. What is written here
  // is the part json cannot carry and nothing else.
  //
  // The field names the site uses are the ones we already serialize - NAME,
  // EVENTTYPE, OBJECTTYPE, SUBROUTINE_NAME, ARGUMENT_INDEX - so no save
  // changes meaning. The input names that differ are all DUMMY inputs, which
  // are not serialized at all.
  var MUTATOR_BEHAVIOUR = {
    modBlock: {
      after: function () {
        this.setDeletable(false);
        this.setTooltip('The mod root. Every rule hangs off this block.');
      }
    },

    ruleBlock: {
      after: function () {
        var self = this;
        this.isOngoing_ = true;
        this.updateShape_();
        var f = this.getField('EVENTTYPE');
        if (f && f.setValidator) {
          f.setValidator(function (v) {
            self.isOngoing_ = (v === 'Ongoing');
            self.updateShape_();
            return v;
          });
        }
      },
      // The object dropdown lives in its own row on the site, named
      // OBJECTTYPE_DUMMY, and that whole row comes and goes with the event
      // kind. Ours used to add and remove a FIELD on the event row instead,
      // which is a different block.
      updateShape_: function () {
        var row = this.getInput('OBJECTTYPE_DUMMY');
        if (this.isOngoing_ && row) { row.setVisible(true); }
        else if (!this.isOngoing_ && row) { row.setVisible(false); }
        // A HIDDEN ROW IS STILL SAVED.
        //
        // Hiding the row got the drawing right - every ruleBlock measures the
        // site's width to the pixel - and left the FILE wrong: the field is
        // still on the block, so Blockly writes it, and our export carried
        // OBJECTTYPE on 30 rules where the site's carried none. Read off the
        // site's own export of the same experience: OBJECTTYPE exists only
        // when the event is Ongoing. All 14 that have it are Ongoing; all 30
        // without are specific events, which take their scope from the event.
        //
        // Blockly's isSerializable() is name && SERIALIZABLE, and SERIALIZABLE
        // is a public property - the same one the capture reads off the site.
        // Setting it is enough to keep the field out of the file while
        // leaving the block, its value and its layout exactly as they are, so
        // switching the event back to Ongoing still has the old scope in it.
        // AND SERIALIZABLE ALONE IS NOT ENOUGH, BECAUSE EDITABLE OVERRIDES IT.
        //
        // Blockly's isSerializable is:
        //   name && (SERIALIZABLE ? true : EDITABLE && <warn> && true)
        // so an EDITABLE field is written to the file even with SERIALIZABLE
        // false. Setting the flag alone changed nothing and the probe said so:
        // serializable false, isSerializable() still true, field still in the
        // file. Asking the field what it decided beat reasoning about it.
        //
        // The row is hidden in this state, so there is nothing to type in
        // either way, and both flags come back when the event returns to
        // Ongoing - with the scope the user last chose still in it.
        try {
          var f = this.getField('OBJECTTYPE');
          if (f) {
            var on = !!this.isOngoing_;
            f.SERIALIZABLE = on;
            if (f.setEditable) f.setEditable(on); else f.EDITABLE = on;
          }
        } catch (e) {}
        try { if (this.rendered) this.render(); } catch (e) {}
      },
      saveExtraState: function () { return { isOngoingEvent: !!this.isOngoing_ }; },
      loadExtraState: function (s) {
        this.isOngoing_ = !!(s && s.isOngoingEvent);
        this.updateShape_();
      }
    },

    subroutineBlock: {
      after: function () { this.params_ = []; },
      saveExtraState: function () {
        return {
          subroutineName: this.getFieldValue('SUBROUTINE_NAME') || '',
          parameters: (this.params_ || []).map(function (p) {
            return { types: p.types, name: p.name };
          })
        };
      },
      loadExtraState: function (s) {
        this.params_ = (s && s.parameters ? s.parameters : []).map(function (p) {
          return { types: p.types, name: p.name };
        });
        this.subName_ = (s && s.subroutineName) || '';
        renderParamLabels(this, 'PARAMETERS', this.params_);
      }
    },

    subroutineInstanceBlock: {
      after: function () { this.params_ = []; },
      saveExtraState: function () {
        return {
          subroutineName: this.getFieldValue('SUBROUTINE_NAME') || '',
          parameters: (this.params_ || []).map(function (p) {
            return { types: p.types, name: p.name };
          })
        };
      },
      loadExtraState: function (s) {
        this.params_ = (s && s.parameters ? s.parameters : []).map(function (p) {
          return { types: p.types, name: p.name };
        });
        this.updateShape_();
      },
      // ENDPARAMS is what the site's own reading is for: an empty row that
      // marks where the parameters stop. The sockets go in FRONT of it.
      updateShape_: function () {
        var i = 0;
        while (this.getInput('PARAM-' + i)) { this.removeInput('PARAM-' + i); i++; }
        for (var k = 0; k < this.params_.length; k++) {
          var inp = this.appendValueInput('PARAM-' + k)
            .appendField(String(this.params_[k].name || ('arg' + k)));
          if (this.getInput('ENDPARAMS')) {
            try { this.moveInputBefore('PARAM-' + k, 'ENDPARAMS'); } catch (e) {}
          }
        }
      }
    },

    // THE ARGUMENT DROPDOWN SHOWS A NAME AND STORES AN INDEX.
    //
    // Its option list is built from whichever subroutine the block sits in, so
    // the reading carries one pair - index "1" against the parameter's own
    // name, "Enable". Replacing that list with a bare 0,1,2,3 threw the names
    // away and drew the index instead, which is 38 pixels narrower and says
    // nothing. loosenDropdowns already keeps the site's pairs AND accepts a
    // value from an older save, so there is nothing to add here.
    subroutineArgumentBlock: {
      // ITS OPTIONS BELONG TO THE SUBROUTINE IT SITS IN, so they cannot be
      // baked into a shape at all. The site's reading carries exactly one
      // pair, index '1' against the parameter's own name 'Enable', because
      // that is the subroutine the captured block happened to be inside.
      // Applying that one pair to all 86 argument blocks drew another
      // subroutine's parameter name on every one of them, which is worse
      // than the bare index it replaced: wrong rather than merely terse.
      //
      // So the list is built when it is opened, from the parameters of the
      // subroutine this block is actually inside. An argument block only
      // means anything in a subroutine, so the root IS the subroutine.
      after: function () {
        var self = this;
        var input = null;
        (this.inputList || []).forEach(function (i) {
          (i.fieldRow || []).forEach(function (x) { if (x.name === 'ARGUMENT_INDEX') input = i; });
        });
        if (!input) return;
        var was = this.getFieldValue('ARGUMENT_INDEX');
        input.removeField('ARGUMENT_INDEX', true);
        var f = new B.FieldDropdown(function () {
          var params = [];
          try {
            var root = self.getRootBlock();
            if (root && root.type === 'subroutineBlock') params = root.params_ || [];
          } catch (e) {}
          var list = params.map(function (p, i) {
            return [String(p.name || ('arg' + i)), String(i)];
          });
          // Nothing to go on, or a saved index the subroutine no longer has:
          // the index itself is shown rather than nothing at all.
          var cur = f && f.getValue();
          if (cur && !list.some(function (r) { return r[1] === cur; })) list.unshift([String(cur), String(cur)]);
          if (!list.length) list.push(['0', '0']);
          return list;
        });
        f.doClassValidation_ = function (v) { return v === undefined ? null : String(v); };
        f.bf6Loose = true;
        input.appendField(f, 'ARGUMENT_INDEX');
        try { f.setValue(String(was == null ? '0' : was)); } catch (e) {}
      }
    },

    If: {
      // THE GEAR IS A REAL BLOCKLY MUTATOR, AND THE SITE'S IS THE STANDARD
      // ONE. Asked on the live page, the If block's icon reports
      // opens=['controls_if_elseif','controls_if_else'] and the block carries
      // decompose, compose and saveConnections. Both sub-blocks are already
      // defined here, because they come over with the other 604 readings. So
      // nothing has to be invented: the standard three methods are written
      // against the counts we already keep, and the icon is hung on.
      after: function () {
        this.elseifCount_ = 0;
        this.elseCount_ = 0;
        try {
          if (B.icons && B.icons.MutatorIcon && this.setMutator) {
            this.setMutator(new B.icons.MutatorIcon(
              ['controls_if_elseif', 'controls_if_else'], this));
          }
        } catch (e) { /* no gear is better than no block */ }
      },

      // What the bubble is filled with when it opens: a container with one
      // sub-block per else-if and one for the else, in that order.
      decompose: function (workspace) {
        // initSvg only exists on a rendered block. The bubble is rendered in
        // the editor, so it is there in practice, but a headless workspace has
        // no svg to make and asking for it throws.
        var draw = function (b) { if (b && b.initSvg) b.initSvg(); return b; };
        var container = draw(workspace.newBlock('controls_if_if'));
        var connection = container.nextConnection;
        for (var i = 1; i <= this.elseifCount_; i++) {
          var elseif = draw(workspace.newBlock('controls_if_elseif'));
          connection.connect(elseif.previousConnection);
          connection = elseif.nextConnection;
        }
        if (this.elseCount_) {
          var elseBlock = draw(workspace.newBlock('controls_if_else'));
          connection.connect(elseBlock.previousConnection);
        }
        return container;
      },

      // And back: the counts are read off the bubble, the shape is rebuilt,
      // and anything that was plugged in is plugged back where it belongs.
      compose: function (container) {
        var clause = container.nextConnection && container.nextConnection.targetBlock();
        this.elseifCount_ = 0;
        this.elseCount_ = 0;
        var valueConnections = [null];
        var statementConnections = [null];
        var elseStatementConnection = null;
        while (clause && !clause.isInsertionMarker()) {
          if (clause.type === 'controls_if_elseif') {
            this.elseifCount_++;
            valueConnections.push(clause.valueConnection_);
            statementConnections.push(clause.statementConnection_);
          } else if (clause.type === 'controls_if_else') {
            this.elseCount_++;
            elseStatementConnection = clause.statementConnection_;
          }
          clause = clause.nextConnection && clause.nextConnection.targetBlock();
        }
        this.updateShape_();
        for (var i = 1; i <= this.elseifCount_; i++) {
          reconnect(valueConnections[i], this, 'IF' + i);
          reconnect(statementConnections[i], this, 'DO' + i);
        }
        reconnect(elseStatementConnection, this, 'ELSE');
      },

      // Remembered before the shape is torn down, so a rule that was inside
      // an else-if is still inside it afterwards. Without this, opening the
      // bubble and closing it again silently empties the block.
      saveConnections: function (container) {
        var clause = container.nextConnection && container.nextConnection.targetBlock();
        var i = 1;
        while (clause) {
          if (clause.type === 'controls_if_elseif') {
            var inputIf = this.getInput('IF' + i);
            var inputDo = this.getInput('DO' + i);
            clause.valueConnection_ = inputIf && inputIf.connection.targetConnection;
            clause.statementConnection_ = inputDo && inputDo.connection.targetConnection;
            i++;
          } else if (clause.type === 'controls_if_else') {
            var inputElse = this.getInput('ELSE');
            clause.statementConnection_ = inputElse && inputElse.connection.targetConnection;
          }
          clause = clause.nextConnection && clause.nextConnection.targetBlock();
        }
      },
      saveExtraState: function () {
        if (!this.elseifCount_ && !this.elseCount_) return {};
        return { elseif: this.elseifCount_, else: this.elseCount_ };
      },
      loadExtraState: function (s) {
        this.elseifCount_ = (s && s.elseif) || 0;
        this.elseCount_ = (s && s['else']) || 0;
        this.updateShape_();
      },
      updateShape_: function () {
        var i = 1;
        while (this.getInput('IF' + i)) {
          this.removeInput('IF' + i);
          if (this.getInput('DO' + i)) this.removeInput('DO' + i);
          i++;
        }
        if (this.getInput('ELSE')) this.removeInput('ELSE');
        for (var k = 1; k <= this.elseifCount_; k++) {
          this.appendValueInput('IF' + k).appendField('Else If');
          this.appendStatementInput('DO' + k);
        }
        if (this.elseCount_) this.appendStatementInput('ELSE').appendField('Else');
      },
      bf6AddElseIf: function () { this.elseifCount_++; this.updateShape_(); },
      bf6ToggleElse: function () { this.elseCount_ = this.elseCount_ ? 0 : 1; this.updateShape_(); }
    }
  };

  // A dropdown built by jsonInit refuses a value that is not on its list, and
  // a save can legitimately hold one: the site's option lists are captured at
  // one moment and a project can be older. Every dropdown the site gave us is
  // rebuilt loose, keeping the site's options and its current value.
  function loosenDropdowns(block) {
    (block.inputList || []).forEach(function (inp) {
      (inp.fieldRow || []).slice().forEach(function (f) {
        if (!f || !f.name) return;
        if (!(f instanceof B.FieldDropdown)) return;
        if (f.bf6Loose) return;
        var opts = [];
        try { opts = f.getOptions(false) || []; } catch (e) {}
        var value = null;
        try { value = f.getValue(); } catch (e) {}
        var at = inp.fieldRow.indexOf(f);
        var name = f.name;
        inp.removeField(name, true);
        var loose = looseDropdown(opts, value);
        loose.bf6Loose = true;
        inp.appendField(loose, name);
        // appendField puts it last; put it back where the site had it.
        if (at >= 0 && at < inp.fieldRow.length - 1) {
          var moved = inp.fieldRow.pop();
          inp.fieldRow.splice(at, 0, moved);
        }
      });
    });
  }

  // Plug a remembered connection back into a named socket, if both are
  // still there. Blockly's own helper for this is not exported, so it is
  // written out: quietly doing nothing is the right answer when a socket
  // the bubble removed is gone.
  function reconnect(connection, block, inputName) {
    if (!connection) return;
    var input = block.getInput(inputName);
    if (!input || !input.connection) return;
    try { input.connection.connect(connection); } catch (e) {}
  }

  function installMutatorFromSite(type, def) {
    var behaviour = MUTATOR_BEHAVIOUR[type];
    if (!behaviour) return false;
    var copy = JSON.parse(JSON.stringify(def));
    delete copy.type;
    var block = {
      init: function () {
        this.jsonInit(copy);
        try { loosenDropdowns(this); } catch (e) {}
        if (behaviour.after) behaviour.after.call(this);
        try { decorateSockets(this, type); } catch (e) {}
      }
    };
    Object.keys(behaviour).forEach(function (k) {
      if (k !== 'after') block[k] = behaviour[k];
    });
    B.Blocks[type] = block;
    state.types[type] = 'site-mutator';
    return true;
  }
  API.installMutatorFromSite = installMutatorFromSite;

  function installJsonBlock(type, def) {
    var copy = JSON.parse(JSON.stringify(def));
    delete copy.type;
    B.Blocks[type] = {
      init: function () {
        try { this.jsonInit(copy); }
        catch (e) { fallbackInit(this, type, null, null); }
        try { decorateSockets(this, type); } catch (e) {}
      }
    };
    state.types[type] = 'json';
  }

  // Offline: no site, no cache. The mined type catalogue gives every type's
  // kind, its input names and its field names; the workspace being opened
  // gives the shape those inputs actually take. Together they are enough to
  // hold a real project without losing a single value, which is all the
  // offline mode promises.
  function installFallback(types, observed) {
    defineMutators();
    var mined = {};
    (types || []).forEach(function (t) { if (t && t.type) mined[t.type] = t; });
    state.mined = mined;
    observed = observed || { inputs: {}, fields: {} };
    var names = {};
    Object.keys(mined).forEach(function (t) { names[t] = 1; });
    Object.keys(observed.inputs || {}).forEach(function (t) { names[t] = 1; });
    Object.keys(observed.fields || {}).forEach(function (t) { names[t] = 1; });
    var n = 0;
    Object.keys(names).forEach(function (type) {
      if (MUTATOR_TYPES.indexOf(type) >= 0) return;
      // FALLBACK MEANS FALLBACK. IT FILLS GAPS, IT DOES NOT OVERWRITE.
      //
      // This replaced EVERY type it knew a mined record for, including the
      // 604 built from the site's own readings - and growDefinitions calls
      // it every time a file is opened. So opening a project silently threw
      // the site definitions away and rebuilt the whole workspace from the
      // approximation: every head badge gone, every block 54px narrower,
      // 96% of leaf blocks matching the site down to 42%.
      //
      // A type that already has the site's own definition keeps it. The
      // offline path is untouched, because offline nothing has one.
      var have = state.types[type];
      if (have === 'json' || have === 'site-mutator') return;
      var m = mined[type] || null;
      var obs = {
        inputs: (observed.inputs && observed.inputs[type]) || {},
        fields: (observed.fields && observed.fields[type]) || {},
        kind: observed.kind ? observed.kind[type] : null,
        seen: (observed.seen && observed.seen[type]) || {}
      };
      B.Blocks[type] = {
        init: function () { fallbackInit(this, type, m, obs); }
      };
      state.types[type] = 'fallback';
      n++;
    });
    // Defining the blocks again threw away the colour pass that had been
    // wrapped onto them, so a definition refresh used to silently put the
    // invented palette back. It is re-applied here, on every path in.
    try { applyBlockColours(); } catch (e) {}
    state.definitionsSource = 'fallback';
    return n;
  }
  API.installFallback = installFallback;

  // Input names the site uses for statement slots. CONDITION (singular, on
  // conditionBlock) is a VALUE slot and must not be caught by this.
  var STATEMENT_INPUT = /^(DO\d*|ELSE|ACTIONS|CONDITIONS|RULES)$/;

  function inputKind(name, observedKind) {
    if (observedKind) return observedKind;
    if (STATEMENT_INPUT.test(name)) return 'statement';
    return 'value';
  }

  function fallbackInit(block, type, mined, obs) {
    var made = {};
    // A LITERAL IS ITS VALUE, AND NOTHING ELSE.
    //
    // The site draws a Number block as the number, a Text block as the quoted
    // words, a Boolean as the dropdown. It does NOT write the type on them.
    // Every other value block does carry its name - CreateVector, CountOf,
    // GetVariable all say so - which is why this is a short list of literals
    // rather than a rule about value blocks.
    var LITERAL = { Number: 1, Text: 1, Boolean: 1 };
    var head = block.appendDummyInput('BF6HEAD');
    if (!LITERAL[type]) head.appendField(type);
    // fields first, so a value the workspace carries always has a home
    var fieldNames = [];
    // THE CHOICES COME WITH THE CATALOGUE. 187 fields across 105 types carry
    // the exact list the site offers - true/false on a Boolean, the compare
    // operators, the trim modes - and all of it was being thrown away here,
    // so every one of them was drawn as a box you type into. A dropdown that
    // has to be typed into is not the same control, and the site's is what
    // the creator knows.
    var fieldSpec = {};
    if (mined && mined.inputs) {
      mined.inputs.forEach(function (inp) {
        (inp.fields || []).forEach(function (f) {
          if (!f || !f.name) return;
          fieldNames.push(f.name);
          fieldSpec[f.name] = f;
        });
      });
    }
    Object.keys((obs && obs.fields) || {}).forEach(function (f) {
      if (fieldNames.indexOf(f) < 0) fieldNames.push(f);
    });
    fieldNames.forEach(function (fname) {
      if (block.getField(fname)) return;
      // THE DECORATIONS ARE THE QUOTE MARKS.
      //
      // A Text block on the site is written the way a quotation is: a pair of
      // heavy quote marks with the words between them, and no type name at
      // all. VARICON0 and VARICON1 are those marks, and they were being
      // skipped, so ours came out reading "Text container2" where the site
      // reads a quoted string. They are still never serialized; they are
      // drawn.
      if (/^VARICON|^ICON-/.test(fname)) {
        // ONLY TEXT IS QUOTED. The catalogue records these decorations on
        // Number as well, but the site draws quote marks round words, not
        // round a number: on the site a Number block is a bare grey field.
        // Drawing them everywhere put quotes around every value on the
        // canvas.
        var deco = (type === 'Text') ? blockImageFor(fname) : '';
        if (deco) {
          try {
            head.appendField(new B.FieldImage(deco, QUOTE_PX, QUOTE_PX, '"'), 'BF6D_' + fname);
          } catch (e) {}
        }
        return;
      }
      var sample = obs && obs.fields ? obs.fields[fname] : undefined;
      var spec = fieldSpec[fname];
      var choices = spec && spec.options && spec.options.length ? spec.options : null;

      // A CHOICE IS A DROPDOWN even when this workspace never used it. The
      // list is the site's own, so the control is real and writes only a value
      // the site offers, which is the reason the "never invent a serializable
      // field" rule below does not have to apply to it.
      if (choices) {
        // THE WORDING IS THE CATALOGUE'S, THE VALUE IS THE SITE'S.
        //
        // The catalogue lists what a choice READS AS - "true", "false" - while
        // the file stores what it IS - "TRUE", "FALSE". Offering the reading as
        // the value would write a casing the site never uses and quietly
        // change the project. So each choice keeps its wording and takes its
        // value from what this workspace actually stores for that field,
        // matched without regard to case. A value the catalogue does not list
        // at all is still offered, because the file is the authority on what is
        // legal, not our mined copy of the site.
        var seen = (obs && obs.seen && obs.seen[fname]) || [];
        var rows = choices.map(function (o) {
          var hit = null;
          for (var s = 0; s < seen.length; s++) {
            if (String(seen[s]).toLowerCase() === String(o).toLowerCase()) { hit = seen[s]; break; }
          }
          return [String(o), String(hit === null ? o : hit)];
        });
        seen.forEach(function (v) {
          if (!rows.some(function (r) { return r[1] === String(v); })) rows.push([String(v), String(v)]);
        });
        head.appendField(looseDropdown(rows, sample !== undefined ? sample : rows[0][1]), fname);
        return;
      }

      // A field the workspace never carries must NOT become a serializable one:
      // an empty text field would write itself into the save and the project
      // would come back different from the one the site holds. It shows as a
      // label instead, which is honest about the shape and writes nothing.
      if (sample === undefined) { head.appendField(new B.FieldLabel(fname), 'BF6L_' + fname); return; }
      var f;
      if (fname === 'NUM' || typeof sample === 'number') f = new B.FieldNumber(0);
      else if (fname === 'VAR') f = new API.FieldVariable(null, null, null, null);
      else f = new B.FieldTextInput(String(sample));
      head.appendField(f, fname);
    });
    // inputs
    var order = [];
    if (mined && mined.inputs) {
      mined.inputs.forEach(function (inp) { if (inp.name) order.push(inp.name); });
    }
    Object.keys((obs && obs.inputs) || {}).forEach(function (nm) {
      if (order.indexOf(nm) < 0) order.push(nm);
    });
    order.sort(function (a, b) {
      var ra = /-(\d+)$/.exec(a), rb = /-(\d+)$/.exec(b);
      if (ra && rb && a.replace(/-\d+$/, '') === b.replace(/-\d+$/, '')) return +ra[1] - +rb[1];
      return 0;
    });
    order.forEach(function (nm) {
      if (made[nm] || nm === 'NAME') return;
      made[nm] = 1;
      var kind = inputKind(nm, obs && obs.inputs ? obs.inputs[nm] : null);
      if (kind === 'statement') { block.appendStatementInput(nm).appendField(nm); return; }
      // A value socket says what fits, never its own internal name. VALUE-0 is
      // the site's name for argument 0; the creator needs to be told it wants a
      // Player. The signature already knows, so it is asked.
      var inp = block.appendValueInput(nm);
      var wants = slotWants(type, nm);
      // The socket REMEMBERS what it accepts; the picture is made later, and
      // only if the socket turns out to be empty. Nothing is drawn here.
      //
      // Never restricted either: an offline signature is a good hint and a bad
      // gate, and a wrong check would refuse a connection the site allows.
      noteSocketWants(inp, wants);
    });
    // What the block IS: how this very workspace uses it beats the catalogue,
    // and the catalogue beats a guess from the input names.
    var kind = (obs && obs.kind) || (mined && mined.kind);
    if (kind === 'value') block.setOutput(true);
    else if (kind === 'statement') { block.setPreviousStatement(true); block.setNextStatement(true); }
    else if (!kind) {
      if (Object.keys(made).some(function (k) { return inputKind(k) === 'statement'; })) {
        block.setPreviousStatement(true); block.setNextStatement(true);
      } else block.setOutput(true);
    }
    // ONE LINE, LIKE THE SITE. Every Portal block lays its inputs out inline:
    // "SetVariable [Global] [Variable] [GameModeStarted] [false]" reads as one
    // sentence. Stacked, the same block becomes three rows and a wall of them
    // is unreadable, which is the single biggest reason ours did not look like
    // the site.
    block.setInputsInline(true);

    // The site colours by what the block IS, and so does this: green for a
    // value, gold for an action, blue for a condition. Two greys for eight
    // hundred blocks threw away the fastest signal on the page.
    paintStyle(block, type === 'conditionBlock' ? 'condition-block-style'
      : kind === 'value' ? 'value-block-style' : 'action-block-style');
    if (mined && mined.tooltip) block.setTooltip(String(mined.tooltip).replace(/<[^>]*>/g, ''));
  }

  // Walk a serialized workspace and record, per type, which inputs are value
  // and which are statement, and one sample value per field. This is what makes
  // the offline definitions fit the project actually being opened.
  function observe(wsJson) {
    // seen: every DISTINCT value a field carries anywhere in the workspace.
    // fields keeps the first one, which is all most callers want; seen is what
    // makes a dropdown able to offer the real values in their real casing.
    var out = { inputs: {}, fields: {}, kind: {}, seen: {} };
    function note(node, asStatement) {
      if (!node || typeof node !== 'object') return;
      var t = node.type;
      if (!t) return;
      // asStatement null means "top level": that says nothing about the block,
      // since a free floating Text block is still a value block.
      if (asStatement !== null && !out.kind[t]) {
        out.kind[t] = asStatement ? 'statement' : 'value';
      }
      out.inputs[t] = out.inputs[t] || {};
      out.fields[t] = out.fields[t] || {};
      out.seen[t] = out.seen[t] || {};
      var fields = node.fields || {};
      for (var f in fields) if (Object.prototype.hasOwnProperty.call(fields, f)) {
        if (out.fields[t][f] === undefined) out.fields[t][f] = fields[f];
        var sv = fields[f];
        if (typeof sv === "string" || typeof sv === "number") {
          out.seen[t][f] = out.seen[t][f] || [];
          if (out.seen[t][f].indexOf(String(sv)) < 0) out.seen[t][f].push(String(sv));
        }
      }
      var inputs = node.inputs || {};
      for (var i in inputs) if (Object.prototype.hasOwnProperty.call(inputs, i)) {
        var child = inputs[i].block || inputs[i].shadow;
        var childIsStatement = !!(child && (child.next || STATEMENT_INPUT.test(i)));
        out.inputs[t][i] = childIsStatement ? 'statement' : 'value';
        note(child, childIsStatement);
        if (child && child.next) note(child.next.block, true);
      }
      if (node.next) note(node.next.block, true);
    }
    var root = wsJson && wsJson.mod ? wsJson.mod : wsJson;
    var blocks = (root && root.blocks && root.blocks.blocks) || [];
    blocks.forEach(function (b) { note(b, null); });
    return out;
  }
  API.observe = observe;

  // =========================================================================
  // Load and save
  // =========================================================================

  function unwrap(json) {
    if (!json) return { blocks: { languageVersion: 0, blocks: [] }, variables: [] };
    return json.mod ? json.mod : json;
  }
  API.unwrap = unwrap;

  function loadWorkspace(ws, json) {
    var st = unwrap(json);
    // Counted BEFORE anything is built, so the first block already knows
    // whether this workspace can afford per socket glyphs.
    noteWorkspaceSize(st);
    state.loading++;
    try {
      B.Events.disable();
      try { ws.clear(); B.serialization.workspaces.load(st, ws); }
      finally { B.Events.enable(); }
    } finally { state.loading--; }
    snapshotUnits(ws);
    return true;
  }
  API.loadWorkspace = loadWorkspace;



  function saveWorkspace(ws) { return B.serialization.workspaces.save(ws); }
  API.saveWorkspace = saveWorkspace;

  // =========================================================================
  // Sync units
  //
  // A unit is the smallest piece the site can be told to replace: every rule,
  // every free floating stack, and the mod shell itself. Sending the whole mod
  // for one edited number would push half a megabyte across for every
  // keystroke, so a rule is its own unit and carries where it hangs.
  // =========================================================================

  function unitsOf(ws) {
    var out = [];
    ws.getTopBlocks(false).forEach(function (top) {
      if (top.type === 'modBlock') {
        out.push(top);
        var inp = top.getInput('RULES');
        var b = inp && inp.connection && inp.connection.targetBlock();
        while (b) { out.push(b); b = b.getNextBlock(); }
      } else {
        out.push(top);
      }
    });
    return out;
  }
  API.unitsOf = unitsOf;

  function saveUnit(block) {
    return B.serialization.blocks.save(block, {
      addCoordinates: !block.getParent(),
      addNextBlocks: false,
      doFullSerialization: false
    });
  }
  API.saveUnit = saveUnit;

  function captureAnchor(block) {
    var conn = (block.previousConnection && block.previousConnection.targetConnection) ||
      (block.outputConnection && block.outputConnection.targetConnection);
    if (!conn) {
      var xy = block.getRelativeToSurfaceXY();
      return { x: Math.round(xy.x), y: Math.round(xy.y) };
    }
    var parent = conn.getSourceBlock();
    var input = null;
    try { input = conn.getParentInput(); } catch (e) { input = null; }
    return { parentId: parent.id, input: input ? input.name : null };
  }
  API.captureAnchor = captureAnchor;

  function reattach(ws, block, anchor) {
    if (!anchor) return;
    if (anchor.parentId) {
      var parent = ws.getBlockById(anchor.parentId);
      if (!parent) return;
      var target = null;
      if (anchor.input) {
        var inp = parent.getInput(anchor.input);
        target = inp && inp.connection;
      } else {
        target = parent.nextConnection;
      }
      if (!target) return;
      var mine = block.previousConnection || block.outputConnection;
      if (mine) { try { target.connect(mine); } catch (e) {} }
    } else if (anchor.x !== undefined) {
      var cur = block.getRelativeToSurfaceXY();
      block.moveBy(anchor.x - cur.x, anchor.y - cur.y);
    }
  }
  API.reattach = reattach;

  // Replace one unit in place: keep the id, keep where it hangs, keep whatever
  // was chained under it.
  function applyReplace(ws, msg) {
    state.applying++;
    state.loading++;
    try {
      B.Events.disable();
      try {
        var old = ws.getBlockById(msg.id);
        var anchor = msg.anchor || null;
        var tail = null;
        if (old) {
          if (!anchor) anchor = captureAnchor(old);
          if (old.nextConnection && old.nextConnection.targetBlock()) {
            tail = old.nextConnection.targetBlock();
            tail.unplug(false);
          }
          old.dispose(false);
        }
        var nb = B.serialization.blocks.append(msg.json, ws);
        reattach(ws, nb, anchor);
        if (tail && nb.nextConnection && tail.previousConnection) {
          try { nb.nextConnection.connect(tail.previousConnection); } catch (e) {}
        }
        return nb;
      } finally { B.Events.enable(); }
    } finally { state.applying--; state.loading--; }
  }
  API.applyReplace = applyReplace;

  function applyDelete(ws, msg) {
    state.applying++;
    try {
      B.Events.disable();
      try {
        var ids = msg.ids || [msg.id];
        ids.forEach(function (id) {
          var b = ws.getBlockById(id);
          if (b) b.dispose(false);
        });
      } finally { B.Events.enable(); }
    } finally { state.applying--; }
  }
  API.applyDelete = applyDelete;

  function applyVariables(ws, list) {
    state.applying++;
    state.loading++;
    try {
      B.Events.disable();
      try {
        (list || []).forEach(function (v) {
          if (!ws.getVariableById(v.id)) ws.createVariable(v.name, v.type || '', v.id);
        });
      } finally { B.Events.enable(); }
    } finally { state.applying--; state.loading--; }
  }
  API.applyVariables = applyVariables;

  function snapshotUnits(ws) {
    var map = {};
    unitsOf(ws).forEach(function (b) {
      map[b.id] = JSON.stringify(saveUnit(b));
    });
    state.units = map;
    return map;
  }
  API.snapshotUnits = snapshotUnits;

  // Diff the workspace against the last snapshot and return the messages that
  // carry it forward. Events tell us where to look; a delete makes us rescan.
  function diffUnits(ws) {
    var msgs = [];
    var seen = {};
    unitsOf(ws).forEach(function (b) {
      var json = saveUnit(b);
      var s = JSON.stringify(json);
      seen[b.id] = 1;
      if (state.units[b.id] !== s) {
        state.units[b.id] = s;
        msgs.push({ op: 'replaceTop', id: b.id, json: json, anchor: captureAnchor(b) });
      }
    });
    var gone = [];
    Object.keys(state.units).forEach(function (id) {
      if (!seen[id]) { gone.push(id); delete state.units[id]; }
    });
    if (gone.length) msgs.push({ op: 'deleteTop', ids: gone });
    return msgs;
  }
  API.diffUnits = diffUnits;

  function variableList(ws) {
    return ws.getAllVariables().map(function (v) {
      return { name: v.name, id: v.getId(), type: v.type };
    });
  }
  API.variableList = variableList;

  // =========================================================================
  // Placeholders and snippets
  // =========================================================================

  // {{OBJID:<kind>}}, {{VAR:<name>:<type>}}, {{TEXT:<label>}}
  function fillPlaceholders(node, ctx) {
    var text = JSON.stringify(node);
    var missing = [];
    text = text.replace(/\{\{OBJID:([A-Za-z]+)\}\}/g, function (m, kind) {
      var v = ctx.objId && ctx.objId(kind);
      if (v === null || v === undefined) { missing.push(m); return '0'; }
      return String(v);
    });
    var out = JSON.parse(text);
    (function walk(n) {
      if (!n || typeof n !== 'object') return;
      if (Array.isArray(n)) { n.forEach(walk); return; }
      if (n.fields) {
        for (var f in n.fields) if (Object.prototype.hasOwnProperty.call(n.fields, f)) {
          var val = n.fields[f];
          if (typeof val === 'string') {
            var mv = /^\{\{VAR:([^:}]+):([^}]*)\}\}$/.exec(val);
            var mt = /^\{\{TEXT:([^}]*)\}\}$/.exec(val);
            if (mv) n.fields[f] = { id: ctx.variable(mv[1], mv[2]) };
            else if (mt) n.fields[f] = ctx.text ? ctx.text(mt[1]) : mt[1];
          } else if (val && typeof val === 'object' && typeof val.bf6var === 'string') {
            var parts = val.bf6var.split(':');
            n.fields[f] = { id: ctx.variable(parts[0], parts[1] || '') };
          }
        }
      }
      for (var k in n) if (Object.prototype.hasOwnProperty.call(n, k)) walk(n[k]);
    })(out);
    return { json: out, missing: missing };
  }
  API.fillPlaceholders = fillPlaceholders;

  function stripIds(node) {
    (function walk(n) {
      if (!n || typeof n !== 'object') return;
      if (Array.isArray(n)) { n.forEach(walk); return; }
      if (n.type && n.id) delete n.id;
      for (var k in n) if (Object.prototype.hasOwnProperty.call(n, k)) walk(n[k]);
    })(node);
    return node;
  }
  API.stripIds = stripIds;

  // =========================================================================
  // ObjId analysis: which ids does the script address, and do they exist
  // =========================================================================

  function objIdRefs(ws) {
    var out = [];
    ws.getAllBlocks(false).forEach(function (b) {
      var kind = OBJID_BLOCKS[b.type];
      if (!kind) return;
      var inp = b.getInput('VALUE-0');
      var child = inp && inp.connection && inp.connection.targetBlock();
      if (!child || child.type !== 'Number') return;
      var v = Number(child.getFieldValue('NUM'));
      if (isNaN(v)) return;
      out.push({ blockId: b.id, numberId: child.id, kind: kind, objId: v });
    });
    return out;
  }
  API.objIdRefs = objIdRefs;

  function staleRefs(ws) {
    var live = {};
    state.objIds.forEach(function (r) { live[String(r.id)] = 1; });
    return objIdRefs(ws).filter(function (r) { return !live[String(r.objId)]; });
  }
  API.staleRefs = staleRefs;

  // =========================================================================
  // What Portal said about a save
  //
  // A rejected save is the site's verdict on OUR blocks, so it is shown on the
  // blocks it is about rather than as a number in a log. Warnings are tagged so
  // they can be cleared again without touching a warning the workspace set for
  // its own reasons.
  // =========================================================================
  var PORTAL_TAG = 'Portal: ';

  // Blockly has no way to read a warning back, and a second warning on the same
  // block would overwrite the first, so what Portal said is kept here by block
  // id and written to the block under its own warning id. The stale-ObjId
  // warning uses a different one and the two no longer fight.
  var WARN_PORTAL = 'bf6portal';

  function portalRelay(ws, result) {
    var applied = [], missing = [];
    state.portalWarnings = state.portalWarnings || {};
    ((result && result.blocks) || []).forEach(function (row) {
      var b = ws.getBlockById(row.id);
      if (!b) { missing.push(row.id); return; }
      var text = String(row.text || 'Portal rejected this block.');
      state.portalWarnings[row.id] = text;
      try { b.setWarningText(PORTAL_TAG + text, WARN_PORTAL); } catch (e) {}
      applied.push(row.id);
    });
    state.portalResult = result || null;
    return { applied: applied, missing: missing };
  }
  API.portalRelay = portalRelay;

  function clearPortalWarnings(ws) {
    var n = 0;
    var map = state.portalWarnings || {};
    Object.keys(map).forEach(function (id) {
      var b = ws.getBlockById(id);
      if (b) { try { b.setWarningText(null, WARN_PORTAL); } catch (e) {} }
      n++;
    });
    state.portalWarnings = {};
    state.portalResult = null;
    return n;
  }
  API.clearPortalWarnings = clearPortalWarnings;

  // The two findings that cause most INVALID_ARGUMENT rejections: an ObjId two
  // placed objects share, and an ObjId the script addresses that nothing
  // carries. Listed beside the site's verdict because they are usually its
  // cause.
  function lintFindings(ws) {
    var out = [];
    var byId = {};
    (state.objIds || []).forEach(function (r) {
      if (r.id === undefined || r.id < 0) return;
      (byId[String(r.id)] = byId[String(r.id)] || []).push(r.name || r.type || '?');
    });
    Object.keys(byId).forEach(function (id) {
      if (byId[id].length > 1) {
        out.push({ kind: 'duplicateObjId', objId: Number(id),
          text: 'ObjId ' + id + ' is on ' + byId[id].length + ' placed objects: ' + byId[id].join(', ') });
      }
    });
    staleRefs(ws).forEach(function (r) {
      out.push({ kind: 'staleObjId', objId: r.objId, blockId: r.blockId,
        text: 'ObjId ' + r.objId + ' is not on any placed ' + r.kind + ' in this level.' });
    });
    return out;
  }
  API.lintFindings = lintFindings;

  // =========================================================================
  // Sync units, read straight out of a saved workspace
  //
  // The same unit rule as unitsOf(), but on JSON rather than on a live
  // workspace, so a local save and whatever the site hands back can be compared
  // without loading either one. This is what makes coming back from a signed
  // out session a diff instead of a guess.
  // =========================================================================
  function unitsOfJson(wsJson) {
    var root = unwrap(wsJson);
    var out = {};
    var tops = (root.blocks && root.blocks.blocks) || [];
    function strip(node) {
      var copy = JSON.parse(JSON.stringify(node));
      delete copy.next;
      return copy;
    }
    tops.forEach(function (top) {
      if (top.type === 'modBlock') {
        var shell = JSON.parse(JSON.stringify(top));
        // the shell without its rules: the rules are units of their own
        if (shell.inputs && shell.inputs.RULES) delete shell.inputs.RULES;
        out[top.id] = { id: top.id, json: shell, anchor: { x: top.x || 0, y: top.y || 0 }, kind: 'mod' };
        var prev = null;
        var b = top.inputs && top.inputs.RULES ? top.inputs.RULES.block : null;
        while (b) {
          out[b.id] = {
            id: b.id, json: strip(b), kind: 'rule',
            anchor: prev ? { parentId: prev, input: null } : { parentId: top.id, input: 'RULES' }
          };
          prev = b.id;
          b = b.next ? b.next.block : null;
        }
      } else {
        out[top.id] = {
          id: top.id, json: strip(top), kind: 'top',
          anchor: { x: top.x || 0, y: top.y || 0 }
        };
      }
    });
    return out;
  }
  API.unitsOfJson = unitsOfJson;

  // Coming back after the site signed us out.
  //
  // Local wins for every unit the journal says we changed while we were cut
  // off; the site wins for every unit we never touched, because someone else,
  // or another tab, may have moved it. Anything identical is left alone.
  function reconcile(localJson, siteJson, journalIds) {
    var local = unitsOfJson(localJson);
    var site = unitsOfJson(siteJson);
    var touched = {};
    (journalIds || []).forEach(function (id) { touched[id] = 1; });
    var toSite = [], toLocal = [], same = 0, addedLocally = [], goneFromSite = [];
    Object.keys(local).forEach(function (id) {
      var l = local[id], s = site[id];
      var lt = JSON.stringify(l.json), st = s ? JSON.stringify(s.json) : null;
      if (s && lt === st) { same++; return; }
      if (!s) { addedLocally.push(l); toSite.push(l); return; }
      if (touched[id]) toSite.push(l);
      else toLocal.push(s);
    });
    Object.keys(site).forEach(function (id) {
      if (local[id]) return;
      // On the site, not here: adopt it, unless we deliberately deleted it.
      if (touched[id]) goneFromSite.push(id);
      else toLocal.push(site[id]);
    });
    return {
      toSite: toSite, toLocal: toLocal, deletedLocally: goneFromSite,
      addedLocally: addedLocally,
      stats: {
        localUnits: Object.keys(local).length,
        siteUnits: Object.keys(site).length,
        unchanged: same,
        pushed: toSite.length,
        pulled: toLocal.length,
        deleted: goneFromSite.length
      }
    };
  }
  API.reconcile = reconcile;

  function countWorkspace(wsJson) {
    var root = unwrap(wsJson);
    var blocks = 0, rules = 0, subs = 0;
    ((root.blocks && root.blocks.blocks) || []).forEach(function (b) {
      (function walk(node) {
        if (!node || typeof node !== 'object') return;
        if (node.type) {
          blocks++;
          if (node.type === 'ruleBlock') rules++;
          if (node.type === 'subroutineBlock') subs++;
        }
        var ins = node.inputs || {};
        for (var k in ins) if (Object.prototype.hasOwnProperty.call(ins, k)) {
          walk(ins[k].block); walk(ins[k].shadow);
        }
        if (node.next) walk(node.next.block);
      })(b);
    });
    return { blocks: blocks, rules: rules, subroutines: subs,
      variables: (root.variables || []).length };
  }
  API.countWorkspace = countWorkspace;

  // =========================================================================
  // Typed signatures
  //
  // Every Portal block carries its signature in its tooltip, which is how the
  // site tells a creator what fits where: "GetCapturePoint(Number): CapturePoint",
  // overloads separated by line breaks. Parsing it gives the slot search real
  // types even offline, without inventing a type system of our own.
  // =========================================================================

  var LITERAL_RET = {
    Number: 'Number', Text: 'String', Boolean: 'Boolean', CreateVector: 'Vector',
    Message: 'Message', EmptyArray: 'Array', AllPlayers: 'Array',
    AllCapturePoints: 'Array', AllVehicles: 'Array', CurrentArrayElement: 'Any Type'
  };

  function stripTags(html) {
    return String(html || '').replace(/<[^>]*>/g, '').replace(/&nbsp;/g, ' ')
      .replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>').trim();
  }
  API.stripTags = stripTags;

  function parseSignature(html) {
    var lines = String(html || '').split(/<br\s*\/?>/i).map(stripTags)
      .filter(function (l) { return l.length > 0; });
    var out = [];
    lines.forEach(function (line) {
      var m = /^([A-Za-z_][A-Za-z_0-9]*)\s*\(([^)]*)\)\s*(?::\s*(.+))?$/.exec(line);
      if (m) {
        out.push({
          name: m[1],
          args: m[2].trim() ? m[2].split(',').map(function (a) { return a.trim(); }) : [],
          ret: (m[3] || '').trim()
        });
        return;
      }
      var m2 = /^([A-Za-z_][A-Za-z_0-9]*)\s*:\s*(.+)$/.exec(line);
      if (m2) out.push({ name: m2[1], args: [], ret: m2[2].trim() });
    });
    return out;
  }
  API.parseSignature = parseSignature;

  // type -> [{name,args,ret}]. Live tooltips first, the mined catalogue after.
  function buildSignatures(tooltipsByType) {
    var sig = {};
    var src = tooltipsByType || state.tooltips || {};
    Object.keys(src).forEach(function (t) {
      var s = parseSignature(src[t]);
      if (s.length) sig[t] = s;
    });
    var mined = state.mined || {};
    Object.keys(mined).forEach(function (t) {
      if (sig[t]) return;
      var s = parseSignature(mined[t].tooltip || '');
      if (s.length) sig[t] = s;
    });
    Object.keys(LITERAL_RET).forEach(function (t) {
      if (!sig[t]) sig[t] = [{ name: t, args: [], ret: LITERAL_RET[t] }];
    });
    state.signatures = sig;
    return sig;
  }
  API.buildSignatures = buildSignatures;

  function tooltipOf(type) {
    if (state.tooltips && state.tooltips[type]) return state.tooltips[type];
    var m = (state.mined || {})[type];
    return (m && m.tooltip) || '';
  }
  API.tooltipOf = tooltipOf;

  function returnTypes(type) {
    var s = (state.signatures || {})[type];
    if (!s) return [];
    var out = [];
    s.forEach(function (o) {
      String(o.ret || '').split('|').forEach(function (one) {
        var t = one.trim();
        if (t && out.indexOf(t) < 0) out.push(t);
      });
    });
    return out;
  }
  API.returnTypes = returnTypes;

  // What a slot wants: argument i of the block's signature, for input VALUE-i,
  // PARAM-i or IF-i. Anything else, and anything typed "Any Type", takes
  // everything.
  function slotWants(blockType, inputName) {
    var m = /^(?:VALUE|PARAM|IF)-?(\d+)$/.exec(inputName || '');
    if (!m) return null;
    var idx = parseInt(m[1], 10);
    var s = (state.signatures || {})[blockType];
    if (!s) return null;
    var want = [];
    for (var i = 0; i < s.length; i++) {
      var a = s[i].args[idx];
      if (!a) continue;
      if (/any/i.test(a)) return null;
      // A slot that takes many kinds writes them as "Player | Team | Vehicle".
      a.split('|').forEach(function (one) {
        var t = one.trim();
        if (t && want.indexOf(t) < 0) want.push(t);
      });
    }
    return want.length ? want : null;
  }
  API.slotWants = slotWants;

  function typeFits(rets, wanted) {
    if (!wanted) return true;
    for (var i = 0; i < rets.length; i++) {
      if (wanted.indexOf(rets[i]) >= 0) return true;
      if (/any/i.test(rets[i])) return true;
    }
    return false;
  }

  function isStatementType(type) {
    var mined = (state.mined || {})[type];
    if (type === 'ruleBlock' || type === 'subroutineInstanceBlock' || type === 'If' ||
      type === 'conditionBlock') return true;
    if (type === 'modBlock' || type === 'subroutineBlock') return false;
    return !!(mined && mined.kind === 'statement');
  }
  API.isStatementType = isStatementType;

  // Everything that can go in this slot, best matches first. Statement slots
  // take statement blocks; a value slot takes value blocks whose return type
  // the slot accepts, and then, at the bottom, the ones whose type we do not
  // know, rather than hiding them.
  function slotCandidates(blockType, inputName, kindOfSlot, filterText) {
    var wanted = kindOfSlot === 'statement' ? null : slotWants(blockType, inputName);
    var filter = (filterText || '').toLowerCase();
    var exact = [], loose = [], unknown = [];
    Object.keys(B.Blocks).forEach(function (type) {
      if (type === 'modBlock') return;
      var statementish = isStatementType(type);
      var rets = returnTypes(type);
      var mined = (state.mined || {})[type];
      var valueish = (mined && mined.kind === 'value') || rets.length > 0;
      if (kindOfSlot === 'statement') { if (!statementish) return; }
      else if (statementish || !valueish) return;
      if (filter) {
        var hay = (type + ' ' + stripTags(tooltipOf(type)) + ' ' + categoryOf(type)).toLowerCase();
        if (hay.indexOf(filter) < 0) return;
      }
      var row = { type: type, rets: rets, category: categoryOf(type), tooltip: tooltipOf(type) };
      if (kindOfSlot === 'statement') exact.push(row);
      else if (!wanted) (rets.length ? exact : loose).push(row);
      else if (typeFits(rets, wanted)) exact.push(row);
      else if (!rets.length) unknown.push(row);
    });
    var byName = function (a, b) { return a.type < b.type ? -1 : a.type > b.type ? 1 : 0; };
    exact.sort(byName); loose.sort(byName); unknown.sort(byName);
    return { wanted: wanted, exact: exact, loose: loose, unknown: unknown };
  }
  API.slotCandidates = slotCandidates;

  // Which toolbox category a type sits in, so the slot search groups the way
  // the site's toolbox does.
  function buildCategoryIndex(toolbox) {
    var idx = {};
    (function walk(node, cat) {
      if (!node || typeof node !== 'object') return;
      var isCat = (node.kind === 'CATEGORY' || node.kind === 'category');
      var here = isCat ? (node.name || cat) : cat;
      if ((node.kind === 'BLOCK' || node.kind === 'block') && node.type) idx[node.type] = here || '';
      (node.contents || []).forEach(function (c) { walk(c, here); });
    })(toolbox, '');
    state.categories = idx;
    return idx;
  }
  API.buildCategoryIndex = buildCategoryIndex;

  function categoryOf(type) { return (state.categories || {})[type] || ''; }
  API.categoryOf = categoryOf;

  // =========================================================================
  // Zoom floor
  //
  // The site never zooms out far enough to see a whole project, which is how
  // people get lost in it. The floor here is computed from the work itself:
  // whatever scale shows every block with a little air around it. The default
  // scale for normal work is untouched.
  // =========================================================================
  function fitScale(bounds, viewW, viewH, pad) {
    if (!bounds || !bounds.width || !bounds.height) return 0.3;
    var p = (pad === undefined) ? 40 : pad;
    var sx = (viewW - p * 2) / bounds.width;
    var sy = (viewH - p * 2) / bounds.height;
    return Math.max(0.02, Math.min(sx, sy));
  }
  API.fitScale = fitScale;

  // =========================================================================
  // Site style
  //
  // What makes a Blockly workspace look like the Portal site is four things,
  // and none of them is guessable:
  //
  //   1. the RENDERER          the silhouette: notches, tabs, corners, hats
  //   2. its CONSTANT PROVIDER every measurement the silhouette is built from
  //   3. the THEME             block styles, category styles, component colours
  //   4. the injected CSS      toolbox, flyout, scrollbars, fonts, tooltips
  //
  // site_sync.js reads all four off the live page and the tool caches them.
  // Everything below takes that payload and turns it into the three things
  // Blockly wants: a theme object, a renderer name, and a per-type colour pass.
  // It is deliberately free of document and window so the node harness can
  // prove each step without a browser.
  //
  // Every section is optional. A payload with only a theme still applies the
  // theme; a payload with only constants still forces the geometry. That
  // matters because the offline default file is extracted from the site's
  // public bundle and may not recover every section.
  // =========================================================================

  var STYLE_SECTIONS = ['renderer', 'constants', 'theme', 'blockColours', 'css', 'categoryIcons'];

  function cloneJson(v) {
    try { return JSON.parse(JSON.stringify(v)); } catch (e) { return null; }
  }

  // ---- icons, and which writer they came from ------------------------------
  // Three writers can supply a category icon and they are not equal:
  //
  //   live      read off a signed in Portal page. That is the site itself, so
  //             it wins outright.
  //   mirror    the local mirror of the site's own public assets, inlined by
  //             the tool at startup. It fills whatever a partial capture
  //             missed, one key at a time, rather than being dropped whole
  //             because the capture had one icon in it.
  //   fallback  the offline default file, recovered from the public bundle.
  //
  // All three key an icon by the same name, because the toolbox names it once:
  // a category's cssconfig.icon is both the css class on its row and the base
  // name of the site's own svg. A live capture writes 'class:<name>' because it
  // read the class off the page; the other two write the bare name. Both forms
  // are accepted here and folded to 'class:<name>', which is what the css pass
  // wants.
  var ICON_TIERS = ['live', 'mirror', 'fallback'];
  API.ICON_TIERS = ICON_TIERS;

  function iconNameOf(key) {
    var s = String(key || '');
    return s.indexOf('class:') === 0 ? s.slice(6) : s;
  }
  API.iconNameOf = iconNameOf;

  // One icon, however it was written: a bare data url, a css background-image
  // value, or a record with any of dataUrl / url / backgroundImage / maskImage.
  function iconRecordOf(v) {
    if (v === null || v === undefined) return null;
    if (typeof v === 'string') {
      var s = v.trim();
      if (!s || s === 'none') return null;
      return { backgroundImage: /^url\(/i.test(s) ? s : 'url("' + s + '")' };
    }
    if (typeof v !== 'object') return null;
    var rec = cloneJson(v) || {};
    if (!rec.backgroundImage) {
      var src = rec.dataUrl || rec.url || rec.image || '';
      if (src) rec.backgroundImage = /^url\(/i.test(src) ? src : 'url("' + src + '")';
    }
    if (rec.backgroundImage === 'none') delete rec.backgroundImage;
    if (rec.maskImage === 'none') delete rec.maskImage;
    return rec;
  }

  // Does this record actually draw a picture, or is it only colours? A live
  // capture records a row's colours under the same map, and those must not be
  // read as "this category already has its icon".
  function iconDraws(rec) {
    return !!(rec && (rec.backgroundImage || rec.maskImage || rec.svg));
  }
  API.iconDraws = iconDraws;

  // Merge the tiers. Returns the map the css pass wants plus the bookkeeping
  // the status pill and the log report: how many icons each tier supplied and
  // which tier is the one in use.
  function mergeIconTiers(tiers) {
    var t = tiers || {};
    var out = {};
    var by = {};                                  // icon name -> tier that won
    var counts = { live: 0, mirror: 0, fallback: 0 };

    // The live map is copied whole and first, keys and all: it carries row
    // colours and name: entries the other tiers know nothing about.
    var live = t.live;
    if (live && typeof live === 'object') {
      Object.keys(live).forEach(function (k) {
        var rec = iconRecordOf(live[k]);
        if (!rec) return;
        out[k] = rec;
        if (String(k).indexOf('row:') === 0 || String(k).indexOf('name:') === 0) return;
        if (!iconDraws(rec)) return;
        var name = iconNameOf(k);
        if (by[name]) return;
        by[name] = 'live';
        counts.live++;
        out['class:' + name] = rec;
      });
    }

    ['mirror', 'fallback'].forEach(function (tier) {
      var src = t[tier];
      if (!src || typeof src !== 'object') return;
      Object.keys(src).forEach(function (k) {
        var name = iconNameOf(k);
        if (by[name]) return;
        var rec = iconRecordOf(src[k]);
        if (!iconDraws(rec)) return;
        by[name] = tier;
        counts[tier]++;
        out['class:' + name] = rec;
      });
    });

    var source = counts.live ? 'live' : counts.mirror ? 'mirror' : counts.fallback ? 'fallback' : 'none';
    return { icons: out, tiers: counts, source: source, by: by };
  }
  API.mergeIconTiers = mergeIconTiers;

  // What the toolbox actually asks for, against what the tiers resolved. This
  // is the number worth reporting: 78 icons in a mirror mean nothing if the
  // toolbox names a key none of them carry.
  function iconCoverage(toolbox, style) {
    var st = style || state.style;
    var by = (st && st.iconsBy) || {};
    var wanted = [];
    var seen = {};
    (function walk(node) {
      if (!node) return;
      if (Array.isArray(node)) { node.forEach(walk); return; }
      if (typeof node !== 'object') return;
      var css = node.cssconfig || node.cssConfig;
      var key = css && (css.icon || css.Icon);
      if (key && !seen[key]) { seen[key] = 1; wanted.push(String(key)); }
      if (node.contents) walk(node.contents);
    })(toolbox && toolbox.contents ? toolbox.contents : toolbox);

    var out = {
      categories: wanted.length,
      resolved: 0,
      byTier: { live: 0, mirror: 0, fallback: 0 },
      missing: []
    };
    wanted.forEach(function (key) {
      var tier = by[iconNameOf(key)];
      if (tier) { out.resolved++; out.byTier[tier]++; }
      else out.missing.push(key);
    });
    return out;
  }
  API.iconCoverage = iconCoverage;

  function isPlainScalar(v) {
    var t = typeof v;
    return t === 'number' || t === 'string' || t === 'boolean';
  }

  // The payload is normalised on the way in, because three different writers
  // produce it: the live page, the cache, and the offline extractor. Anything
  // that means the same thing is folded to one shape here rather than being
  // guarded at every use.
  function normalizeStyle(raw) {
    if (!raw || typeof raw !== 'object') return null;
    var out = {
      v: raw.v || 1,
      source: raw.source || 'live',
      capturedAt: raw.capturedAt || raw.at || '',
      url: raw.url || '',
      blocklyVersion: raw.blocklyVersion || '',
      note: raw.note || ''
    };

    // renderer: a string, or an object with a name and a registry key.
    var r = raw.renderer;
    if (typeof r === 'string') out.renderer = { name: r, registryKey: r };
    else if (r && typeof r === 'object') {
      out.renderer = {
        name: r.name || r.registryKey || '',
        registryKey: r.registryKey || r.name || '',
        className: r.className || '',
        registered: r.registered || []
      };
    }
    if (out.renderer && !out.renderer.name && !out.renderer.registryKey) delete out.renderer;

    // constants: a flat map, or split into scalars and paths by the writer.
    var c = raw.constants;
    if (c && typeof c === 'object') {
      var flat = {};
      var take = function (src) {
        if (!src || typeof src !== 'object') return;
        Object.keys(src).forEach(function (k) {
          var v = src[k];
          if (isPlainScalar(v) || (v && typeof v === 'object')) flat[k] = v;
        });
      };
      if (c.scalars || c.paths || c.objects) { take(c.scalars); take(c.objects); take(c.paths); }
      else take(c);
      if (Object.keys(flat).length) out.constants = flat;
    }

    // theme: nested, or spread across the top level.
    var t = raw.theme;
    if (!t && (raw.blockStyles || raw.categoryStyles || raw.componentStyles)) {
      t = {
        name: raw.themeName, blockStyles: raw.blockStyles, categoryStyles: raw.categoryStyles,
        componentStyles: raw.componentStyles, fontStyle: raw.fontStyle, startHats: raw.startHats
      };
    }
    if (t && typeof t === 'object') {
      out.theme = {
        name: t.name || 'bf6portal',
        blockStyles: cloneJson(t.blockStyles) || {},
        categoryStyles: cloneJson(t.categoryStyles) || {},
        componentStyles: cloneJson(t.componentStyles) || {},
        fontStyle: cloneJson(t.fontStyle) || {},
        startHats: !!t.startHats
      };
      var empty = !Object.keys(out.theme.blockStyles).length &&
        !Object.keys(out.theme.categoryStyles).length &&
        !Object.keys(out.theme.componentStyles).length &&
        !Object.keys(out.theme.fontStyle).length;
      if (empty) delete out.theme;
    }

    // per type colours: {type: {...}} or {type: '#hex'}
    var bc = raw.blockColours || raw.blockColors || raw.blockStylesByType;
    if (bc && typeof bc === 'object') {
      var map = {};
      Object.keys(bc).forEach(function (type) {
        var v = bc[type];
        if (typeof v === 'string' || typeof v === 'number') map[type] = { colour: v };
        else if (v && typeof v === 'object') map[type] = cloneJson(v) || {};
      });
      if (Object.keys(map).length) out.blockColours = map;
    }

    // css: a string, an array of strings, or {text, sources, fonts}
    var css = raw.css;
    if (typeof css === 'string') out.css = { text: css, sources: [], fonts: {} };
    else if (Array.isArray(css)) out.css = { text: css.join('\n'), sources: [], fonts: {} };
    else if (css && typeof css === 'object') {
      out.css = {
        text: typeof css.text === 'string' ? css.text :
          (Array.isArray(css.rules) ? css.rules.join('\n') : ''),
        sources: css.sources || [],
        // The custom properties the rules are written in terms of. This
        // object is rebuilt field by field rather than copied, so a field
        // added to the capture reaches here and is silently dropped: the
        // variables were captured correctly, written to disk correctly, and
        // then thrown away one step before they were used.
        vars: cloneJson(css.vars) || {},
        fonts: cloneJson(css.fonts) || {}
      };
    }
    if (out.css && !out.css.text && !Object.keys(out.css.fonts || {}).length) delete out.css;

    // Icons come in three tiers and are merged by name, not taken from one
    // writer. categoryIcons is the live capture; mirrorIcons is the local
    // mirror of the site's public assets; icons and valueTypeIcons are the
    // offline default file's own copy, which is the last word.
    var fallbackIcons = {};
    [raw.icons, raw.valueTypeIcons, raw.fallbackIcons].forEach(function (m) {
      if (m && typeof m === 'object') {
        Object.keys(m).forEach(function (k) { if (!fallbackIcons[k]) fallbackIcons[k] = m[k]; });
      }
    });
    var mirrorIcons = {};
    [raw.mirrorIcons, raw.mirrorValueTypeIcons].forEach(function (m) {
      if (m && typeof m === 'object') {
        Object.keys(m).forEach(function (k) { if (!mirrorIcons[k]) mirrorIcons[k] = m[k]; });
      }
    });
    var merged = mergeIconTiers({
      live: raw.categoryIcons, mirror: mirrorIcons, fallback: fallbackIcons
    });
    if (Object.keys(merged.icons).length) out.categoryIcons = merged.icons;
    out.iconTiers = merged.tiers;
    out.iconSource = merged.source;
    out.iconsBy = merged.by;

    // The site's own block images (1x1, quote0, quote1) and the directory they
    // live in, so Blockly's media path can point at the mirror instead of at
    // the network. Both are handed over by the tool; neither is invented here.
    // The socket shape rules, measured on the site. Kept whole: the shape a
    // socket is drawn with is decided by a method, and a method is the one
    // thing a json capture cannot carry, so this is the record of what that
    // method DID when it was asked.
    if (raw.fieldClasses && typeof raw.fieldClasses === 'object') {
      out.fieldClasses = cloneJson(raw.fieldClasses);
    }

    if (raw.socketRules && typeof raw.socketRules === 'object') {
      out.socketRules = cloneJson(raw.socketRules);
    }

    if (raw.blockImages && typeof raw.blockImages === 'object') {
      out.blockImages = cloneJson(raw.blockImages) || {};
    }
    if (typeof raw.mediaPath === 'string' && raw.mediaPath) out.mediaPath = raw.mediaPath;

    // The site's faces, as @font-face rules pointing at the mirrored files.
    if (typeof raw.fontCss === 'string' && raw.fontCss) out.fontCss = raw.fontCss;
    if (raw.fonts && typeof raw.fonts === 'object') out.fonts = cloneJson(raw.fonts) || {};
    if (Array.isArray(raw.fontsMissing)) out.fontsMissing = raw.fontsMissing.slice();
    if (raw.fontAliases && typeof raw.fontAliases === 'object') {
      out.fontAliases = cloneJson(raw.fontAliases) || {};
    }
    if (typeof raw.mirrorBuild === 'string') out.mirrorBuild = raw.mirrorBuild;

    return out;
  }
  API.normalizeStyle = normalizeStyle;

  // Hand a captured payload in. Returns the summary, so the caller can put the
  // counts straight on screen and in the log.
  function setStyle(raw) {
    var st = normalizeStyle(raw);
    state.style = st;
    state.styleRenderer = null;
    return styleSummary();
  }
  API.setStyle = setStyle;
  // The captured style itself, for anything that needs to SHOW what was
  // captured rather than apply it - the legend reads its swatches and symbols
  // straight out of here, so it can never describe a colour we do not draw.
  API.style = function () { return state.style || null; };

  function styleSummary() {
    var st = state.style;
    var out = {
      source: st ? st.source : 'none',
      capturedAt: st ? st.capturedAt : '',
      blocklyVersion: st ? st.blocklyVersion : '',
      renderer: '',
      rendererBase: '',
      theme: '',
      counts: { constants: 0, blockStyles: 0, categoryStyles: 0, componentStyles: 0,
        blockColours: 0, categoryIcons: 0, cssBytes: 0 },
      iconSource: st ? (st.iconSource || 'none') : 'none',
      iconTiers: (st && st.iconTiers) || { live: 0, mirror: 0, fallback: 0 },
      fonts: st && st.fonts ? Object.keys(st.fonts).length : 0,
      fontsMissing: (st && st.fontsMissing) || [],
      present: [],
      missing: STYLE_SECTIONS.slice()
    };
    if (!st) return out;
    out.present = STYLE_SECTIONS.filter(function (s) { return !!st[s]; });
    out.missing = STYLE_SECTIONS.filter(function (s) { return !st[s]; });
    if (st.renderer) out.renderer = st.renderer.name || st.renderer.registryKey || '';
    out.rendererBase = baseRendererFor(out.renderer);
    if (st.theme) {
      out.theme = st.theme.name || '';
      out.counts.blockStyles = Object.keys(st.theme.blockStyles || {}).length;
      out.counts.categoryStyles = Object.keys(st.theme.categoryStyles || {}).length;
      out.counts.componentStyles = Object.keys(st.theme.componentStyles || {}).length;
    }
    if (st.constants) out.counts.constants = Object.keys(st.constants).length;
    if (st.blockColours) out.counts.blockColours = Object.keys(st.blockColours).length;
    if (st.categoryIcons) out.counts.categoryIcons = Object.keys(st.categoryIcons).length;
    if (st.css) out.counts.cssBytes = (st.css.text || '').length;
    return out;
  }
  API.styleSummary = styleSummary;

  // Where the icons came from, in words, because "35 icons" does not say
  // whether they are the site's own or our stand-ins.
  function iconSourceLine(s) {
    var sum = s || styleSummary();
    var t = sum.iconTiers || {};
    var parts = [];
    if (t.live) parts.push(t.live + ' from the site');
    if (t.mirror) parts.push(t.mirror + ' from the mirror');
    if (t.fallback) parts.push(t.fallback + ' from the offline default');
    if (!parts.length) return 'no icons';
    return 'icons ' + parts.join(', ');
  }
  API.iconSourceLine = iconSourceLine;

  function styleSummaryLine(s) {
    var sum = s || styleSummary();
    return 'style ' + sum.source +
      ': renderer ' + (sum.renderer || 'not captured') +
      ' (drawn with ' + sum.rendererBase + ')' +
      ', theme ' + (sum.theme || 'not captured') +
      ', ' + sum.counts.constants + ' constants' +
      ', ' + sum.counts.blockStyles + ' block styles' +
      ', ' + sum.counts.categoryStyles + ' category styles' +
      ', ' + sum.counts.componentStyles + ' component colours' +
      ', ' + sum.counts.blockColours + ' typed colours' +
      ', ' + sum.counts.categoryIcons + ' icons' +
      ' (' + iconSourceLine(sum) + ')' +
      (sum.fonts ? ', ' + sum.fonts + ' mirrored font faces' : '') +
      ', ' + sum.counts.cssBytes + ' bytes of CSS' +
      (sum.missing.length ? '. Not captured: ' + sum.missing.join(', ') : '. Nothing missing');
  }
  API.styleSummaryLine = styleSummaryLine;

  // ---- the renderer -------------------------------------------------------
  // The site may register a renderer of its own. Ours cannot know that class,
  // so the shape is rebuilt instead: the closest base renderer draws, and every
  // captured measurement is forced onto its constant provider. That is what
  // makes the notches, the puzzle tabs, the corner radii and the hats line up
  // rather than merely look similar.
  var BASE_RENDERERS = ['geras', 'thrasos', 'zelos', 'minimalist'];

  function baseRendererFor(name) {
    var n = String(name || '').toLowerCase();
    for (var i = 0; i < BASE_RENDERERS.length; i++) {
      if (n === BASE_RENDERERS[i]) return BASE_RENDERERS[i];
    }
    // Named something else, or nothing at all: read the shape off the numbers.
    var c = (state.style && state.style.constants) || {};
    if (c.FULL_BLOCK_FIELDS === true || c.FIELD_COLOUR_FULL_BLOCK === true) return 'zelos';
    if (c.DARK_PATH_OFFSET !== undefined || c.MAX_BOTTOM_WIDTH !== undefined) return 'geras';
    if (n.indexOf('zelos') >= 0 || n.indexOf('scratch') >= 0) return 'zelos';
    if (n.indexOf('thrasos') >= 0) return 'thrasos';
    if (n.indexOf('minimal') >= 0) return 'minimalist';
    // Portal blocks are bevelled and carry the side notch and the puzzle tab of
    // the classic look, which in Blockly is geras. It is the honest default and
    // the status pill says so when nothing was captured.
    return 'geras';
  }
  API.baseRendererFor = baseRendererFor;

  // Every scalar the site reported, onto the provider. Objects (the derived
  // path data: NOTCH, PUZZLE_TAB, START_HAT, INSIDE_CORNERS, OUTSIDE_CORNERS)
  // go on afterwards, because init() builds those from the scalars and the
  // captured ones must win.
  // A CAPTURED OBJECT THAT WAS MOSTLY BEHAVIOUR IS AN EMPTY SHELL, AND MUST
  // NOT REPLACE THE LIVE ONE.
  //
  // The capture crosses from the site to here as JSON, and JSON carries no
  // functions. NOTCH and PUZZLE_TAB survive that trip intact because they are
  // numbers and path strings, which is the whole reason objects are copied at
  // all. The zelos DYNAMIC shapes do not: HEXAGONAL, ROUNDED and SQUARED are
  // almost entirely methods - width(), height(), pathDown(), pathUp() - and
  // they arrive as {"type":1,"isDynamic":true} with every one of them gone.
  //
  // Writing that over the provider's real shape leaves the renderer holding an
  // object it will call height() on, and the throw lands inside Blockly's own
  // flyout code where nothing reports it. The symptom is not an error message:
  // it is a toolbox category that does not open. Seen on 2026-09-06, and the
  // same trap is set for styleManager, which is all methods too.
  //
  // So the rule is about what SURVIVED the trip, not about a list of names: if
  // the provider already holds something with behaviour and the captured copy
  // has none, the live one stays.
  function isHollowCopy(live, captured) {
    if (captured && captured.isDynamic === true) return true;
    if (!live || typeof live !== 'object') return false;
    var liveFns = 0;
    for (var k in live) { try { if (typeof live[k] === 'function') liveFns++; } catch (e) {} }
    if (!liveFns) return false;
    for (var k2 in captured) { try { if (typeof captured[k2] === 'function') return false; } catch (e) {} }
    return true;
  }
  API.isHollowCopy = isHollowCopy;

  function assignConstants(cp, captured, withObjects) {
    if (!cp || !captured) return 0;
    var n = 0;
    Object.keys(captured).forEach(function (k) {
      var v = captured[k];
      if (isPlainScalar(v)) { cp[k] = v; n++; }
      else if (withObjects && v && typeof v === 'object') {
        if (isHollowCopy(cp[k], v)) return;
        var c = cloneJson(v);
        if (c !== null) { cp[k] = c; n++; }
      }
    });
    return n;
  }
  API.assignConstants = assignConstants;

  function fieldSpacingConstants(cp, compact) {
    var keys = ['FIELD_BORDER_RECT_HEIGHT', 'FIELD_DROPDOWN_BORDER_RECT_HEIGHT', 'FIELD_BORDER_RECT_Y_PADDING', 'DUMMY_INPUT_MIN_HEIGHT'];
    if (!cp.bf6FieldSpacingBase) {
      cp.bf6FieldSpacingBase = {};
      keys.forEach(function (k) { cp.bf6FieldSpacingBase[k] = cp[k]; });
    }
    var base = cp.bf6FieldSpacingBase;
    keys.forEach(function (k) { cp[k] = base[k]; });
    if (!compact) return;
    // Keep the captured font and text metrics. Only remove excess vertical
    // padding, with room around the text even for a different site capture.
    ['FIELD_BORDER_RECT_HEIGHT', 'FIELD_DROPDOWN_BORDER_RECT_HEIGHT'].forEach(function (k) {
      if (typeof base[k] === 'number') cp[k] = Math.min(base[k], Math.max(cp.FIELD_TEXT_HEIGHT + 7, base[k] - 6));
    });
    cp.FIELD_BORDER_RECT_Y_PADDING = Math.max(0, (cp.FIELD_BORDER_RECT_HEIGHT - cp.FIELD_TEXT_HEIGHT) / 2);
    if (typeof base.DUMMY_INPUT_MIN_HEIGHT === 'number')
      cp.DUMMY_INPUT_MIN_HEIGHT = Math.min(base.DUMMY_INPUT_MIN_HEIGHT, cp.FIELD_BORDER_RECT_HEIGHT);
  }
  API.applyFieldSpacing = function (ws, compact) {
    var spaces = [ws], flyout = ws.getFlyout && ws.getFlyout();
    if (flyout && flyout.getWorkspace()) spaces.push(flyout.getWorkspace());
    spaces.forEach(function (space) {
      fieldSpacingConstants(space.getRenderer().getConstants(), compact);
      space.getAllBlocks(false).forEach(function (block) {
        block.inputList.forEach(function (input) {
          input.fieldRow.forEach(function (field) {
            if (field.EDITABLE && field.forceRerender) {
              // A theme change can replace the renderer's provider while a
              // field still holds its previous provider. Refresh that cache.
              field.markDirty(); field.forceRerender();
            }
          });
        });
      });
    });
  };

  // A real subclass, so instanceof still holds and anything the base provider
  // does in its constructor still happens. Reflect.construct rather than
  // CP.call: Blockly 10 ships its classes as real ES6 classes, and one of those
  // refuses to be called without new.
  function styledConstantsClass(CP, captured) {
    function Styled() {
      var self = Reflect.construct(CP, [], Styled);
      assignConstants(self, captured, false);
      return self;
    }
    Styled.prototype = Object.create(CP.prototype);
    Styled.prototype.constructor = Styled;
    Styled.prototype.init = function () {
      assignConstants(this, captured, false);
      if (CP.prototype.init) CP.prototype.init.call(this);
      assignConstants(this, captured, true);
      fieldSpacingConstants(this, state.prefs && state.prefs.compactFields === true);
    };

    // THE SHAPE OF A SOCKET IS A METHOD, AND METHODS DO NOT CROSS.
    //
    // Everything else about the look travels as numbers. shapeFor does not: it
    // is code on the provider that reads a connection's type list and picks a
    // shape. So ours kept using Blockly's own answer, and Blockly's own answer
    // for anything that accepts a Boolean is a HEXAGON. The site has no
    // hexagons anywhere - asked directly, on the live page, with a real
    // connection of each of seven types, it returned ROUND every single time:
    //
    //   Boolean=ROUND Number=ROUND String=ROUND Player=ROUND
    //   Vector=ROUND  Array=ROUND  any=ROUND
    //
    // That is the diamond-shaped sockets in our editor against the rounded
    // pills on the site. The answer is applied from the capture rather than
    // hardcoded, so if the site ever starts distinguishing types again, the
    // next capture carries it and this follows.
    var rules = state.style && state.style.socketRules;
    var shapes = rules && rules.shapes;
    if (shapes) {
      var ids = {};
      Object.keys(shapes).forEach(function (k) {
        var t = shapes[k] && shapes[k].type;
        if (t !== undefined && t !== null) ids[t] = (ids[t] || 0) + 1;
      });
      var only = Object.keys(ids);
      if (only.length === 1) {
        var wantId = Number(only[0]);
        Styled.prototype.shapeFor = function (connection) {
          try {
            var t = connection && connection.type;
            if (t === B.INPUT_VALUE || t === B.OUTPUT_VALUE) {
              // Found by id, not by name: the provider's own field for that
              // shape is whatever it happens to be called on this build.
              var self = this;
              var hit = null;
              ['ROUNDED', 'HEXAGONAL', 'SQUARED'].forEach(function (n) {
                if (!hit && self[n] && self[n].type === wantId) hit = self[n];
              });
              if (hit) return hit;
            }
          } catch (e) {}
          return CP.prototype.shapeFor.call(this, connection);
        };
      }
    }
    Styled.bf6Captured = captured;
    return Styled;
  }
  API.styledConstantsClass = styledConstantsClass;

  function rendererClass(name) {
    if (!B || !B.registry || !name) return null;
    try {
      var Type = B.registry.Type && B.registry.Type.RENDERER ? B.registry.Type.RENDERER : 'renderer';
      // Asked first, because getClass on a name it does not have complains to
      // the console, and asking about the site's own renderer is normal here.
      if (B.registry.hasItem && !B.registry.hasItem(Type, name)) return null;
      return B.registry.getClass(Type, name) || null;
    } catch (e) { return null; }
  }
  API.rendererClass = rendererClass;

  function registerRenderer(name, klass) {
    if (!B || !B.registry) return false;
    var Type = (B.registry.Type && B.registry.Type.RENDERER) ? B.registry.Type.RENDERER : 'renderer';
    try { B.registry.register(Type, name, klass, true); return true; }
    catch (e) { /* fall through */ }
    try { B.blockRendering.register(name, klass); return true; } catch (e2) { return false; }
  }

  // Returns the renderer name to inject with, or null when nothing was
  // captured and the caller should keep its own default.
  function installStyledRenderer(regName) {
    if (!B) return null;
    var st = state.style;
    var captured = st && st.constants;
    var wanted = st && st.renderer && (st.renderer.name || st.renderer.registryKey);

    // The site's own renderer, already known here: nothing to rebuild.
    if (wanted && rendererClass(wanted) && !captured) {
      state.styleRenderer = { name: wanted, base: wanted, forced: 0, exact: true };
      return wanted;
    }
    if (!captured || !Object.keys(captured).length) {
      if (wanted && rendererClass(wanted)) {
        state.styleRenderer = { name: wanted, base: wanted, forced: 0, exact: true };
        return wanted;
      }
      return null;
    }

    var base = baseRendererFor(wanted);
    var BaseR = rendererClass(base);
    if (!BaseR) return null;
    var name = regName || 'bf6portal';
    var CPClass = null;

    function Styled(nm) { return Reflect.construct(BaseR, [nm], Styled); }
    Styled.prototype = Object.create(BaseR.prototype);
    Styled.prototype.constructor = Styled;
    Styled.prototype.makeConstants_ = function () {
      if (!CPClass) {
        var probe = BaseR.prototype.makeConstants_.call(this);
        CPClass = styledConstantsClass(probe.constructor, captured);
      }
      return new CPClass();
    };
    if (!registerRenderer(name, Styled)) return null;
    state.styleRenderer = {
      name: name, base: base, forced: Object.keys(captured).length,
      exact: !!(wanted && wanted === base)
    };
    return name;
  }
  API.installStyledRenderer = installStyledRenderer;

  // ---- the theme ----------------------------------------------------------
  // Handed out as a spec rather than a Theme, because defining a Theme
  // registers it under its name and a second definition under the same name is
  // refused. The caller passes a fresh name each time it applies.
  function themeSpec() {
    var t = state.style && state.style.theme;
    if (!t) return null;
    return {
      blockStyles: cloneJson(t.blockStyles) || {},
      categoryStyles: cloneJson(t.categoryStyles) || {},
      componentStyles: cloneJson(t.componentStyles) || {},
      fontStyle: cloneJson(t.fontStyle) || {},
      startHats: !!t.startHats
    };
  }
  API.themeSpec = themeSpec;

  // ---- per type colours ---------------------------------------------------
  // A block type that sets its colour in code rather than through a style is
  // invisible to the theme. The site was asked what colour each type actually
  // came out, and that answer is put back on the definition here, after the
  // definitions are installed and before any workspace is built.
  function paintBlock(block, want, known) {
    if (!block || !want) return false;
    var done = false;
    // The site names a style where it can, and a style carries the whole shade
    // set rather than one flat face. A per type colour that is really one of
    // the eleven named styles is put back through the style, so the block is
    // shaded the way the site shades it instead of being flooded.
    var styleName = want.style;
    if (!styleName && MUTATOR_STYLE[block.type]) styleName = MUTATOR_STYLE[block.type];
    if (styleName && known && known[styleName] && block.setStyle) {
      try { block.setStyle(styleName); done = true; } catch (e) { done = false; }
    }
    if (!done && want.colour !== undefined && want.colour !== null && block.setColour) {
      try { block.setColour(want.colour); done = true; } catch (e) {}
    }
    try {
      if (want.hat) block.hat = want.hat;
      if (want.outputShape !== undefined && want.outputShape !== null) {
        if (typeof block.setOutputShape === 'function') block.setOutputShape(want.outputShape);
        else block.outputShape_ = want.outputShape;
      }
    } catch (e) {}
    return done;
  }
  API.paintBlock = paintBlock;

  function applyBlockColours() {
    if (!B || !B.Blocks) return 0;
    var st = state.style;
    var map = st && st.blockColours;
    if (!map) return 0;
    var known = (st.theme && st.theme.blockStyles) || {};
    var n = 0;
    Object.keys(map).forEach(function (type) {
      var def = B.Blocks[type];
      if (!def) return;
      var want = map[type];
      if (!want || (!want.style && want.colour === undefined && !want.hat &&
        want.outputShape === undefined)) return;
      // Always re-wrap from the original, so applying twice does not nest.
      if (!def.bf6InitOriginal) def.bf6InitOriginal = def.init;
      var original = def.bf6InitOriginal;
      def.init = function () {
        if (original) original.call(this);
        paintBlock(this, want, known);
      };
      def.bf6Style = want;
      n++;
    });
    state.blockColoursApplied = n;
    return n;
  }
  API.applyBlockColours = applyBlockColours;

  // =========================================================================
  // Portal files
  //
  // Two real formats, both verified against real files rather than guessed:
  //
  //   A  WORKSPACE ONLY, what the site's blocks page exports and imports
  //      {"mod":{"blocks":{"languageVersion":0,"blocks":[...]},"variables":[...]}}
  //      verified against night_ops_breakthrough_workspace.json (5,088 blocks,
  //      104 variables). "variables" is absent when the project has none.
  //
  //   B  FULL EXPERIENCE, what the site itself imports and exports
  //      verified against BF6_SFX.json, a real published export:
  //        mutators          key -> a number, or [[team, value], ...] per team
  //        assetRestrictions key -> value, {} when nothing is restricted
  //        gameMode          "ModBuilderCustom"
  //        name, description strings
  //        mapRotation       [{id:"<MP_Map>-<GameMode><n>", spatialAttachment}]
  //        patchId           null
  //        workspace         the WHOLE format A document, mod wrapper included
  //        teamComposition   [[teamId, {humanCapacity}], ...], two entries
  //        attachments       every file, the spatial one included
  //
  //      An attachment is
  //        {id, version, filename, isProcessable, processingStatus,
  //         attachmentData:{original, compiled}, attachmentType, errors}
  //      with metadata added, before errors, on the spatial one. The copy
  //      inside mapRotation carries the same fields in a different order
  //      (id, filename, metadata, version, ...) and is otherwise identical to
  //      its entry in attachments: same id, same content.
  //
  //      attachmentData.original is base64 of the file's own UTF-8 text.
  //      compiled was "" on all four attachments of the sample; nothing the
  //      tool writes fills it.
  //
  //      attachmentType, from the sample and matching the kind numbering
  //      BF6PortalProfile.cpp already uses for the same four files:
  //        1  <name>.spatial.json     isProcessable true,  metadata "mapIdx=N"
  //        2  bundle.ts               isProcessable true
  //        3  blacklist.json          isProcessable true,  version ""
  //        4  bundle.strings.json     isProcessable FALSE
  //      processingStatus was 2 on all four; version is a STRING.
  //
  // Both files are written on one line, the way the site writes them.
  // =========================================================================

  var ATTACHMENT_TYPE = { SPATIAL: 1, TYPESCRIPT: 2, BLACKLIST: 3, STRINGS: 4 };
  API.ATTACHMENT_TYPE = ATTACHMENT_TYPE;

  var ATTACHMENT_KIND = { 1: 'spatial', 2: 'typescript', 3: 'blacklist', 4: 'strings' };
  API.ATTACHMENT_KIND = ATTACHMENT_KIND;

  // The site's own defaults, for a field the tool cannot fill honestly.
  var EXPERIENCE_DEFAULTS = {
    gameMode: 'ModBuilderCustom',
    patchId: null,
    teamComposition: [[1, { humanCapacity: 0 }], [2, { humanCapacity: 0 }]]
  };
  API.EXPERIENCE_DEFAULTS = EXPERIENCE_DEFAULTS;

  function b64encode(text) {
    var s = String(text === undefined || text === null ? '' : text);
    if (typeof Buffer !== 'undefined' && Buffer.from) return Buffer.from(s, 'utf8').toString('base64');
    var bytes = new TextEncoder().encode(s);
    var bin = '';
    for (var i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(bin);
  }
  API.b64encode = b64encode;

  function b64decode(b64) {
    var s = String(b64 || '');
    if (!s) return '';
    if (typeof Buffer !== 'undefined' && Buffer.from) return Buffer.from(s, 'base64').toString('utf8');
    var bin = atob(s);
    var bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new TextDecoder('utf-8').decode(bytes);
  }
  API.b64decode = b64decode;

  // The site's id format: a lowercase v4 uuid, 8-4-4-4-12.
  function uuid() {
    var b = new Array(16), i;
    var c = (typeof crypto !== 'undefined' && crypto) ? crypto :
      (typeof globalThis !== 'undefined' && globalThis.crypto) ? globalThis.crypto : null;
    if (c && c.getRandomValues) {
      var a = new Uint8Array(16);
      c.getRandomValues(a);
      for (i = 0; i < 16; i++) b[i] = a[i];
    } else {
      for (i = 0; i < 16; i++) b[i] = Math.floor(Math.random() * 256);
    }
    b[6] = (b[6] & 0x0f) | 0x40;          // version 4
    b[8] = (b[8] & 0x3f) | 0x80;          // variant 1
    var hex = '';
    for (i = 0; i < 16; i++) hex += (b[i] < 16 ? '0' : '') + b[i].toString(16);
    return hex.slice(0, 8) + '-' + hex.slice(8, 12) + '-' + hex.slice(12, 16) + '-' +
      hex.slice(16, 20) + '-' + hex.slice(20);
  }
  API.uuid = uuid;

  function attachmentTypeFor(filename) {
    var f = String(filename || '').toLowerCase();
    if (/\.spatial\.json$/.test(f)) return ATTACHMENT_TYPE.SPATIAL;
    if (/\.ts$/.test(f)) return ATTACHMENT_TYPE.TYPESCRIPT;
    if (/blacklist\.json$/.test(f)) return ATTACHMENT_TYPE.BLACKLIST;
    if (/strings.*\.json$/.test(f) || /\.strings\.json$/.test(f)) return ATTACHMENT_TYPE.STRINGS;
    return 0;
  }
  API.attachmentTypeFor = attachmentTypeFor;

  // ---- what is this file? -------------------------------------------------
  // Read off the content, never off the extension: the site hands out .json
  // files of three different shapes and people rename all of them.
  function looksLikeTypeScript(text) {
    var t = String(text || '');
    if (!t) return false;
    if (/(^|\n)\s*(import|export)\s/.test(t)) return true;
    if (/(^|\n)\s*function\s+[A-Za-z_$]/.test(t) && /\bmod\./.test(t)) return true;
    if (/\bmodlib\b/.test(t)) return true;
    return false;
  }

  function classifyJson(doc) {
    if (!doc || typeof doc !== 'object' || Array.isArray(doc)) return 'unknown';
    if (doc.workspace && typeof doc.workspace === 'object' &&
      (doc.mapRotation !== undefined || doc.attachments !== undefined ||
        doc.gameMode !== undefined || doc.mutators !== undefined)) return 'experience';
    if (doc.mod && typeof doc.mod === 'object' && doc.mod.blocks) return 'workspace';
    if (doc.blocks && typeof doc.blocks === 'object' && Array.isArray(doc.blocks.blocks)) return 'workspace';
    if (Array.isArray(doc.Portal_Dynamic) || Array.isArray(doc.Portal_Static)) return 'spatial';
    var keys = Object.keys(doc);
    if (keys.length) {
      var allText = keys.every(function (k) {
        var v = doc[k];
        return typeof v === 'string' || (v && typeof v === 'object' && !Array.isArray(v));
      });
      if (allText) return 'strings';
    }
    return 'unknown';
  }

  // Returns {format, doc, text, looksLike}. format is one of
  // workspace | experience | typescript | strings | spatial | unknown.
  function detectFormat(input) {
    var text = null, doc = null;
    if (typeof input === 'string') {
      text = input;
      var t = text.replace(/^﻿/, '');
      var head = t.replace(/^\s+/, '').charAt(0);
      if (head === '{' || head === '[') {
        try { doc = JSON.parse(t); } catch (e) { doc = null; }
      }
    } else if (input && typeof input === 'object') {
      doc = input;
    }
    // Local recovery files carry their project identity beside the complete
    // workspace. Opening one must work through the normal Import button.
    if (doc && ['autosave', 'projectRecovery', 'updateWorkspace'].indexOf(doc.op) >= 0 &&
        doc.json && classifyJson(doc.json) === 'workspace') doc = doc.json;
    var format;
    if (doc) format = classifyJson(doc);
    else if (looksLikeTypeScript(text)) format = 'typescript';
    else format = 'unknown';

    var looksLike;
    switch (format) {
      case 'workspace': looksLike = 'a Portal workspace: blocks and variables, no experience around them'; break;
      case 'experience': looksLike = 'a full Portal experience: a workspace, a map rotation and its attachments'; break;
      case 'typescript': looksLike = 'a Portal script export: TypeScript the site wrote from blocks'; break;
      case 'strings': looksLike = 'a Portal strings bundle: keys and the text behind them, no blocks'; break;
      case 'spatial': looksLike = 'a Portal spatial export: placed objects, no blocks'; break;
      default:
        if (doc) looksLike = 'JSON, but not a shape Portal uses: top level keys ' +
          Object.keys(doc).slice(0, 8).join(', ');
        else if (text) looksLike = 'text that is neither JSON nor TypeScript, starting "' +
          String(text).replace(/\s+/g, ' ').slice(0, 60) + '"';
        else looksLike = 'nothing readable';
    }
    return { format: format, doc: doc, text: text, looksLike: looksLike };
  }
  API.detectFormat = detectFormat;

  // ---- format B, read ------------------------------------------------------
  // "MP_Portal_Sand-ModBuilderCustom0" is the map, the game mode and the slot
  // index run together, so it is taken apart from the right.
  function parseRotationId(id) {
    var s = String(id || '');
    var cut = s.lastIndexOf('-');
    if (cut < 0) return { map: s, gameMode: '', index: 0 };
    var map = s.slice(0, cut);
    var rest = s.slice(cut + 1);
    var m = /^(.*?)(\d+)$/.exec(rest);
    return {
      map: map,
      gameMode: m ? m[1] : rest,
      index: m ? parseInt(m[2], 10) : 0
    };
  }
  API.parseRotationId = parseRotationId;

  function readAttachment(a) {
    if (!a || typeof a !== 'object') return null;
    var data = a.attachmentData || {};
    var type = (a.attachmentType !== undefined && a.attachmentType !== null)
      ? a.attachmentType : attachmentTypeFor(a.filename);
    return {
      id: a.id || '',
      filename: a.filename || '',
      attachmentType: type,
      kind: ATTACHMENT_KIND[type] || 'unknown',
      version: a.version === undefined || a.version === null ? '' : String(a.version),
      metadata: a.metadata === undefined ? null : a.metadata,
      isProcessable: !!a.isProcessable,
      processingStatus: a.processingStatus === undefined ? 2 : a.processingStatus,
      errors: a.errors || [],
      bytes: (data.original || '').length,
      text: b64decode(data.original || ''),
      compiled: data.compiled || ''
    };
  }

  function readExperience(doc) {
    if (!doc || typeof doc !== 'object') return null;
    var atts = (doc.attachments || []).map(readAttachment).filter(Boolean);
    var rot = (doc.mapRotation || []).map(function (r, i) {
      var parsed = parseRotationId(r && r.id);
      var sp = (r && r.spatialAttachment) || null;
      return {
        id: (r && r.id) || '',
        map: parsed.map,
        gameMode: parsed.gameMode,
        index: parsed.index >= 0 ? parsed.index : i,
        spatialId: sp ? (sp.id || '') : '',
        spatialFilename: sp ? (sp.filename || '') : '',
        metadata: sp ? (sp.metadata || '') : ''
      };
    });
    return {
      workspace: doc.workspace || { mod: { blocks: { languageVersion: 0, blocks: [] } } },
      attachments: atts,
      mapRotation: rot,
      // Everything that is not the workspace: kept whole so an export can put
      // the experience back exactly as it came, with only the blocks changed.
      shell: {
        mutators: cloneJson(doc.mutators) || {},
        assetRestrictions: cloneJson(doc.assetRestrictions) || {},
        gameMode: doc.gameMode || EXPERIENCE_DEFAULTS.gameMode,
        name: doc.name || '',
        description: doc.description || '',
        mapRotation: cloneJson(doc.mapRotation) || [],
        patchId: doc.patchId === undefined ? null : doc.patchId,
        teamComposition: cloneJson(doc.teamComposition) || cloneJson(EXPERIENCE_DEFAULTS.teamComposition),
        attachments: cloneJson(doc.attachments) || []
      }
    };
  }
  API.readExperience = readExperience;

  // ---- format B, write -----------------------------------------------------
  // Key order matters only to a human reading the file, but it is the site's
  // order and there is no reason to differ from it.
  function makeAttachment(spec) {
    var type = spec.attachmentType || attachmentTypeFor(spec.filename);
    var a = {};
    a.id = spec.id || uuid();
    a.version = spec.version === undefined || spec.version === null ? '' : String(spec.version);
    a.filename = spec.filename || '';
    a.isProcessable = spec.isProcessable !== undefined ? !!spec.isProcessable
      : type !== ATTACHMENT_TYPE.STRINGS;      // the sample: only the strings file is false
    a.processingStatus = spec.processingStatus === undefined ? 2 : spec.processingStatus;
    a.attachmentData = {
      original: spec.original !== undefined ? spec.original : b64encode(spec.text || ''),
      compiled: spec.compiled || ''
    };
    a.attachmentType = type;
    if (spec.metadata !== undefined && spec.metadata !== null) a.metadata = spec.metadata;
    a.errors = spec.errors || [];
    return a;
  }
  API.makeAttachment = makeAttachment;

  // The same attachment as the map rotation carries it: identical content, the
  // site's other key order.
  function rotationCopyOf(a) {
    var o = {};
    o.id = a.id;
    o.filename = a.filename;
    if (a.metadata !== undefined) o.metadata = a.metadata;
    o.version = a.version;
    o.isProcessable = a.isProcessable;
    o.processingStatus = a.processingStatus;
    o.attachmentData = { original: a.attachmentData.original, compiled: a.attachmentData.compiled };
    o.attachmentType = a.attachmentType;
    o.errors = a.errors || [];
    return o;
  }
  API.rotationCopyOf = rotationCopyOf;

  // opts:
  //   workspace        the format A document to put in (required)
  //   shell            an imported experience's non-workspace fields, if any
  //   name/description/gameMode/mutators/assetRestrictions/teamComposition
  //                    override the shell field by field
  //   maps             [{map, index, spatialText, spatialFilename}] to rebuild
  //                    the rotation from; omitted leaves the shell's rotation
  //   files            [{filename, text, attachmentType, version}] extra
  //                    attachments: the script bundle, its strings, a blacklist
  //   freshIds         true (the default) gives every attachment a new uuid
  //
  // Returns {doc, summary}. summary.sources says where every field came from
  // and summary.defaulted lists the ones that are the site's default because
  // nothing honest was available.
  function writeExperience(opts) {
    opts = opts || {};
    var shell = opts.shell || {};
    var sources = {};
    var defaulted = [];
    var fresh = opts.freshIds !== false;

    function pick(field, given, shellValue, fallback) {
      if (given !== undefined && given !== null) { sources[field] = 'the tool'; return given; }
      if (shellValue !== undefined && shellValue !== null) { sources[field] = 'the imported experience'; return shellValue; }
      sources[field] = 'the site default';
      defaulted.push(field);
      return fallback;
    }

    var doc = {};
    doc.mutators = pick('mutators', opts.mutators, shell.mutators, {});
    doc.assetRestrictions = pick('assetRestrictions', opts.assetRestrictions, shell.assetRestrictions, {});
    doc.gameMode = pick('gameMode', opts.gameMode, shell.gameMode, EXPERIENCE_DEFAULTS.gameMode);
    doc.name = pick('name', opts.name, shell.name, '');
    doc.description = pick('description', opts.description, shell.description, '');

    // The attachments come first, because the rotation carries copies of them.
    var attachments = [];
    var bySlot = {};

    if (opts.maps && opts.maps.length) {
      sources.mapRotation = 'the tool';
      opts.maps.forEach(function (m, i) {
        var idx = (m.index === undefined || m.index === null) ? i : m.index;
        if (m.spatialText === undefined || m.spatialText === null) { bySlot[idx] = null; return; }
        var a = makeAttachment({
          filename: m.spatialFilename || ((m.map || 'map') + '.spatial.json'),
          text: m.spatialText,
          attachmentType: ATTACHMENT_TYPE.SPATIAL,
          version: m.version === undefined ? '' : m.version,
          metadata: 'mapIdx=' + idx,
          isProcessable: true
        });
        attachments.push(a);
        bySlot[idx] = a;
      });
      doc.mapRotation = opts.maps.map(function (m, i) {
        var idx = (m.index === undefined || m.index === null) ? i : m.index;
        var row = { id: (m.map || '') + '-' + doc.gameMode + idx };
        if (bySlot[idx]) row.spatialAttachment = rotationCopyOf(bySlot[idx]);
        return row;
      });
    } else if (shell.mapRotation && shell.mapRotation.length) {
      sources.mapRotation = 'the imported experience';
      doc.mapRotation = cloneJson(shell.mapRotation) || [];
      // Its spatial attachments belong in the attachments array too, and both
      // copies must keep the same id.
      doc.mapRotation.forEach(function (row) {
        if (!row || !row.spatialAttachment) return;
        if (fresh) row.spatialAttachment.id = uuid();
        var a = makeAttachment({
          id: row.spatialAttachment.id,
          filename: row.spatialAttachment.filename,
          original: (row.spatialAttachment.attachmentData || {}).original,
          compiled: (row.spatialAttachment.attachmentData || {}).compiled,
          attachmentType: row.spatialAttachment.attachmentType || ATTACHMENT_TYPE.SPATIAL,
          version: row.spatialAttachment.version,
          metadata: row.spatialAttachment.metadata,
          isProcessable: row.spatialAttachment.isProcessable,
          processingStatus: row.spatialAttachment.processingStatus,
          errors: row.spatialAttachment.errors
        });
        attachments.push(a);
      });
    } else {
      sources.mapRotation = 'the site default';
      defaulted.push('mapRotation');
      doc.mapRotation = [];
    }

    doc.patchId = opts.patchId === undefined
      ? (shell.patchId === undefined ? EXPERIENCE_DEFAULTS.patchId : shell.patchId)
      : opts.patchId;
    sources.patchId = 'the site default';

    // The workspace is the one field that is always the tool's own.
    var ws = opts.workspace || { mod: { blocks: { languageVersion: 0, blocks: [] } } };
    doc.workspace = ws.mod ? cloneJson(ws) : { mod: cloneJson(ws) };
    sources.workspace = 'the editor';

    doc.teamComposition = pick('teamComposition', opts.teamComposition, shell.teamComposition,
      cloneJson(EXPERIENCE_DEFAULTS.teamComposition));

    // Everything the imported experience carried that is not spatial: the
    // script bundle, its strings, the blacklist. Replaced file by file where
    // the tool has something newer.
    var replaced = {};
    (opts.files || []).forEach(function (f) {
      if (!f || !f.filename) return;
      replaced[f.filename] = true;
      attachments.push(makeAttachment({
        filename: f.filename, text: f.text,
        attachmentType: f.attachmentType || attachmentTypeFor(f.filename),
        version: f.version, isProcessable: f.isProcessable,
        metadata: f.metadata
      }));
    });
    (shell.attachments || []).forEach(function (a) {
      if (!a || !a.filename) return;
      if (replaced[a.filename]) return;
      if ((a.attachmentType || attachmentTypeFor(a.filename)) === ATTACHMENT_TYPE.SPATIAL) return;
      var kept = makeAttachment({
        id: fresh ? uuid() : a.id,
        filename: a.filename,
        original: (a.attachmentData || {}).original,
        compiled: (a.attachmentData || {}).compiled,
        attachmentType: a.attachmentType,
        version: a.version,
        metadata: a.metadata,
        isProcessable: a.isProcessable,
        processingStatus: a.processingStatus,
        errors: a.errors
      });
      attachments.push(kept);
    });
    doc.attachments = attachments;

    return {
      doc: doc,
      summary: {
        sources: sources,
        defaulted: defaulted,
        attachments: attachments.map(function (a) {
          return { filename: a.filename, attachmentType: a.attachmentType,
            kind: ATTACHMENT_KIND[a.attachmentType] || 'unknown',
            bytes: (a.attachmentData.original || '').length };
        }),
        maps: doc.mapRotation.map(function (r) { return r.id; })
      }
    };
  }
  API.writeExperience = writeExperience;

  // Format A, exactly as the site writes it: the mod wrapper, and variables
  // only when there are any.
  function writeWorkspaceDoc(saved) {
    var st = unwrap(saved);
    var mod = { blocks: st.blocks || { languageVersion: 0, blocks: [] } };
    if (st.variables && st.variables.length) mod.variables = st.variables;
    return { mod: mod };
  }
  API.writeWorkspaceDoc = writeWorkspaceDoc;

  return API;
});
