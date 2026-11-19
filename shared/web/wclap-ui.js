// The page of a WCLAP whose editor is a WebAssembly module (ui.wasm): the
// plugin's native Cairo editor compiled for wasm32-wasip1. This runtime loads
// it, shows what it draws in a canvas, feeds it pointer, wheel and key input
// and file picks/drops, and carries its messages to and from the plugin
// (module.wasm in the host's audio thread): out with window.parent.postMessage,
// in as "message" events.
//
// A bundle's index.html calls start() with its own settings:
//
//   import {start} from './wclap-ui.js'
//   start({fonts: ['Inter-Medium.ttf', 'Inter-Bold.ttf'], storageKey: 'aurum-wclap-files-v1'})
//
// The editor module exports the wclap_ui_* functions below and imports
// wclap.send, wclap.file_dialog and wclap.download; see
// shared/web/README.md for the whole contract.
import {WasiMemFs} from './wasi.js'

// Keys the editors know by their X11 keysym.
const KEYSYMS = {
    Backspace: 0xff08, Tab: 0xff09, Enter: 0xff0d, Escape: 0xff1b, Home: 0xff50, ArrowLeft: 0xff51,
    ArrowUp: 0xff52, ArrowRight: 0xff53, ArrowDown: 0xff54, PageUp: 0xff55, PageDown: 0xff56, End: 0xff57,
    Insert: 0xff63, Delete: 0xffff, Shift: 0xffe1, Control: 0xffe3, Alt: 0xffe9, CapsLock: 0xffe5,
    F1: 0xffbe, F2: 0xffbf, F3: 0xffc0, F4: 0xffc1, F5: 0xffc2, F6: 0xffc3, F7: 0xffc4, F8: 0xffc5,
    F9: 0xffc6, F10: 0xffc7, F11: 0xffc8, F12: 0xffc9,
}

const mods = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey || e.metaKey ? 2 : 0) | (e.altKey ? 4 : 0)

async function fetchBytes(url) {
    const r = await fetch(url)
    if (!r.ok) throw new Error(`${url}: ${r.status}`)
    return new Uint8Array(await r.arrayBuffer())
}

export async function start(config = {}) {
    const canvas = document.getElementById('ui')
    const status = document.getElementById('status')
    try {
        await run(canvas, status, config)
    } catch (err) {
        console.error('WCLAP editor:', err)
        if (status) status.textContent = 'The editor failed to start: ' + err.message
    }
}

