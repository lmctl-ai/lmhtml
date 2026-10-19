#!/usr/bin/env node
// A narrow adapter for compiling _dsl_ blocks from a lmscript source checkout.
// Public users should use the published lmscript CLI instead.
import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {basename, dirname, resolve, join} from 'node:path';
import {pathToFileURL} from 'node:url';

if (!process.env.LMSCRIPT_SOURCE) {
  console.error('error: source adapter requires LMSCRIPT_SOURCE; install @lmctl-ai/lmctl and use lmscript for normal builds');
  process.exit(2);
}
const sourceRoot = resolve(process.env.LMSCRIPT_SOURCE);
let dsl;
try {
  dsl = await import(pathToFileURL(join(sourceRoot, 'dsl.ts')).href);
  if (dsl.DSL_VERSION !== 5) throw new Error(`expected DSL v5, found v${dsl.DSL_VERSION}`);
} catch (error) {
  console.error(`error: cannot load DSL v5 compiler from LMSCRIPT_SOURCE=${sourceRoot}. ${error.message}`);
  process.exit(2);
}
const args = process.argv.slice(2);

if (args.length === 1 && ['--version', '-v'].includes(args[0])) {
  console.log(`lmscript DSL v${dsl.DSL_VERSION} (${sourceRoot}/dsl.ts)`);
  process.exit(0);
}
if (args[0] === '--dsl-signatures') {
  if (args.length !== 2) { console.error('error: --dsl-signatures needs exactly one schema path'); process.exit(2); }
  const schemaPath = resolve(args[1]);
  const schema = dsl.parseDslSchema(readFileSync(schemaPath), schemaPath);
  console.log(dsl.canonicalSignatures(schema).join('\n'));
  process.exit(0);
}
if (args.length < 1 || args.length > 2 || args[0].startsWith('-')) {
  console.error('usage: lmscript-source <source.lms> [output.dslb]');
  process.exit(2);
}

try {
  const sourcePath = resolve(args[0]);
  const source = readFileSync(sourcePath, 'utf8').replace(/\r\n?/g, '\n');
  const openers = [...source.matchAll(/^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*_dsl_[ \t]+schema[ \t]*=[ \t]*("(?:\\.|[^"\\\r\n])*")[ \t]*;[ \t]*(?:#[^\r\n]*)?$/gm)];
  if (openers.length !== 1) throw new Error('adapter requires exactly one standalone _dsl_ block');
  const opener = openers[0];
  if (!opener) throw new Error('could not find `_dsl_ schema="...";`');
  const bodyStart = opener.index + opener[0].length;
  const closing = /^[ \t]*end _dsl_[ \t]*;?[ \t]*(?:#[^\r\n]*)?$/m.exec(source.slice(bodyStart));
  const end = closing ? bodyStart + closing.index : -1;
  if (end < 0) throw new Error('could not find `end _dsl_`');
  const schemaName = JSON.parse(opener[2]);
  const schemaPath = resolve(dirname(sourcePath), schemaName);
  const schema = dsl.parseDslSchema(readFileSync(schemaPath), schemaPath);
  const compiled = dsl.compileDsl(source.slice(bodyStart, end).split(/\r?\n/), schema);
  const outputs = [...source.slice(end + closing[0].length).matchAll(/^[ \t]*savedsl[ \t]*\([ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*,[ \t]*("(?:\\.|[^"\\\r\n])*")[ \t]*\)[ \t]*;?[ \t]*(?:#[^\r\n]*)?$/gm)];
  const output = outputs.find(match => match[1] === opener[1]);
  if (!args[1] && !output) throw new Error('provide an output.dslb argument or a literal savedsl path for this block');
  const outputPath = resolve(args[1] ?? JSON.parse(output[2]));
  mkdirSync(dirname(outputPath), {recursive: true});
  writeFileSync(outputPath, compiled.bytes);
  console.log(`wrote ${outputPath}`);
} catch (error) {
  console.error(`error:${basename(args[0])}: ${error instanceof Error ? error.message : String(error)}`);
  process.exit(2);
}
