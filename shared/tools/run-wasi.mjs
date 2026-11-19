// Runs a WASI command module under Node's built-in WASI, with no file system
// access and no environment -- the same as a browser host gives a WCLAP.
//   node shared/tools/run-wasi.mjs <plugin>/build-wasm/<selftest>.wasm
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';

const file = process.argv[2];
if (!file) {
    console.error('usage: node run-wasi.mjs <module.wasm>');
    process.exit(2);
}
const wasi = new WASI({ version: 'preview1', args: [file], env: {}, returnOnExit: true });
const module = await WebAssembly.compile(await readFile(file));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
process.exit(wasi.start(instance));
