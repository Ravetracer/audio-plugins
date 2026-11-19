// Runs a WCLAP's editor (ui.wasm) in node with the page's WASI shim and
// writes one rendered frame as a PNG: the editor offscreen, no browser.
// Messages the editor sends to the plugin are counted, not answered. Every
// file in the bundle's fonts/ is put at /fonts/<name>, as the page does.
//
//   node shared/tools/wclap-ui-snapshot.mjs <bundle dir> out.png [width height dpr] [--click x,y ...]
import {readFile, readdir, writeFile} from 'node:fs/promises'
import {deflateSync} from 'node:zlib'
import {WasiMemFs} from '../web/wasi.js'

const args = process.argv.slice(2)
const clicks = []
for (let i = args.indexOf('--click'); i >= 0; i = args.indexOf('--click')) {
    clicks.push(args[i + 1].split(',').map(Number))
    args.splice(i, 2)
}
const [bundle, out, w = '1000', h = '660', dpr = '1'] = args
if (!bundle || !out) {
    console.error('usage: ui-snapshot.mjs <bundle dir> <out.png> [width height dpr] [--click x,y]')
    process.exit(2)
}

const fs = new WasiMemFs({env: {HOME: '/home/web'}})
for (const f of await readdir(`${bundle}/fonts`).catch(() => []))
    fs.writeFile('/fonts/' + f, new Uint8Array(await readFile(`${bundle}/fonts/${f}`)))

let ex = null, sent = 0
const memory = () => ex.memory
const {instance} = await WebAssembly.instantiate(await readFile(`${bundle}/ui.wasm`), {
    wasi_snapshot_preview1: fs.imports(memory),
    wclap: {send() { ++sent }, file_dialog() {}, download() {}},
})
ex = instance.exports
ex._initialize()

const t0 = performance.now()
ex.wclap_ui_start(Number(w), Number(h), Number(dpr))
ex.wclap_ui_tick()
const t1 = performance.now()
for (const [x, y] of clicks) {
    ex.wclap_ui_pointer_move(x, y, 0)
    ex.wclap_ui_pointer_down(x, y, 1, 0, 1)
    ex.wclap_ui_pointer_up(x, y, 1, 0)
}
// Let the editor's timer run a few frames, as the page does.
let serial = 0
for (let i = 0; i < 4; ++i) serial = ex.wclap_ui_tick()
const t2 = performance.now()

const fw = ex.wclap_ui_frame_width(), fh = ex.wclap_ui_frame_height()
const rgba = new Uint8Array(memory().buffer, ex.wclap_ui_frame(), fw * fh * 4)
await writeFile(out, png(rgba, fw, fh))
console.log(`${out}: ${fw}x${fh}, frame ${serial}, first frame ${(t1 - t0).toFixed(1)} ms, ` +
    `4 ticks ${(t2 - t1).toFixed(1)} ms, ${sent} messages to the plugin`)

function png(rgba, width, height) {
    const raw = Buffer.alloc((width * 4 + 1) * height)
    for (let y = 0; y < height; ++y) {
        raw[y * (width * 4 + 1)] = 0
        Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4).copy(raw, y * (width * 4 + 1) + 1)
    }
    const chunk = (type, data) => {
        const len = Buffer.alloc(4)
        len.writeUInt32BE(data.length)
        const td = Buffer.concat([Buffer.from(type), data])
        const crc = Buffer.alloc(4)
        crc.writeUInt32BE(crc32(td) >>> 0)
        return Buffer.concat([len, td, crc])
    }
    const ihdr = Buffer.alloc(13)
    ihdr.writeUInt32BE(width, 0)
    ihdr.writeUInt32BE(height, 4)
    ihdr[8] = 8
    ihdr[9] = 6
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
        chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))])
}

function crc32(buf) {
    let c, crc = 0xffffffff
    for (let n = 0; n < buf.length; n++) {
        c = (crc ^ buf[n]) & 0xff
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
        crc = (crc >>> 8) ^ c
    }
    return crc ^ 0xffffffff
}
