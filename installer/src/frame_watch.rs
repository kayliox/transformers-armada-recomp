// frame_watch.rs - (TODO 421, a testing aid) TF_SETUP_FRAMELOG=<file>: once a second, how many frames the window drew,
// how much CPU its UI thread used and whether Windows calls the window hung (IsHungAppWindow: no messages taken for 5 s).
// Tells a window that stops drawing (the event loop not running) from one that draws but is not shown.
use std::io::Write;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

static FRAMES: AtomicU64 = AtomicU64::new(0);
static UI_THREAD: AtomicU32 = AtomicU32::new(0);

type H = *mut std::ffi::c_void;
#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentThreadId() -> u32;
    fn OpenThread(access: u32, inherit: i32, id: u32) -> H;
    fn GetThreadTimes(thread: H, c: *mut u64, e: *mut u64, k: *mut u64, u: *mut u64) -> i32;
    fn SuspendThread(thread: H) -> u32;
    fn ResumeThread(thread: H) -> u32;
    fn GetThreadContext(thread: H, context: *mut Context) -> i32;
    fn RtlLookupFunctionEntry(pc: u64, image_base: *mut u64, history: H) -> H;
    fn RtlVirtualUnwind(kind: u32, image_base: u64, pc: u64, entry: H, context: *mut Context, handler_data: *mut H,
                        establisher: *mut u64, pointers: H) -> H;
    fn GetModuleHandleExW(flags: u32, address: *const u8, module: *mut H) -> i32;
    fn GetModuleFileNameW(module: H, name: *mut u16, size: u32) -> u32;
}

/// The x64 CONTEXT (1232 bytes, 16-aligned): ContextFlags at 0x30, Rsp 0x98, Rip 0xF8.
#[repr(C, align(16))]
struct Context([u8; 1232]);
impl Context {
    fn get(&self, at: usize) -> u64 { u64::from_le_bytes(self.0[at..at + 8].try_into().unwrap()) }
    fn set(&mut self, at: usize, v: u64) { self.0[at..at + 8].copy_from_slice(&v.to_le_bytes()); }
}

/// The UI thread's return addresses, walked while it is suspended (x64 unwind data; a leaf without any: its return
/// address on the stack). Named after the walk, with the thread running again.
fn ui_stack(thread_id: u32) -> Vec<u64> {
    let mut pcs = Vec::new();
    // THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION
    let thread = unsafe { OpenThread(0x0002 | 0x0008 | 0x0040, 0, thread_id) };
    if thread.is_null() {
        return pcs;
    }
    unsafe {
        if SuspendThread(thread) != u32::MAX {
            let mut ctx = Box::new(Context([0; 1232]));
            ctx.0[0x30..0x34].copy_from_slice(&0x0010_000Bu32.to_le_bytes()); // CONTEXT_FULL (AMD64)
            if GetThreadContext(thread, &mut *ctx) != 0 {
                for _ in 0..48 {
                    let pc = ctx.get(0xF8);
                    if pc == 0 {
                        break;
                    }
                    pcs.push(pc);
                    let mut base = 0u64;
                    let entry = RtlLookupFunctionEntry(pc, &mut base, std::ptr::null_mut());
                    if entry.is_null() {
                        let rsp = ctx.get(0x98);
                        if rsp == 0 {
                            break;
                        }
                        ctx.set(0xF8, *(rsp as *const u64));
                        ctx.set(0x98, rsp + 8);
                    } else {
                        let (mut data, mut frame) = (std::ptr::null_mut(), 0u64);
                        RtlVirtualUnwind(0, base, pc, entry, &mut *ctx, &mut data, &mut frame, std::ptr::null_mut());
                    }
                }
            }
            ResumeThread(thread);
        }
    }
    pcs
}

