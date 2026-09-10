'use strict';
// Keep the project's chosen build command, then check what it actually emitted.
const {spawn} = require('node:child_process');
const path = require('node:path');
const {validateBundle} = require('./portal-export.cjs');
const project = path.resolve(process.argv[2]);
const child = spawn(process.execPath,[process.argv[3],'run','build'],{cwd:project,windowsHide:true,stdio:'inherit'});
child.on('error',error=>{console.error(error.message);process.exitCode=1;});
child.on('close',code=>{
  if (code !== 0) {process.exitCode=code || 1;return;}
  try {
    const ts = require(require.resolve('typescript',{paths:[project]}));
    validateBundle(ts,project,path.join(project,'dist/bundle.ts'));
    console.log('The combined upload script passed validation.');
  } catch(error) {console.error(error.message);process.exitCode=1;}
});
