'use strict';
// The same pipeline is used by the Unreal button and the command-line tests.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');
const TEMPLATE = path.join(__dirname, 'template');
const digest = value => crypto.createHash('sha256').update(value).digest('hex');

function sourceFiles(input) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) throw Error('No generated source files were supplied.');
  const entries = Object.entries(input), seen = new Set(), files = {};
  if (!entries.length || entries.length > 512) throw Error('Export must contain between 1 and 512 source files.');
  let bytes = 0;
  for (let [name, body] of entries) {
    name = name.replace(/\\/g, '/');
    if (!name || /[:\x00-\x1f]/.test(name) || name.startsWith('/') || name.split('/').some(s => !s || s === '.' || s === '..'))
      throw Error('Invalid generated file path: ' + name);
    if (typeof body !== 'string' || !/\.(ts|json)$/.test(name)) throw Error('Invalid generated file: ' + name);
    // The native converter emits flat modules; extended compilation emits src/.
    if (!name.includes('/')) name = 'src/' + name;
    if (!name.startsWith('src/') && name !== 'blocks/workspace.json' && name !== 'logic/source-map.json')
      throw Error('Unexpected generated path: ' + name);
    const key = name.toLowerCase();
    if (seen.has(key)) throw Error('Duplicate generated path: ' + name);
    seen.add(key); bytes += Buffer.byteLength(body);
    if (bytes > 32 * 1024 * 1024) throw Error('Generated source exceeds 32 MB.');
    files[name] = body;
  }
  if (!files['src/index.ts']) throw Error('The compiler did not generate src/index.ts.');
  const stringKeys = new Map();
  for (const [name, body] of Object.entries(files)) if (name.startsWith('src/') && name.endsWith('strings.json')) {
    let strings;
    try { strings = JSON.parse(body); } catch { throw Error('Invalid strings JSON: ' + name); }
    if (!strings || typeof strings !== 'object' || Array.isArray(strings)) throw Error('Strings must be a JSON object: ' + name);
    for (const [key,value] of Object.entries(strings)) {
      const prior = stringKeys.get(key), encoded = JSON.stringify(value);
      if (prior && prior.value !== encoded) throw Error('Conflicting string key "' + key + '" in ' + prior.file + ' and ' + name + '. Rename the key or give it the same text before exporting.');
      stringKeys.set(key,{file:name,value:encoded});
    }
  }
  return files;
}

function run(args, cwd) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, args, { cwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    let tail = '';
    for (const stream of [child.stdout, child.stderr]) stream.on('data', bytes => {
      const text = bytes.toString(); tail = (tail + text).slice(-12000); process.stdout.write(text);
    });
    child.on('error', reject);
    child.on('close', code => code === 0 ? resolve() : reject(Error('Build stage failed (' + code + '):\n' + tail)));
  });
}