async function run(canvas, status, config) {
    const ctx = canvas.getContext('2d', {alpha: false})
    // Settings and user presets under /home/web persist in this browser, as
    // far as it gives the page storage.
    const fs = new WasiMemFs({env: {HOME: '/home/web', ...(config.env || {})},
        persistKey: config.storageKey || null, persistRoot: '/home/web'})
    for (const f of config.fonts || []) fs.writeFile('/fonts/' + f, await fetchBytes('fonts/' + f))
    for (const [path, url] of Object.entries(config.files || {})) fs.writeFile(path, await fetchBytes(url))

    let ex = null
    const memory = () => ex.memory
    const bytesAt = (ptr, len) => new Uint8Array(memory().buffer, ptr, len)
    const text = (ptr, len) => new TextDecoder().decode(bytesAt(ptr, len))
    const pass = bytes => { // into the editor's scratch buffer
        const ptr = ex.wclap_ui_buffer(bytes.length)
        bytesAt(ptr, bytes.length).set(bytes)
        return ptr
    }
    const imports = {
        wasi_snapshot_preview1: fs.imports(memory),
        wclap: {
            send(ptr, len) {
                const copy = bytesAt(ptr, len).slice()
                window.parent.postMessage(copy.buffer, '*', [copy.buffer])
            },
            file_dialog(id, mode, filterPtr, filterLen) {
                openFileDialog(fs, mode, text(filterPtr, filterLen), path => {
                    const enc = new TextEncoder().encode(path)
                    ex.wclap_ui_dialog_done(id, pass(enc), enc.length)
                })
            },
            // Hands the user a file (an export): its name and bytes.
            download(namePtr, nameLen, dataPtr, dataLen) {
                const blob = new Blob([bytesAt(dataPtr, dataLen).slice()])
                const a = document.createElement('a')
                a.href = URL.createObjectURL(blob)
                a.download = text(namePtr, nameLen).split('/').pop() || 'download'
                document.body.appendChild(a)
                a.click()
                a.remove()
                setTimeout(() => URL.revokeObjectURL(a.href), 10000)
            },
        },
    }
    const {instance} = await WebAssembly.instantiate(await fetchBytes('ui.wasm'), imports)
    ex = instance.exports
    ex._initialize()

    window.addEventListener('message', event => {
        let data = event.data
        if (ArrayBuffer.isView(data)) data = new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
        else if (data instanceof ArrayBuffer) data = new Uint8Array(data)
        else return
        pass(data)
        ex.wclap_ui_message(data.length)
    })

    // The canvas covers the page at the screen's pixel density.
    let dpr = window.devicePixelRatio || 1
    const cssSize = () => [Math.max(1, Math.round(window.innerWidth)), Math.max(1, Math.round(window.innerHeight))]
    const [w0, h0] = cssSize()
    ex.wclap_ui_start(w0, h0, dpr)
    const resize = () => {
        dpr = window.devicePixelRatio || 1
        const [w, h] = cssSize()
        ex.wclap_ui_resize(w, h, dpr)
    }
    window.addEventListener('resize', resize)
    new ResizeObserver(resize).observe(document.documentElement)

    let shown = 0
    const frame = () => {
        const serial = ex.wclap_ui_tick()
        if (serial !== shown) {
            shown = serial
            const w = ex.wclap_ui_frame_width(), h = ex.wclap_ui_frame_height()
            if (w > 0 && h > 0) {
                if (canvas.width !== w || canvas.height !== h) {
                    canvas.width = w
                    canvas.height = h
                }
                const pixels = new Uint8ClampedArray(memory().buffer, ex.wclap_ui_frame(), w * h * 4)
                ctx.putImageData(new ImageData(pixels, w, h), 0, 0)
                if (status) status.style.display = 'none'
            }
        }
        requestAnimationFrame(frame)
    }
    requestAnimationFrame(frame)

    // Pointer, in canvas pixels; buttons 1 left, 2 middle, 3 right.
    const at = e => {
        const r = canvas.getBoundingClientRect()
        return [(e.clientX - r.left) * canvas.width / Math.max(r.width, 1),
                (e.clientY - r.top) * canvas.height / Math.max(r.height, 1)]
    }
    const button = e => e.button === 0 ? 1 : e.button === 1 ? 2 : e.button === 2 ? 3 : 0
    let lastDown = {t: 0, x: 0, y: 0, b: 0}
    canvas.addEventListener('pointerdown', e => {
        canvas.focus({preventScroll: true})
        canvas.setPointerCapture(e.pointerId)
        const [x, y] = at(e), b = button(e)
        const double = b === lastDown.b && e.timeStamp - lastDown.t < 400 &&
            Math.abs(x - lastDown.x) < 5 * dpr && Math.abs(y - lastDown.y) < 5 * dpr
        lastDown = double ? {t: 0, x, y, b} : {t: e.timeStamp, x, y, b}
        ex.wclap_ui_pointer_down(x, y, b, mods(e), double ? 2 : 1)
    })
    canvas.addEventListener('pointerup', e => {
        const [x, y] = at(e)
        ex.wclap_ui_pointer_up(x, y, button(e), mods(e))
    })
    canvas.addEventListener('pointermove', e => {
        const [x, y] = at(e)
        ex.wclap_ui_pointer_move(x, y, mods(e))
    })
    canvas.addEventListener('pointerleave', e => { if (!e.buttons) ex.wclap_ui_leave() })
    canvas.addEventListener('contextmenu', e => e.preventDefault())
    canvas.addEventListener('wheel', e => {
        const [x, y] = at(e)
        const unit = e.deltaMode === 1 ? 3 : e.deltaMode === 2 ? 1 : 100
        ex.wclap_ui_wheel(x, y, -e.deltaY / unit, mods(e))
        e.preventDefault()
    }, {passive: false})

    // Keys: named keys by keysym, printable ones by code point with their
    // text. Key-up goes to editors that want it (a C64 keyboard does).
    const key = (e, down) => {
        let keysym = KEYSYMS[e.key] || 0
        let txt = ''
        if (!keysym && [...e.key].length === 1) {
            keysym = e.key.codePointAt(0)
            if (!(e.ctrlKey || e.metaKey)) txt = e.key
        }
        if (!keysym) return
        const enc = new TextEncoder().encode(txt)
        pass(enc)
        const used = down ? ex.wclap_ui_key(keysym, enc.length, mods(e))
            : ex.wclap_ui_key_up ? ex.wclap_ui_key_up(keysym, mods(e)) : 0
        if (used) e.preventDefault()
    }
    canvas.addEventListener('keydown', e => key(e, true))
    canvas.addEventListener('keyup', e => key(e, false))

    // Files dropped on the editor: copied into /upload, their paths handed
    // over one per line.
    if (ex.wclap_ui_files_dropped) {
        canvas.addEventListener('dragover', e => e.preventDefault())
        canvas.addEventListener('drop', async e => {
            e.preventDefault()
            const files = [...(e.dataTransfer?.files || [])]
            if (!files.length) return
            const stamp = Date.now().toString(36)
            const paths = []
            for (const f of files) {
                const path = `/upload/${stamp}/${f.name}`
                fs.writeFile(path, new Uint8Array(await f.arrayBuffer()))
                paths.push(path)
            }
            const [x, y] = at(e)
            const enc = new TextEncoder().encode(paths.join('\n'))
            pass(enc)
            ex.wclap_ui_files_dropped(enc.length, x, y)
        })
    }
}

// The editors' file dialogs: the browser's picker, the picked files copied
// into /upload in the editor's file system. Mode 0 opens a file, 1 a folder,
// 2 asks for a file name to save under (answered with a path in /download;
// the editor then hands the written file over with wclap.download).
function openFileDialog(fs, mode, filter, done) {
    if (mode === 2) {
        const name = window.prompt('Save as', '')
        done(name ? '/download/' + name.replace(/[\/\\]/g, '_') : '')
        return
    }
    const input = document.createElement('input')
    input.type = 'file'
    if (mode === 1) input.webkitdirectory = true
    const patterns = (filter.split('|')[1] || '').trim().split(/\s+/).filter(Boolean)
    const exts = [...new Set(patterns.map(p => p.replace(/^\*/, '').toLowerCase()).filter(p => p.startsWith('.')))]
    if (exts.length && mode !== 1) input.accept = exts.join(',')
    let answered = false
    const answer = path => { if (!answered) { answered = true; done(path) } }
    input.addEventListener('cancel', () => answer(''))
    input.addEventListener('change', async () => {
        const files = [...input.files]
        if (!files.length) return answer('')
        const stamp = Date.now().toString(36)
        let result = ''
        for (const f of files) {
            const rel = mode === 1 && f.webkitRelativePath ? f.webkitRelativePath : f.name
            const path = `/upload/${stamp}/${rel}`
            fs.writeFile(path, new Uint8Array(await f.arrayBuffer()))
            if (!result) result = mode === 1 ? `/upload/${stamp}/${rel.split('/')[0]}` : path
        }
        answer(result)
    })
    input.click()
}
