#!/usr/bin/env node
/*
 * BF6Convert command line.
 *
 *   node cli.js blocks2ts <workspace.json> <outDir>
 *   node cli.js ts2blocks <srcDir | file.ts> <out.json>
 *
 * Both print a report. Exit code 0 on success, 1 on failure.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var BF6Convert = require('./convert.js');

/* The TypeScript compiler API is only needed for ts2blocks. Look for it next to
 * this file, then in a Portal project given by BF6_TS or BF6_TEMPLATE. */
(function loadTypeScript() {
    var candidates = [];
    if (process.env.BF6_TS) { candidates.push(process.env.BF6_TS); }
    if (process.env.BF6_TEMPLATE) {
        candidates.push(path.join(process.env.BF6_TEMPLATE, 'node_modules', 'typescript'));
    }
    candidates.push('typescript');
    candidates.push(path.join(process.cwd(), 'node_modules', 'typescript'));
    candidates.push('C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\_template-v1.7.0\\node_modules\\typescript');
    for (var i = 0; i < candidates.length; i++) {
        try {
            BF6Convert.setTypeScript(require(candidates[i]));
            return;
        } catch (e) { /* next */ }
    }
})();

function fail(msg) {
    process.stderr.write('error: ' + msg + '\n');
    process.exit(1);
}

function ensureDir(dir) {
    if (!fs.existsSync(dir)) { fs.mkdirSync(dir, { recursive: true }); }
}

function collectTs(target) {
    var sources = {};
    var stat = fs.statSync(target);
    if (stat.isFile()) {
        sources[path.basename(target)] = fs.readFileSync(target, 'utf8');
        return sources;
    }
    var stack = [target];
    while (stack.length) {
        var dir = stack.pop();
        var entries = fs.readdirSync(dir, { withFileTypes: true });
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i];
            var full = path.join(dir, e.name);
            if (e.isDirectory()) {
                if (e.name === 'node_modules' || e.name === 'dist' || e.name.charAt(0) === '.') { continue; }
                stack.push(full);
            } else if (/\.ts$/.test(e.name) && !/\.d\.ts$/.test(e.name)) {
                sources[path.relative(target, full).replace(/\\/g, '/')] = fs.readFileSync(full, 'utf8');
            }
        }
    }
    return sources;
}

function printReport(report) {
    var out = [];
    out.push('');
    out.push('Report (' + report.kind + ')');
    var k;
    for (k in report.counts) {
        if (Object.prototype.hasOwnProperty.call(report.counts, k)) {
            out.push('  ' + k + ': ' + report.counts[k]);
        }
    }
    if (report.groups) {
        var g, parts = [];
        for (g in report.groups) {
            if (Object.prototype.hasOwnProperty.call(report.groups, g)) {
                parts.push(g + '=' + report.groups[g]);
            }
        }
        out.push('  groups: ' + parts.join(', '));
    }
    if (report.warnings.length) {
        out.push('  warnings: ' + report.warnings.length);
        for (var i = 0; i < Math.min(report.warnings.length, 20); i++) {
            out.push('    - ' + report.warnings[i]);
        }
    }
    out.push('  unconvertible: ' + report.unconvertible.length);
    var shown = {}, count = 0;
    for (var j = 0; j < report.unconvertible.length; j++) {
        var u = report.unconvertible[j];
        var line = '    ' + u.file + ':' + u.line + '  ' + u.construct;
        if (shown[line]) { continue; }
        shown[line] = 1;
        out.push(line);
        out.push('      suggestion: ' + u.suggestion);
        count++;
        if (count >= 40) {
            out.push('    ... ' + (report.unconvertible.length - j - 1) + ' more');
            break;
        }
    }
    process.stdout.write(out.join('\n') + '\n');
}