async function dependencies(cache, npmCli) {
  const lock = fs.readFileSync(path.join(TEMPLATE, 'package-lock.json'));
  const dir = path.join(cache, digest(lock).slice(0, 20));
  fs.mkdirSync(dir, { recursive: true });
  const ready = path.join(dir, '.ready');
  const expected = JSON.parse(fs.readFileSync(path.join(TEMPLATE, 'package.json'))).devDependencies;
  const valid = () => fs.existsSync(ready) && Object.entries(expected).every(([name, version]) => {
    try { return JSON.parse(fs.readFileSync(path.join(dir, 'node_modules', name, 'package.json'))).version === version; }
    catch { return false; }
  });
  if (valid()) return dir;
  if (!npmCli || !fs.existsSync(npmCli)) throw Error('npm was not found. Install Node.js 24 or newer, then retry Export for Portal.');
  const lease = path.join(dir, '.installing');
  let fd;
  for (let attempt = 0; attempt < 2; ++attempt) {
    try { fd = fs.openSync(lease, 'wx'); break; }
    catch (error) {
      if (error.code !== 'EEXIST') throw error;
      const owner = Number(fs.readFileSync(lease, 'utf8'));
      let alive = true;
      if (Number.isSafeInteger(owner) && owner > 0) {
        try { process.kill(owner, 0); } catch (e) { if (e.code === 'ESRCH') alive = false; }
      }
      // An empty lease can belong to an installer that has just opened it.
      else alive = Date.now() - fs.statSync(lease).mtimeMs < 60000;
      if (alive || attempt) throw Error('Another export is installing the build tools. Wait for it to finish, then retry.');
      fs.unlinkSync(lease);
      console.log('Recovering an interrupted build tools installation...');
    }
  }
  try {
    fs.writeFileSync(fd, String(process.pid));
    if (fs.existsSync(ready)) fs.unlinkSync(ready);
    for (const name of ['package.json', 'package-lock.json']) fs.copyFileSync(path.join(TEMPLATE, name), path.join(dir, name));
    console.log('Installing the Portal build tools. The first export needs an internet connection.');
    await run([npmCli, 'ci', '--ignore-scripts', '--no-audit', '--no-fund'], dir);
    fs.writeFileSync(ready, digest(lock));
    if (!valid()) throw Error('The installed build tools do not match the shipped versions.');
    return dir;
  } finally { fs.closeSync(fd); fs.unlinkSync(lease); }
}

function validateBundle(ts, project, bundle) {
  const text = fs.readFileSync(bundle, 'utf8');
  if (!text.trim()) throw Error('The bundler produced an empty script.');
  const source = ts.createSourceFile(bundle, text, ts.ScriptTarget.Latest, true);
  let external = false;
  function visit(node) {
    if (ts.isImportDeclaration(node) || ts.isImportEqualsDeclaration(node) ||
        (ts.isExportDeclaration(node) && node.moduleSpecifier) ||
        (ts.isCallExpression(node) && node.expression.kind === ts.SyntaxKind.ImportKeyword)) external = true;
    ts.forEachChild(node, visit);
  }
  visit(source);
  if (external) throw Error('The combined script still contains imports. It is not ready for Portal.');
  // The community bundler writes @ts-nocheck. Check its actual combined output
  // independently so stripped aliases and cross-module collisions cannot pass.
  const checkFile = path.resolve(bundle);
  const checkedText = text.replace(/\/\/\s*@ts-nocheck[^\r\n]*/g, '');
  const cfg = ts.readConfigFile(path.join(project, 'tsconfig.json'), ts.sys.readFile);
  const parsed = ts.parseJsonConfigFileContent(cfg.config, ts.sys, project, undefined, path.join(project,'tsconfig.json'));
  if (cfg.error) throw Error('Could not read the project TypeScript configuration.');
  if (parsed.errors.length) throw Error('Invalid project TypeScript configuration.');
  if (!parsed.options.typeRoots) parsed.options.typeRoots = [path.join(project,'node_modules/@types'),path.join(project,'node_modules')];
  parsed.options.noEmit = true;
  const host = ts.createCompilerHost(parsed.options);
  const originalRead = host.readFile;
  host.readFile = file => path.resolve(file).toLowerCase() === checkFile.toLowerCase() ? checkedText : originalRead(file);
  const declarations = parsed.fileNames.filter(file => file.endsWith('.d.ts'));
  const program = ts.createProgram([checkFile,...declarations], parsed.options, host);
  const diagnostics = ts.getPreEmitDiagnostics(program).filter(d => d.category === ts.DiagnosticCategory.Error);
  if (diagnostics.length) throw Error('Combined script validation failed:\n' + ts.formatDiagnostics(diagnostics, {
    getCanonicalFileName: x => x, getCurrentDirectory: () => project, getNewLine: () => '\n'
  }));
  return text;
}

