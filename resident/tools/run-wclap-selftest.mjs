// Runs the WCLAP self-test (a WASI command) under Node's WASI with a ROM
// folder and, optionally, a program made readable inside it -- the ROMs are
// handed in at run time, never compiled in. Nothing else of the file system
// is visible to the test.
//
//   node tools/run-wclap-selftest.mjs build-wasm/resident-wclap-selftest.wasm <rom folder> [program] [--bench]
import {readFile} from 'node:fs/promises'
import {WASI} from 'node:wasi'
import path from 'node:path'

const [file, romDir, ...rest] = process.argv.slice(2)
if (!file || !romDir) {
    console.error('usage: node run-wclap-selftest.mjs <selftest.wasm> <rom folder> [program] [--bench]')
    process.exit(2)
}
const args = [file, '--roms', '/roms']
const preopens = {'/roms': path.resolve(romDir)}
for (const a of rest) {
    if (a.startsWith('--')) { args.push(a); continue }
    preopens['/program'] = path.resolve(path.dirname(a))
    args.push('--program', '/program/' + path.basename(a))
}
const wasi = new WASI({version: 'preview1', args, env: {}, preopens, returnOnExit: true})
const module = await WebAssembly.compile(await readFile(file))
const instance = await WebAssembly.instantiate(module, wasi.getImportObject())
process.exit(wasi.start(instance))
