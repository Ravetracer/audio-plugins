// WASI preview 1 for the editor module (ui.wasm), over an in-memory file
// system: what the editor reads and writes as files (settings, presets,
// fonts, uploads) lives here. One tree under a preopened "/", plain files and
// directories, no links. The part below `persistRoot` is saved to
// localStorage when it changes and loaded again on the next start, so user
// presets and settings survive, as far as the browser keeps the page's storage.

const E = {
    SUCCESS: 0, ACCES: 2, BADF: 8, EXIST: 20, INVAL: 28, IO: 29, ISDIR: 31, NOENT: 44, NOSYS: 52,
    NOTDIR: 54, NOTEMPTY: 55, NOTSUP: 58, SPIPE: 70,
}
const FILETYPE_DIRECTORY = 3, FILETYPE_REGULAR = 4, FILETYPE_CHAR = 2
const O_CREAT = 1, O_DIRECTORY = 2, O_EXCL = 4, O_TRUNC = 8
const FD_APPEND = 1

let nextIno = 1n
const dirNode = () => ({type: 'dir', children: new Map(), ino: nextIno++, mtime: now()})
const fileNode = () => ({type: 'file', data: new Uint8Array(0), size: 0, ino: nextIno++, mtime: now()})
const now = () => BigInt(Date.now()) * 1000000n

class ProcExit extends Error {
    constructor(code) { super(`exit ${code}`); this.code = code }
}

export class WasiMemFs {
    constructor({env = {}, persistKey = null, persistRoot = null} = {}) {
        this.env = Object.entries(env).map(([k, v]) => `${k}=${v}`)
        this.root = dirNode()
        this.fds = new Map()
        this.fds.set(3, {node: this.root, path: '/', preopen: '/'})
        this.nextFd = 4
        this.memory = null
        this.persistKey = persistKey
        this.persistRoot = persistRoot
        this.persistTimer = 0
        this.stdout = ''
        this.load()
    }

    // ------------------------------------------------------------ the tree

    split(path) {
        return path.split('/').filter(p => p.length && p !== '.')
    }

    // Resolves `path` from directory `base`; returns {parent, name, node}.
    resolve(base, path) {
        const parts = this.split(path)
        const stack = [base]
        for (let i = 0; i < parts.length - 1; ++i) {
            const p = parts[i]
            if (p === '..') { if (stack.length > 1) stack.pop(); continue }
            const next = stack[stack.length - 1].children.get(p)
            if (!next) return {error: E.NOENT}
            if (next.type !== 'dir') return {error: E.NOTDIR}
            stack.push(next)
        }
        const name = parts.length ? parts[parts.length - 1] : '.'
        const parent = stack[stack.length - 1]
        if (name === '..') {
            const up = stack.length > 1 ? stack[stack.length - 2] : parent
            return {parent: up, name: '.', node: up}
        }
        if (name === '.') return {parent, name, node: parent}
        return {parent, name, node: parent.children.get(name)}
    }

    absolute(path) {
        const r = this.resolve(this.root, path)
        return r
    }

    mkdirs(path) {
        let dir = this.root
        for (const p of this.split(path)) {
            let next = dir.children.get(p)
            if (!next) { next = dirNode(); dir.children.set(p, next) }
            dir = next
        }
        return dir
    }

    writeFile(path, bytes) {
        const parts = this.split(path)
        const dir = this.mkdirs(parts.slice(0, -1).join('/'))
        const f = fileNode()
        f.data = bytes
        f.size = bytes.length
        dir.children.set(parts[parts.length - 1], f)
        this.changed(path)
    }

    // ------------------------------------------------------------ persistence

    changed(path) {
        if (!this.persistKey || !this.persistRoot || !path.startsWith(this.persistRoot)) return
        clearTimeout(this.persistTimer)
        this.persistTimer = setTimeout(() => this.save(), 400)
    }

    changedNode() {
        // Path unknown (an fd without one): save if anything is persisted at all.
        if (this.persistKey && this.persistRoot) this.changed(this.persistRoot)
    }