async function build(job, jobDir) {
  if (Number(process.versions.node.split('.')[0]) < 24) throw Error('Export for Portal requires Node.js 24 or newer.');
  const files = sourceFiles(job.files);
  if (typeof job.outputDir !== 'string' || !path.isAbsolute(job.outputDir)) throw Error('Choose an absolute export folder.');
  if (typeof job.toolchainDir !== 'string' || !path.isAbsolute(job.toolchainDir)) throw Error('Missing build tools folder.');
  const tools = await dependencies(job.toolchainDir, job.npmCli);
  const project = path.join(jobDir, 'project');
  // A job owns a new isolated source tree. Never compile unrelated source left
  // by an earlier export or overwrite the Script editor's handwritten project.
  fs.mkdirSync(project);
  for (const name of ['package.json', 'package-lock.json', 'tsconfig.json'])
    fs.copyFileSync(path.join(TEMPLATE, name), path.join(project, name));
  for (const [name, body] of Object.entries(files)) {
    const dest = path.join(project, name); fs.mkdirSync(path.dirname(dest), { recursive: true }); fs.writeFileSync(dest, body);
  }
  fs.symlinkSync(path.join(tools, 'node_modules'), path.join(project, 'node_modules'), 'junction');
  console.log('Checking generated TypeScript...');
  await run([path.join(tools, 'node_modules/typescript/bin/tsc'), '--noEmit', '--pretty', 'false', '-p', project], project);
  console.log('Bundling the mode into one script...');
  const ts = require(path.join(tools, 'node_modules/typescript'));
  fs.writeFileSync(path.join(project,'bundle-input.ts'), require('./generated-modules.cjs')(ts,project));
  let stringsIndex=0;
  for (const [name,body] of Object.entries(files)) if (name.startsWith('src/') && name.endsWith('strings.json'))
    fs.writeFileSync(path.join(project,'module-'+stringsIndex+++'.strings.json'),body);
  await run([path.join(tools, 'node_modules/bf6-portal-bundler/index.js'), '--entrypoint', './bundle-input.ts', '--outDir', './dist'], project);
  const text = validateBundle(ts, project, path.join(project, 'dist/bundle.ts'));
  const stringsPath = path.join(project, 'dist/bundle.strings.json');
  const strings = fs.existsSync(stringsPath) ? fs.readFileSync(stringsPath, 'utf8') : '{}\n';
  const value = JSON.parse(strings);
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error('The combined strings file is not a JSON object.');
  fs.mkdirSync(job.outputDir, { recursive: true });
  // A distinct folder per successful export makes an old successful bundle
  // impossible to mistake for the output of a failed rebuild.
  const staging = fs.mkdtempSync(path.join(job.outputDir, '.portal-export-pending-'));
  const output = path.join(job.outputDir, 'Portal-Export-' + path.basename(staging).split('-').pop());
  try {
    fs.writeFileSync(path.join(staging, 'bundle.ts'), text);
    fs.writeFileSync(path.join(staging, 'bundle.strings.json'), strings);
    fs.writeFileSync(path.join(staging, 'README.txt'), 'Upload bundle.ts in the Portal script editor and bundle.strings.json in its strings upload.\nBoth files belong to this export.\n');
    fs.renameSync(staging, output);
  } catch (error) {
    // This path was allocated by this job inside the chosen output folder.
    fs.rmSync(staging, {recursive:true, force:true});
    throw error;
  }
  console.log('Portal export ready: ' + output);
  return { ok: true, outputDir: output, sourceDir: project, script: path.join(output, 'bundle.ts'), strings: path.join(output, 'bundle.strings.json') };
}

if (require.main === module) {
  const request = process.argv[2];
  const jobDir = path.dirname(path.resolve(request || '.'));
  Promise.resolve().then(() => build(JSON.parse(fs.readFileSync(request, 'utf8')), jobDir))
    .then(result => fs.writeFileSync(path.join(jobDir, 'result.json'), JSON.stringify(result)))
    .catch(error => {
      console.error(error.message);
      fs.writeFileSync(path.join(jobDir, 'result.json'), JSON.stringify({ ok: false, error: error.message }));
      process.exitCode = 1;
    });
}
module.exports = { sourceFiles, validateBundle, build };