function main(argv) {
    var cmd = argv[2];
    if (!cmd || cmd === '-h' || cmd === '--help') {
        process.stdout.write([
            'BF6Convert ' + BF6Convert.VERSION,
            '',
            '  node cli.js blocks2ts <workspace.json> <outDir>',
            '  node cli.js ts2blocks <srcDir | file.ts> <out.json>',
            '  node cli.js blocks2template <workspace.json> <projectDir>',
            ''
        ].join('\n'));
        return 0;
    }

    if (cmd === 'blocks2ts') {
        var inFile = argv[3], outDir = argv[4];
        if (!inFile || !outDir) { fail('usage: node cli.js blocks2ts <workspace.json> <outDir>'); }
        var ws = JSON.parse(fs.readFileSync(inFile, 'utf8'));
        var r = BF6Convert.blocksToTs(ws, {});
        ensureDir(outDir);
        var names = Object.keys(r.files);
        names.sort();
        for (var i = 0; i < names.length; i++) {
            fs.writeFileSync(path.join(outDir, names[i]), r.files[names[i]], 'utf8');
            process.stdout.write('wrote ' + path.join(outDir, names[i]) + '\n');
        }
        printReport(r.report);
        return 0;
    }

    /* BLOCKS INTO AN ALREADY SCAFFOLDED TEMPLATE PROJECT.
     *
     * blocks2ts writes a bare pile of .ts into a folder of its own, which is
     * the right shape for inspecting a conversion and the wrong shape for
     * working on one. Plenty of creators never touch the TypeScript template,
     * so their experience arrives as a block workspace and nothing else; this
     * lands that workspace in <projectDir>/src so the template's bundler,
     * strict tsc and utility modules apply to it like any other project.
     *
     * It writes ONLY the files the conversion produced. The template's own
     * src/debug-tool and src/helpers are left alone, so the features that come
     * with the template are still there to import.
     *
     * A replaced file is kept beside itself as <name>.orig, so a second import
     * can be compared against the first rather than silently overwriting it.
     *
     * The last line of stdout is a JSON summary, for the tool to report.
     */
    if (cmd === 'blocks2template') {
        var wsFile = argv[3], projectDir = argv[4];
        if (!wsFile || !projectDir) {
            fail('usage: node cli.js blocks2template <workspace.json> <projectDir>');
        }
        var wsDoc = JSON.parse(fs.readFileSync(wsFile, 'utf8'));
        var res2 = BF6Convert.blocksToTs(wsDoc, {});
        var srcDir = path.join(projectDir, 'src');

        /* AN IMPORT THAT CANNOT EAT SOMEBODY'S WORK.
         *
         * The first version copied each destination to .orig in a best-effort
         * try/catch and then wrote over it regardless. Three ways that loses
         * work, all of them reproducible:
         *
         *   - the .orig copy fails and the overwrite happens anyway;
         *   - importing a second time replaces .orig with the FIRST import's
         *     generated file, so the handwritten original is gone for good;
         *   - a write failing halfway leaves a half-imported project and still
         *     reports success.
         *
         * So: what this tool generated is recorded, with a hash. A destination
         * whose content still matches what we generated is OURS and is replaced
         * freely. Anything else is the author's, gets a .orig that is never
         * overwritten once it exists, and if that backup cannot be made the
         * import stops before touching anything. Everything is staged and only
         * committed once every file is written.
         */
        var MANIFEST = path.join(projectDir, '.bf6-generated.json');
        var prior = {};
        try { prior = JSON.parse(fs.readFileSync(MANIFEST, 'utf8')).files || {}; } catch (e) { prior = {}; }

        function hash(text) {
            return require('crypto').createHash('sha256').update(text, 'utf8').digest('hex');
        }

        var outNames = Object.keys(res2.files);
        outNames.sort();

        // ---- 1. decide, touching nothing -------------------------------
        var plan = [], blocked = [];
        for (var k = 0; k < outNames.length; k++) {
            var rel = 'src/' + outNames[k];
            var dest = path.join(srcDir, outNames[k]);
            var step = { rel: rel, dest: dest, text: res2.files[outNames[k]], backup: null };
            if (fs.existsSync(dest)) {
                var current = '';
                try { current = fs.readFileSync(dest, 'utf8'); }
                catch (e) {
                    blocked.push(rel + ' could not be read, so it is not safe to replace');
                    continue;
                }
                var wasOurs = prior[rel] && prior[rel] === hash(current);
                if (!wasOurs) {
                    // The author's file. Keep the FIRST .orig forever: a second
                    // import must not bury the handwritten original under a
                    // generated one.
                    var orig = dest + '.orig';
                    var have = null;
                    try { have = fs.statSync(orig); } catch (e) { have = null; }
                    if (!have) {
                        step.backup = orig;
                    } else if (!have.isFile()) {
                        // Something that is not a file is sitting where the
                        // backup goes, so a backup cannot be made here. Treating
                        // "it exists" as "it is backed up" is how the overwrite
                        // went ahead with nothing kept.
                        blocked.push(rel + ' cannot be backed up: ' + orig + ' is not a file');
                        continue;
                    } else {
                        // AN EXISTING .orig IS NOT PROOF THIS VERSION IS SAFE.
                        //
                        // "First .orig wins" stopped the second import burying
                        // the handwritten original, and then went too far: a
                        // hand edit made AFTER an import has an .orig already,
                        // so it was replaced with nothing kept. Reproduced -
                        // the edit was in no file in the project afterwards.
                        //
                        // Every DISTINCT authored version is kept. If the .orig
                        // already holds exactly this text there is nothing new
                        // to save; otherwise it goes to its own numbered
                        // snapshot beside it, and nothing is ever overwritten.
                        var already = '';
                        try { already = fs.readFileSync(orig, 'utf8'); } catch (e) { already = null; }
                        if (already === null) {
                            blocked.push(rel + ' cannot be replaced: ' + orig + ' could not be read');
                            continue;
                        }
                        if (already !== current) {
                            var slot = 2, extra = '';
                            for (; slot < 100; slot++) {
                                extra = dest + '.orig' + slot;
                                var taken = null;
                                try { taken = fs.readFileSync(extra, 'utf8'); } catch (e) { taken = null; }
                                if (taken === null) { break; }          // free
                                if (taken === current) { extra = ''; break; }   // already kept
                            }
                            if (slot >= 100) {
                                blocked.push(rel + ' has too many kept versions beside it; move some aside first');
                                continue;
                            }
                            if (extra) { step.backup = extra; }
                        }
                    }
                }
            }
            plan.push(step);
        }
        if (blocked.length) {
            fail('nothing was imported:\n  ' + blocked.join('\n  '));
        }

        // ---- 2. back up, failing closed --------------------------------
        for (var b = 0; b < plan.length; b++) {
            if (!plan[b].backup) { continue; }
            try { fs.copyFileSync(plan[b].dest, plan[b].backup); }
            catch (e) {
                fail('nothing was imported: could not keep a copy of ' + plan[b].rel
                     + ' (' + e.message + '). Your file is untouched.');
            }
        }

        // ---- 3. stage every file beside its destination ----------------
        ensureDir(srcDir);
        var staged = [];
        try {
            for (var t = 0; t < plan.length; t++) {
                ensureDir(path.dirname(plan[t].dest));
                var tmp = plan[t].dest + '.bf6new';
                fs.writeFileSync(tmp, plan[t].text, 'utf8');
                staged.push({ tmp: tmp, step: plan[t] });
            }
        } catch (e) {
            staged.forEach(function (x) { try { fs.unlinkSync(x.tmp); } catch (e2) {} });
            fail('nothing was imported: ' + e.message + '. Your files are untouched.');
        }

        // ---- 4. commit -------------------------------------------------
        var written = [], generated = {};
        for (var c = 0; c < staged.length; c++) {
            try {
                fs.renameSync(staged[c].tmp, staged[c].step.dest);
                written.push(staged[c].step.rel);
                generated[staged[c].step.rel] = hash(staged[c].step.text);
            } catch (e) {
                // A rename failing this late is rare and cannot be undone for
                // the ones already committed, so it is REPORTED rather than
                // hidden behind a success line.
                staged.slice(c).forEach(function (x) { try { fs.unlinkSync(x.tmp); } catch (e2) {} });
                process.stdout.write('BF6CONVERT ' + JSON.stringify({
                    ok: false,
                    why: 'the import stopped part way at ' + staged[c].step.rel + ': ' + e.message,
                    files: written
                }) + '\n');
                return 1;
            }
        }
        try {
            fs.writeFileSync(MANIFEST, JSON.stringify({ files: generated }, null, 4) + '\n', 'utf8');
        } catch (e) { /* the import stands; the next one is just more cautious */ }

        printReport(res2.report);
        // The counts live under report.counts, not on the report itself, which
        // is why this summary first reported a 43 rule conversion as 0 rules.
        var rep = res2.report || {};
        var counts = rep.counts || {};
        process.stdout.write('BF6CONVERT ' + JSON.stringify({
            ok: true,
            rules: (counts.rules == null ? 0 : counts.rules),
            subroutines: (counts.subroutines == null ? 0 : counts.subroutines),
            variables: (counts.variables == null ? 0 : counts.variables),
            unconvertible: (rep.unconvertible ? rep.unconvertible.length : 0),
            warnings: (rep.warnings ? rep.warnings.length : 0),
            kept: plan.filter(function (x) { return x.backup; }).map(function (x) { return x.rel; }),
            files: written
        }) + '\n');
        return 0;
    }

    if (cmd === 'ts2blocks') {
        var target = argv[3], outFile = argv[4];
        if (!target || !outFile) { fail('usage: node cli.js ts2blocks <srcDir | file.ts> <out.json>'); }
        var sources = collectTs(target);
        var res = BF6Convert.tsToBlocks(sources, {});
        ensureDir(path.dirname(path.resolve(outFile)));
        fs.writeFileSync(outFile, JSON.stringify(res.workspace, null, 2) + '\n', 'utf8');
        process.stdout.write('wrote ' + outFile + '\n');
        printReport(res.report);
        return 0;
    }

    fail('unknown command "' + cmd + '"');
    return 1;
}

process.exit(main(process.argv));