    save() {
        const out = {}
        const walk = (node, path) => {
            for (const [name, child] of node.children) {
                const p = path + '/' + name
                if (child.type === 'dir') { out[p] = null; walk(child, p) }
                else out[p] = toBase64(child.data.subarray(0, child.size))
            }
        }
        const r = this.absolute(this.persistRoot)
        if (!r.node || r.node.type !== 'dir') return
        walk(r.node, this.persistRoot)
        try { localStorage.setItem(this.persistKey, JSON.stringify(out)) } catch (e) { /* no storage here */ }
    }

    load() {
        if (!this.persistKey) return
        let text = null
        try { text = localStorage.getItem(this.persistKey) } catch (e) { return }
        if (!text) return
        try {
            for (const [path, data] of Object.entries(JSON.parse(text))) {
                if (data === null) this.mkdirs(path)
                else {
                    const parts = this.split(path)
                    const dir = this.mkdirs(parts.slice(0, -1).join('/'))
                    const f = fileNode()
                    f.data = fromBase64(data)
                    f.size = f.data.length
                    dir.children.set(parts[parts.length - 1], f)
                }
            }
        } catch (e) { console.warn('Aurum: stored files unreadable', e) }
    }

    // ------------------------------------------------------------ WASI

    view() { return new DataView(this.memory.buffer) }
    bytes() { return new Uint8Array(this.memory.buffer) }
    str(ptr, len) { return new TextDecoder().decode(this.bytes().subarray(ptr, ptr + len)) }

    pathOf(fd, path) {
        const base = this.fds.get(fd)
        if (!base || !base.node || base.node.type !== 'dir') return null
        return {base: base.node, full: (base.path === '/' ? '' : base.path) + '/' + path}
    }

    fileStat(node, ptr) {
        const v = this.view()
        for (let i = 0; i < 64; i += 8) v.setBigUint64(ptr + i, 0n, true)
        v.setBigUint64(ptr + 8, node.ino, true)
        v.setUint8(ptr + 16, node.type === 'dir' ? FILETYPE_DIRECTORY : FILETYPE_REGULAR)
        v.setBigUint64(ptr + 24, 1n, true)
        v.setBigUint64(ptr + 32, BigInt(node.type === 'dir' ? 0 : node.size), true)
        v.setBigUint64(ptr + 40, node.mtime, true)
        v.setBigUint64(ptr + 48, node.mtime, true)
        v.setBigUint64(ptr + 56, node.mtime, true)
    }

    grow(node, size) {
        if (size <= node.data.length) return
        const bigger = new Uint8Array(Math.max(size, node.data.length * 2, 256))
        bigger.set(node.data.subarray(0, node.size))
        node.data = bigger
    }