/// "module.dll!export+0x12" (the nearest export below it) or "module+0x1234".
fn name(pc: u64) -> String {
    unsafe {
        let mut module: H = std::ptr::null_mut();
        if GetModuleHandleExW(0x4 | 0x2, pc as *const u8, &mut module) == 0 { // FROM_ADDRESS | UNCHANGED_REFCOUNT
            return format!("{pc:#x}");
        }
        let mut buf = [0u16; 260];
        let n = GetModuleFileNameW(module, buf.as_mut_ptr(), 260) as usize;
        let path = String::from_utf16_lossy(&buf[..n]);
        let file = path.rsplit('\\').next().unwrap_or(&path).to_string();
        let base = module as u64;
        let rva = (pc - base) as u32;
        let b = base as *const u8;
        let rd32 = |off: u32| -> u32 { std::ptr::read_unaligned(b.add(off as usize) as *const u32) };
        let rd16 = |off: u32| -> u16 { std::ptr::read_unaligned(b.add(off as usize) as *const u16) };
        let pe = rd32(0x3C);
        let (dir_rva, dir_size) = (rd32(pe + 0x18 + 0x70), rd32(pe + 0x18 + 0x74)); // PE32+ export directory
        let mut best: Option<(u32, String)> = None;
        if dir_rva != 0 && dir_size != 0 {
            let (names, funcs, ords, count) = (rd32(dir_rva + 0x20), rd32(dir_rva + 0x1C), rd32(dir_rva + 0x24), rd32(dir_rva + 0x18));
            for i in 0..count.min(20000) {
                let f = rd32(funcs + 4 * rd16(ords + 2 * i) as u32);
                if f <= rva && best.as_ref().is_none_or(|(at, _)| f > *at) {
                    let np = b.add(rd32(names + 4 * i) as usize);
                    let len = (0..200).position(|k| *np.add(k) == 0).unwrap_or(0);
                    best = Some((f, String::from_utf8_lossy(std::slice::from_raw_parts(np, len)).into_owned()));
                }
            }
        }
        match best {
            Some((at, export)) if rva - at < 0x10000 => format!("{file}!{export}+{:#x}", rva - at),
            _ => format!("{file}+{rva:#x}"),
        }
    }
}
#[link(name = "user32")]
unsafe extern "system" {
    fn EnumThreadWindows(thread: u32, f: unsafe extern "system" fn(isize, isize) -> i32, l: isize) -> i32;
    fn IsWindowVisible(w: isize) -> i32;
    fn IsHungAppWindow(w: isize) -> i32;
}

unsafe extern "system" fn first_visible(w: isize, out: isize) -> i32 {
    if unsafe { IsWindowVisible(w) } != 0 {
        unsafe { *(out as *mut isize) = w };
        return 0;
    }
    1
}

/// Each frame (App::ui).
pub fn frame() {
    FRAMES.fetch_add(1, Ordering::Relaxed);
    if UI_THREAD.load(Ordering::Relaxed) == 0 {
        UI_THREAD.store(unsafe { GetCurrentThreadId() }, Ordering::Relaxed);
    }
}

/// At start: the watch, when TF_SETUP_FRAMELOG names a file.
pub fn start() {
    let Some(path) = std::env::var_os("TF_SETUP_FRAMELOG").filter(|p| !p.is_empty()) else { return };
    std::thread::spawn(move || {
        let Ok(mut out) = std::fs::File::create(&path) else { return };
        let start = std::time::Instant::now();
        let (mut last_frames, mut last_cpu, mut thread, mut stalled) = (0u64, 0u64, std::ptr::null_mut::<std::ffi::c_void>(), 0u32);
        loop {
            std::thread::sleep(std::time::Duration::from_secs(1));
            let tid = UI_THREAD.load(Ordering::Relaxed);
            if thread.is_null() && tid != 0 {
                thread = unsafe { OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, 0, tid) };
            }
            let (mut c, mut e, mut k, mut u) = (0u64, 0u64, 0u64, 0u64);
            let cpu = if !thread.is_null() && unsafe { GetThreadTimes(thread, &mut c, &mut e, &mut k, &mut u) } != 0 { (k + u) / 10_000 } else { 0 };
            let mut window = 0isize;
            if tid != 0 {
                unsafe { EnumThreadWindows(tid, first_visible, &mut window as *mut isize as isize) };
            }
            let hung = window != 0 && unsafe { IsHungAppWindow(window) } != 0;
            let frames = FRAMES.load(Ordering::Relaxed);
            let _ = writeln!(out, "{:7.1} s frames {} (+{}) ui-cpu {} ms (+{}) hung {}", start.elapsed().as_secs_f32(), frames,
                             frames - last_frames, cpu, cpu.saturating_sub(last_cpu), hung as u8);
            // (no frame for 3 s while a step runs: where the UI thread is, then every 60 s of the same stall)
            stalled = if frames == last_frames { stalled + 1 } else { 0 };
            if tid != 0 && (stalled == 3 || (stalled > 3 && stalled % 60 == 3)) {
                let _ = writeln!(out, "  the UI thread, stalled {stalled} s:");
                for pc in ui_stack(tid) {
                    let _ = writeln!(out, "    {}", name(pc));
                }
            }
            let _ = out.flush();
            (last_frames, last_cpu) = (frames, cpu);
        }
    });
}
