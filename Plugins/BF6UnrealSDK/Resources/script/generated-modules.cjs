'use strict';
// Generated Blockly modules use namespace imports and mutable exported state.
// Preserve them explicitly before the Portal bundler strips ES module syntax.
module.exports = function generatedModules(ts, project) {
  const path = require('node:path');
  const configPath=path.join(project,'tsconfig.json');
  const cfg=ts.readConfigFile(configPath,ts.sys.readFile);
  const parsed=ts.parseJsonConfigFileContent(cfg.config,ts.sys,project,undefined,configPath);
  const program=ts.createProgram(parsed.fileNames,parsed.options), checker=program.getTypeChecker();
  const entry=program.getSourceFile(path.join(project,'src/index.ts'));
  if (!entry) throw Error('Generated entrypoint is missing.');
  const identifiers = new Set();
  for (const sf of program.getSourceFiles()) if (!sf.isDeclarationFile) {
    const visit = node => { if (ts.isIdentifier(node)) identifiers.add(node.text); ts.forEachChild(node,visit); };
    visit(sf);
  }
  let prefix = '__bf6_module_';
  while ([...identifiers].some(name => name.startsWith(prefix))) prefix = '_' + prefix;
  const modules=new Map(), order=[], visiting=new Set();
  function discover(sf) {
    if (visiting.has(sf)) throw Error('The generated module graph has a circular dependency: '+sf.fileName);
    if (modules.has(sf)) return modules.get(sf);
    visiting.add(sf);
    const record={sf,name:prefix+modules.size,bindings:new Map()}; modules.set(sf,record);
    for (const node of sf.statements) {
      if ((ts.isExportDeclaration(node) && (node.moduleSpecifier || !node.exportClause || node.exportClause.elements.length)) || ts.isExportAssignment(node)) throw Error('Generated re-exports/default exports are unsupported: '+sf.fileName);
      if (!ts.isImportDeclaration(node)) continue;
      if (!ts.isStringLiteral(node.moduleSpecifier) || !node.moduleSpecifier.text.startsWith('.'))
        throw Error('Generated modules may only import their own relative source files.');
      const resolved=ts.resolveModuleName(node.moduleSpecifier.text,sf.fileName,parsed.options,ts.sys).resolvedModule;
      const target=resolved && program.getSourceFile(resolved.resolvedFileName);
      const relative=target?path.relative(path.join(project,'src'),target.fileName):'..';
      if (!target || target.isDeclarationFile || relative==='..' || relative.startsWith('..'+path.sep) || path.isAbsolute(relative))
        throw Error('Generated import does not resolve inside src/: '+node.moduleSpecifier.text);
      const other=discover(target);
      const clause=node.importClause;
      if (!clause) continue;
      if (clause.name) throw Error('Default imports are not emitted by the Blockly compiler.');
      const bindings=clause.namedBindings;
      if (bindings && ts.isNamespaceImport(bindings)) record.bindings.set(checker.getSymbolAtLocation(bindings.name),[other.name]);
      if (bindings && ts.isNamedImports(bindings)) for (const item of bindings.elements)
        record.bindings.set(checker.getSymbolAtLocation(item.name),[other.name,(item.propertyName||item.name).text]);
    }
    visiting.delete(sf); order.push(record); return record;
  }
  const root=discover(entry), printer=ts.createPrinter({newLine:ts.NewLineKind.LineFeed});
  const output=[];
  for (const record of order) {
    function names(parts, type) { let node=ts.factory.createIdentifier(parts[0]); for (const part of parts.slice(1))
      node=type?ts.factory.createQualifiedName(node,part):ts.factory.createPropertyAccessExpression(node,part); return node; }
    const transform=context=>{
      function visit(node) {
        if (ts.isImportDeclaration(node)) return undefined;
        if (ts.isExportDeclaration(node)) return undefined;
        if (ts.isImportEqualsDeclaration(node)) throw Error('Unexpected import assignment in generated source.');
        if (ts.isCallExpression(node) && node.expression.kind===ts.SyntaxKind.ImportKeyword) throw Error('Dynamic imports cannot be uploaded to Portal.');
        if (ts.isShorthandPropertyAssignment(node)) {
          const parts=record.bindings.get(checker.getShorthandAssignmentValueSymbol(node));
          if (parts) return ts.factory.createPropertyAssignment(node.name,names(parts,false));
        }
        if (ts.isIdentifier(node)) {
          const parts=record.bindings.get(checker.getSymbolAtLocation(node));
          if (parts) {
            const p=node.parent;
            const type=(ts.isTypeReferenceNode(p)&&p.typeName===node)||ts.isQualifiedName(p)||ts.isTypeQueryNode(p);
            return names(parts,type);
          }
        }
        return ts.visitEachChild(node,visit,context);
      }
      return sf=>ts.visitNode(sf,visit);
    };
    const result=ts.transform(record.sf,[transform]);
    try { output.push('// Generated module: '+path.relative(project,record.sf.fileName).replace(/\\/g,'/')+'\nnamespace '+record.name+' {\n'+printer.printFile(result.transformed[0])+'\n}\n'); }
    finally { result.dispose(); }
  }
  const symbol=checker.getSymbolAtLocation(entry);
  for (const item of symbol?checker.getExportsOfModule(symbol):[]) {
    const name=item.getName();
    if (!/^[$A-Z_a-z][$\w]*$/.test(name) || name==='default') throw Error('Invalid generated entry export: '+name);
    if (!(item.declarations || []).some(ts.isFunctionDeclaration))
      throw Error('Generated entrypoints may only export functions: '+name);
    const target=root.name+'.'+name;
    output.push('export function '+name+'(...args: Parameters<typeof '+target+'>): ReturnType<typeof '+target+'> { return '+target+'(...args); }');
  }
  if (!symbol || !checker.getExportsOfModule(symbol).length) throw Error('The generated script has no exported event handlers. Connect a rule to the mod root before exporting.');
  return output.join('\n')+'\n';
};