    imports(getMemory) {
        const self = this
        const mem = () => { self.memory = getMemory(); return self }
        const imports = {
            args_get: () => E.SUCCESS,
            args_sizes_get(argc, argvBufSize) {
                const v = mem().view()
                v.setUint32(argc, 0, true)
                v.setUint32(argvBufSize, 0, true)
                return E.SUCCESS
            },
            environ_sizes_get(count, size) {
                const v = mem().view()
                v.setUint32(count, self.env.length, true)
                v.setUint32(size, self.env.reduce((n, s) => n + new TextEncoder().encode(s).length + 1, 0), true)
                return E.SUCCESS
            },
            environ_get(environ, buf) {
                const v = mem().view(), b = self.bytes()
                for (const s of self.env) {
                    const enc = new TextEncoder().encode(s)
                    v.setUint32(environ, buf, true)
                    environ += 4
                    b.set(enc, buf)
                    b[buf + enc.length] = 0
                    buf += enc.length + 1
                }
                return E.SUCCESS
            },
            clock_time_get(id, precision, out) {
                const t = id === 0 ? now() : BigInt(Math.round(performance.now() * 1e6))
                mem().view().setBigUint64(out, t, true)
                return E.SUCCESS
            },
            clock_res_get(id, out) {
                mem().view().setBigUint64(out, 1000n, true)
                return E.SUCCESS
            },
            random_get(buf, len) {
                crypto.getRandomValues(mem().bytes().subarray(buf, buf + len))
                return E.SUCCESS
            },
            proc_exit(code) { throw new ProcExit(code) },
            sched_yield: () => E.SUCCESS,
            poll_oneoff: () => E.NOTSUP,

            fd_prestat_get(fd, out) {
                const f = self.fds.get(fd)
                if (!f || !f.preopen) return E.BADF
                const v = mem().view()
                v.setUint8(out, 0)
                v.setUint32(out + 4, new TextEncoder().encode(f.preopen).length, true)
                return E.SUCCESS
            },
            fd_prestat_dir_name(fd, path, len) {
                const f = self.fds.get(fd)
                if (!f || !f.preopen) return E.BADF
                mem().bytes().set(new TextEncoder().encode(f.preopen).subarray(0, len), path)
                return E.SUCCESS
            },
            fd_fdstat_get(fd, out) {
                const v = mem().view()
                let type
                if (fd <= 2) type = FILETYPE_CHAR
                else {
                    const f = self.fds.get(fd)
                    if (!f) return E.BADF
                    type = f.node.type === 'dir' ? FILETYPE_DIRECTORY : FILETYPE_REGULAR
                }
                v.setUint8(out, type)
                v.setUint16(out + 2, 0, true)
                v.setBigUint64(out + 8, 0xffffffffffffffffn, true)
                v.setBigUint64(out + 16, 0xffffffffffffffffn, true)
                return E.SUCCESS
            },
            fd_fdstat_set_flags: () => E.SUCCESS,
            fd_filestat_get(fd, out) {
                const f = self.fds.get(fd)
                if (!f) return fd <= 2 ? E.SUCCESS : E.BADF
                mem().fileStat(f.node, out)
                return E.SUCCESS
            },
            fd_close(fd) {
                const f = self.fds.get(fd)
                if (!f || f.preopen) return f ? E.SUCCESS : E.BADF
                if (f.written) f.path ? self.changed(f.path) : self.changedNode()
                self.fds.delete(fd)
                return E.SUCCESS
            },
            fd_seek(fd, offset, whence, out) {
                const f = self.fds.get(fd)
                if (!f) return fd <= 2 ? E.SPIPE : E.BADF
                if (f.node.type === 'dir') return E.ISDIR
                const base = whence === 0 ? 0 : whence === 1 ? f.pos : f.node.size
                const pos = base + Number(offset)
                if (pos < 0) return E.INVAL
                f.pos = pos
                mem().view().setBigUint64(out, BigInt(pos), true)
                return E.SUCCESS
            },
            fd_tell(fd, out) {
                const f = self.fds.get(fd)
                if (!f) return E.BADF
                mem().view().setBigUint64(out, BigInt(f.pos), true)
                return E.SUCCESS
            },
            fd_read(fd, iovs, iovsLen, out) {
                const f = self.fds.get(fd)
                const v = mem().view(), b = self.bytes()
                if (!f) { v.setUint32(out, 0, true); return fd === 0 ? E.SUCCESS : E.BADF }
                if (f.node.type === 'dir') return E.ISDIR
                let n = 0
                for (let i = 0; i < iovsLen; ++i) {
                    const ptr = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true)
                    const k = Math.max(0, Math.min(len, f.node.size - f.pos))
                    b.set(f.node.data.subarray(f.pos, f.pos + k), ptr)
                    f.pos += k
                    n += k
                    if (k < len) break
                }
                v.setUint32(out, n, true)
                return E.SUCCESS
            },
            fd_pread(fd, iovs, iovsLen, offset, out) {
                const f = self.fds.get(fd)
                if (!f) return E.BADF
                const saved = f.pos
                f.pos = Number(offset)
                const r = imports.fd_read(fd, iovs, iovsLen, out)
                f.pos = saved
                return r
            },
            fd_write(fd, iovs, iovsLen, out) {
                const v = mem().view(), b = self.bytes()
                let n = 0
                if (fd === 1 || fd === 2) {
                    for (let i = 0; i < iovsLen; ++i) {
                        const ptr = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true)
                        self.stdout += new TextDecoder().decode(b.subarray(ptr, ptr + len))
                        n += len
                    }
                    let nl
                    while ((nl = self.stdout.indexOf('\n')) >= 0) {
                        console.log('[ui] ' + self.stdout.slice(0, nl))
                        self.stdout = self.stdout.slice(nl + 1)
                    }
                    v.setUint32(out, n, true)
                    return E.SUCCESS
                }
                const f = self.fds.get(fd)
                if (!f) return E.BADF
                if (f.node.type === 'dir') return E.ISDIR
                if (f.append) f.pos = f.node.size
                for (let i = 0; i < iovsLen; ++i) {
                    const ptr = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true)
                    self.grow(f.node, f.pos + len)
                    f.node.data.set(self.bytes().subarray(ptr, ptr + len), f.pos)
                    f.pos += len
                    f.node.size = Math.max(f.node.size, f.pos)
                    n += len
                }
                f.node.mtime = now()
                f.written = true
                self.view().setUint32(out, n, true)
                return E.SUCCESS
            },
            fd_sync: () => E.SUCCESS,
            fd_datasync: () => E.SUCCESS,
            fd_readdir(fd, buf, bufLen, cookie, out) {
                const f = self.fds.get(fd)
                const v = mem().view(), b = self.bytes()
                if (!f) return E.BADF
                if (f.node.type !== 'dir') return E.NOTDIR
                const entries = [...f.node.children.entries()]
                let used = 0
                for (let i = Number(cookie); i < entries.length; ++i) {
                    const [name, node] = entries[i]
                    const enc = new TextEncoder().encode(name)
                    const header = new Uint8Array(24)
                    const hv = new DataView(header.buffer)
                    hv.setBigUint64(0, BigInt(i + 1), true)
                    hv.setBigUint64(8, node.ino, true)
                    hv.setUint32(16, enc.length, true)
                    hv.setUint8(20, node.type === 'dir' ? FILETYPE_DIRECTORY : FILETYPE_REGULAR)
                    const entry = new Uint8Array(24 + enc.length)
                    entry.set(header)
                    entry.set(enc, 24)
                    const k = Math.min(entry.length, bufLen - used)
                    b.set(entry.subarray(0, k), buf + used)
                    used += k
                    if (used >= bufLen) break
                }
                v.setUint32(out, used, true)
                return E.SUCCESS
            },
            fd_renumber: () => E.NOTSUP,
            fd_advise: () => E.SUCCESS,
            fd_allocate: () => E.SUCCESS,
            fd_filestat_set_size(fd, size) {
                const f = self.fds.get(fd)
                if (!f || f.node.type === 'dir') return E.BADF
                const n = Number(size)
                self.grow(f.node, n)
                if (n > f.node.size) f.node.data.fill(0, f.node.size, n)
                f.node.size = n
                f.written = true
                return E.SUCCESS
            },
            fd_filestat_set_times: () => E.SUCCESS,

            path_open(dirfd, dirflags, pathPtr, pathLen, oflags, rightsBase, rightsInh, fdflags, out) {
                const p = self.pathOf(dirfd, mem().str(pathPtr, pathLen))
                if (!p) return E.BADF
                const r = self.resolve(p.base, mem().str(pathPtr, pathLen))
                if (r.error) return r.error
                let node = r.node
                if (node && (oflags & O_EXCL) && (oflags & O_CREAT)) return E.EXIST
                if (!node) {
                    if (!(oflags & O_CREAT)) return E.NOENT
                    node = fileNode()
                    r.parent.children.set(r.name, node)
                    self.changed(p.full)
                }
                if ((oflags & O_DIRECTORY) && node.type !== 'dir') return E.NOTDIR
                if (node.type === 'file' && (oflags & O_TRUNC)) {
                    node.size = 0
                    node.mtime = now()
                }
                const fd = self.nextFd++
                self.fds.set(fd, {node, pos: 0, append: !!(fdflags & FD_APPEND), path: p.full, written: !!(oflags & O_TRUNC)})
                self.view().setUint32(out, fd, true)
                return E.SUCCESS
            },
            path_filestat_get(dirfd, flags, pathPtr, pathLen, out) {
                const p = self.pathOf(dirfd, mem().str(pathPtr, pathLen))
                if (!p) return E.BADF
                const r = self.resolve(p.base, self.str(pathPtr, pathLen))
                if (r.error) return r.error
                if (!r.node) return E.NOENT
                self.fileStat(r.node, out)
                return E.SUCCESS
            },
            path_filestat_set_times: () => E.SUCCESS,
            path_create_directory(dirfd, pathPtr, pathLen) {
                const p = self.pathOf(dirfd, mem().str(pathPtr, pathLen))
                if (!p) return E.BADF
                const r = self.resolve(p.base, self.str(pathPtr, pathLen))
                if (r.error) return r.error
                if (r.node) return E.EXIST
                r.parent.children.set(r.name, dirNode())
                self.changed(p.full)
                return E.SUCCESS
            },
            path_remove_directory(dirfd, pathPtr, pathLen) {
                const p = self.pathOf(dirfd, mem().str(pathPtr, pathLen))
                if (!p) return E.BADF
                const r = self.resolve(p.base, self.str(pathPtr, pathLen))
                if (r.error) return r.error
                if (!r.node) return E.NOENT
                if (r.node.type !== 'dir') return E.NOTDIR
                if (r.node.children.size) return E.NOTEMPTY
                r.parent.children.delete(r.name)
                self.changed(p.full)
                return E.SUCCESS
            },
            path_unlink_file(dirfd, pathPtr, pathLen) {
                const p = self.pathOf(dirfd, mem().str(pathPtr, pathLen))
                if (!p) return E.BADF
                const r = self.resolve(p.base, self.str(pathPtr, pathLen))
                if (r.error) return r.error
                if (!r.node) return E.NOENT
                if (r.node.type === 'dir') return E.ISDIR
                r.parent.children.delete(r.name)
                self.changed(p.full)
                return E.SUCCESS
            },
            path_rename(fd, oldPtr, oldLen, newFd, newPtr, newLen) {
                const po = self.pathOf(fd, mem().str(oldPtr, oldLen)), pn = self.pathOf(newFd, self.str(newPtr, newLen))
                if (!po || !pn) return E.BADF
                const a = self.resolve(po.base, self.str(oldPtr, oldLen))
                const b = self.resolve(pn.base, self.str(newPtr, newLen))
                if (a.error) return a.error
                if (b.error) return b.error
                if (!a.node) return E.NOENT
                if (b.node && b.node.type === 'dir' && b.node.children.size) return E.NOTEMPTY
                a.parent.children.delete(a.name)
                b.parent.children.set(b.name, a.node)
                self.changed(po.full)
                self.changed(pn.full)
                return E.SUCCESS
            },
            path_readlink: () => E.INVAL,
            path_symlink: () => E.NOTSUP,
            path_link: () => E.NOTSUP,
            sock_accept: () => E.NOTSUP,
            sock_recv: () => E.NOTSUP,
            sock_send: () => E.NOTSUP,
            sock_shutdown: () => E.NOTSUP,
        }
        return imports
    }
}

function toBase64(bytes) {
    let s = ''
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
    return btoa(s)
}

function fromBase64(text) {
    const s = atob(text)
    const out = new Uint8Array(s.length)
    for (let i = 0; i < s.length; ++i) out[i] = s.charCodeAt(i)
    return out
}
