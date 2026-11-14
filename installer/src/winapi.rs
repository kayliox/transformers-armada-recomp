// winapi.rs - the few Windows API calls the setup makes itself, in place of helper programs it used
// to start (taskkill, a PowerShell one-liner, Wait-Process, cmd, explorer): a job object that holds
// the steps' process tree (so Cancel stops all of it), waiting for a process, the PATH as the
// registry has it, opening a folder. Plain kernel32/advapi32/shell32 functions, no extra crates.
use std::ffi::{OsStr, c_void};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::io::AsRawHandle;
use std::path::Path;

type Handle = *mut c_void;

#[repr(C)]
#[derive(Default)]
struct BasicLimit {
    per_process_user_time_limit: i64,
    per_job_user_time_limit: i64,
    limit_flags: u32,
    minimum_working_set_size: usize,
    maximum_working_set_size: usize,
    active_process_limit: u32,
    affinity: usize,
    priority_class: u32,
    scheduling_class: u32,
}

#[repr(C)]
#[derive(Default)]
struct IoCounters {
    counts: [u64; 6],
}

#[repr(C)]
#[derive(Default)]
struct ExtendedLimit {
    basic: BasicLimit,
    io: IoCounters,
    process_memory_limit: usize,
    job_memory_limit: usize,
    peak_process_memory_used: usize,
    peak_job_memory_used: usize,
}

const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: i32 = 9;
const JOB_OBJECT_LIMIT_BREAKAWAY_OK: u32 = 0x0000_0800;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x0000_2000;
const JOB_OBJECT_BASIC_PROCESS_ID_LIST: i32 = 3;
const SYNCHRONIZE: u32 = 0x0010_0000;
const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x0000_1000;
const INFINITE: u32 = 0xFFFF_FFFF;
const PROCESS_TERMINATE: u32 = 0x0000_0001;
const HKEY_CURRENT_USER: isize = 0x8000_0001u32 as i32 as isize;
const HKEY_LOCAL_MACHINE: isize = 0x8000_0002u32 as i32 as isize;
const RRF_RT_REG_SZ_OR_EXPAND_SZ: u32 = 0x0000_0002 | 0x0000_0004;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateJobObjectW(attributes: *mut c_void, name: *const u16) -> Handle;
    fn SetInformationJobObject(job: Handle, class: i32, info: *mut c_void, length: u32) -> i32;
    fn AssignProcessToJobObject(job: Handle, process: Handle) -> i32;
    fn TerminateJobObject(job: Handle, exit_code: u32) -> i32;
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> Handle;
    fn TerminateProcess(process: Handle, exit_code: u32) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
    fn CloseHandle(handle: Handle) -> i32;
    fn QueryInformationJobObject(job: Handle, class: i32, info: *mut c_void, length: u32, returned: *mut u32) -> i32;
    fn GetExitCodeProcess(process: Handle, code: *mut u32) -> i32;
    fn IsProcessInJob(process: Handle, job: Handle, result: *mut i32) -> i32;
    fn QueryFullProcessImageNameW(process: Handle, flags: u32, name: *mut u16, size: *mut u32) -> i32;
    fn GetProcessTimes(process: Handle, creation: *mut u64, exit: *mut u64, kernel: *mut u64, user: *mut u64) -> i32;
}

#[link(name = "advapi32")]
unsafe extern "system" {
    fn RegGetValueW(key: isize, sub_key: *const u16, value: *const u16, flags: u32, kind: *mut u32, data: *mut c_void, size: *mut u32) -> i32;
}

#[link(name = "user32")]
unsafe extern "system" {
    fn GetAsyncKeyState(key: i32) -> i16;
    fn AllowSetForegroundWindow(pid: u32) -> i32;
    fn MessageBoxW(window: Handle, text: *const u16, caption: *const u16, kind: u32) -> i32;
}

/// The Shift key held down right now (a launcher's start with Shift held: the setup's window instead of the game).
/// (TODO 290) Lets the programs a step starts bring their windows forward - Windows' permission prompt (UAC) for an
/// installer the tools step starts came up behind the setup's window, or only as a flashing taskbar button, and the
/// step sat at "Starting package install" for minutes.
pub fn allow_foreground() {
    const ASFW_ANY: u32 = 0xFFFF_FFFF;
    unsafe { AllowSetForegroundWindow(ASFW_ANY) };
}

/// (TODO 371) Windows 11's Smart App Control: HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy, VerifiedAndReputablePolicyState
/// (Microsoft's "Test app signatures with Smart App Control": 0 off, 1 on, 2 evaluation). None: not there (older Windows).
pub fn smart_app_control_state() -> Option<u32> {
    const RRF_RT_REG_DWORD: u32 = 0x0000_0010;
    let (key, value) = (wide(r"SYSTEM\CurrentControlSet\Control\CI\Policy"), wide("VerifiedAndReputablePolicyState"));
    let mut data = 0u32;
    let mut size = std::mem::size_of::<u32>() as u32;
    let status = unsafe {
        RegGetValueW(HKEY_LOCAL_MACHINE, key.as_ptr(), value.as_ptr(), RRF_RT_REG_DWORD, std::ptr::null_mut(),
                     (&mut data as *mut u32).cast(), &mut size)
    };
    (status == 0).then_some(data)
}

/// A plain message box (no owner window), for a launcher's start that has no window of its own.
pub fn message_box(caption: &str, text: &str) {
    const MB_ICONWARNING: u32 = 0x0000_0030;
    const MB_SETFOREGROUND: u32 = 0x0001_0000;
    let (text, caption) = (wide(text), wide(caption));
    unsafe { MessageBoxW(std::ptr::null_mut(), text.as_ptr(), caption.as_ptr(), MB_ICONWARNING | MB_SETFOREGROUND) };
}

pub fn shift_held() -> bool {
    const VK_SHIFT: i32 = 0x10;
    unsafe { GetAsyncKeyState(VK_SHIFT) < 0 }
}

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteW(hwnd: *mut c_void, operation: *const u16, file: *const u16, parameters: *const u16, directory: *const u16, show: i32) -> isize;
}

fn wide(s: impl AsRef<OsStr>) -> Vec<u16> {
    s.as_ref().encode_wide().chain(std::iter::once(0)).collect()
}

/// A process and everything it starts, in one job object. Cancel (`terminate`) ends the whole tree.
/// With `kill_on_close` the tree also ends when the job is dropped (the step finished, or the setup
/// closed); without it what is left keeps running (the game an update starts again at its end).
pub struct ProcessTree {
    job: Handle, // null: the process could not be put in a job (then only it is ended)
    pid: u32,
}

// (the handle is only used through the job functions, which are thread-safe)
unsafe impl Send for ProcessTree {}
unsafe impl Sync for ProcessTree {}

impl ProcessTree {
    pub fn new(child: &std::process::Child, kill_on_close: bool) -> Self {
        let pid = child.id();
        unsafe {
            let job = CreateJobObjectW(std::ptr::null_mut(), std::ptr::null());
            if job.is_null() {
                return Self { job, pid };
            }
            let mut info = ExtendedLimit::default();
            // (BREAKAWAY_OK: a process that asks to leave the job may: the game starts its updater so)
            info.basic.limit_flags = JOB_OBJECT_LIMIT_BREAKAWAY_OK | if kill_on_close { JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE } else { 0 };
            let ok = SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, (&mut info as *mut ExtendedLimit).cast(),
                                             std::mem::size_of::<ExtendedLimit>() as u32) != 0
                && AssignProcessToJobObject(job, child.as_raw_handle()) != 0;
            if !ok {
                CloseHandle(job);
                return Self { job: std::ptr::null_mut(), pid };
            }
            Self { job, pid }
        }
    }

    /// The processes in the job now (empty when it is not a job): what the first one started and did not let leave
    /// the job - the game started again by itself, a setting's or a mod's restart.
    pub fn processes(&self) -> Vec<u32> {
        if self.job.is_null() {
            return Vec::new();
        }
        // JOBOBJECT_BASIC_PROCESS_ID_LIST: two u32 counts, then the ids (ULONG_PTR each)
        let mut buffer = vec![0usize; 2 + 64];
        unsafe {
            let ok = QueryInformationJobObject(self.job, JOB_OBJECT_BASIC_PROCESS_ID_LIST, buffer.as_mut_ptr().cast(),
                                               (buffer.len() * std::mem::size_of::<usize>()) as u32, std::ptr::null_mut()) != 0;
            if !ok {
                return Vec::new();
            }
            let listed = (*(buffer.as_ptr().cast::<u32>().add(1))) as usize;
            buffer[1..1 + listed.min(64)].iter().map(|&pid| pid as u32).collect()
        }
    }

    /// (TODO 370) Waits for job member `pid` to end: its exit code. None: it has gone, or the id names a process that is not
    /// in the job - Windows gives an id again soon after its process ends, and waiting on the stranger took ITS exit code (the
    /// launcher test's 109 on the release runner) or would wait for a process that has nothing to do with the game. The
    /// process is checked once it is open, so its id cannot be given again while it is waited for.
    pub fn wait_for_member(&self, pid: u32) -> Option<i32> {
        if self.job.is_null() {
            return None;
        }
        unsafe {
            let process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if process.is_null() {
                return None;
            }
            let mut member = 0i32;
            if IsProcessInJob(process, self.job, &mut member) == 0 || member == 0 {
                CloseHandle(process);
                return None;
            }
            WaitForSingleObject(process, INFINITE);
            let mut code = 0u32;
            let ok = GetExitCodeProcess(process, &mut code) != 0;
            CloseHandle(process);
            ok.then_some(code as i32)
        }
    }

    /// Ends the process and everything it started (or only the process, when it is not in a job).
    pub fn terminate(&self) {
        unsafe {
            if !self.job.is_null() {
                TerminateJobObject(self.job, 1);
                return;
            }
            let process = OpenProcess(PROCESS_TERMINATE, 0, self.pid);
            if !process.is_null() {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        }
    }
}

impl Drop for ProcessTree {
    fn drop(&mut self) {
        if !self.job.is_null() {
            unsafe { CloseHandle(self.job) };
        }
    }
}

/// Waits for a process to end and gives its exit code (None when it cannot be opened: already gone).
pub fn wait_for_exit(pid: u32) -> Option<i32> {
    unsafe {
        let process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if process.is_null() {
            return None;
        }
        WaitForSingleObject(process, INFINITE);
        let mut code = 0u32;
        let ok = GetExitCodeProcess(process, &mut code) != 0;
        CloseHandle(process);
        ok.then_some(code as i32)
    }
}

/// Whether a process is the console host Windows starts for a console program (it ends with the last one).
pub fn is_console_host(pid: u32) -> bool {
    unsafe {
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if process.is_null() {
            return false;
        }
        let mut name = vec![0u16; 1024];
        let mut size = name.len() as u32;
        let ok = QueryFullProcessImageNameW(process, 0, name.as_mut_ptr(), &mut size) != 0;
        CloseHandle(process);
        ok && String::from_utf16_lossy(&name[..size as usize]).to_ascii_lowercase().ends_with("\\conhost.exe")
    }
}

/// (TODO 289) A running process's start time (FILETIME ticks): with its id it names the process, an id alone can be
/// given to another process once it has ended. None: not running (or not ours to ask).
pub fn process_started(pid: u32) -> Option<u64> {
    unsafe {
        let process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if process.is_null() {
            return None;
        }
        let running = WaitForSingleObject(process, 0) != 0; // (0: signalled = it has ended)
        let (mut creation, mut exit, mut kernel, mut user) = (0u64, 0u64, 0u64, 0u64);
        let ok = GetProcessTimes(process, &mut creation, &mut exit, &mut kernel, &mut user) != 0;
        CloseHandle(process);
        (ok && running).then_some(creation)
    }
}

/// Waits up to `timeout_ms` for a process to end (at once when it is not there).
pub fn wait_for_process(pid: u32, timeout_ms: u32) {
    unsafe {
        let process = OpenProcess(SYNCHRONIZE, 0, pid);
        if !process.is_null() {
            WaitForSingleObject(process, timeout_ms);
            CloseHandle(process);
        }
    }
}

/// A REG_SZ / REG_EXPAND_SZ value (environment variables expanded), or None.
fn registry_string(root: isize, key: &str, value: &str) -> Option<String> {
    let (key, value) = (wide(key), wide(value));
    let mut size = 0u32;
    unsafe {
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ_OR_EXPAND_SZ, std::ptr::null_mut(), std::ptr::null_mut(), &mut size) != 0 {
            return None;
        }
        // (an expanded value can be longer than the stored one: room for that)
        let mut buffer = vec![0u16; (size as usize / 2).max(1) + 4096];
        let mut bytes = (buffer.len() * 2) as u32;
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_REG_SZ_OR_EXPAND_SZ, std::ptr::null_mut(), buffer.as_mut_ptr().cast(), &mut bytes) != 0 {
            return None;
        }
        let len = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
        Some(String::from_utf16_lossy(&buffer[..len]))
    }
}

/// PATH as the registry has it now (the machine's, then the user's): programs installed since this
/// process started are found without a restart.
pub fn registry_path() -> String {
    let machine = registry_string(HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment", "Path");
    let user = registry_string(HKEY_CURRENT_USER, "Environment", "Path");
    [machine, user].into_iter().flatten().collect::<Vec<_>>().join(";")
}

/// Opens a folder in Explorer.
pub fn open_folder(folder: &Path) {
    let (op, file) = (wide("open"), wide(folder.as_os_str()));
    unsafe {
        ShellExecuteW(std::ptr::null_mut(), op.as_ptr(), file.as_ptr(), std::ptr::null(), std::ptr::null(), 1);
    }
}

/// A value of any type as its raw bytes, or None.
fn registry_bytes(root: isize, key: &str, value: &str) -> Option<Vec<u8>> {
    const RRF_RT_ANY: u32 = 0x0000_ffff;
    let (key, value) = (wide(key), wide(value));
    let mut size = 0u32;
    unsafe {
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_ANY, std::ptr::null_mut(), std::ptr::null_mut(), &mut size) != 0 || size == 0 {
            return None;
        }
        let mut buffer = vec![0u8; size as usize];
        if RegGetValueW(root, key.as_ptr(), value.as_ptr(), RRF_RT_ANY, std::ptr::null_mut(), buffer.as_mut_ptr().cast(), &mut size) != 0 {
            return None;
        }
        buffer.truncate(size as usize);
        Some(buffer)
    }
}

/// The graphics cards Windows knows of: (name, video memory in bytes; 0 when it does not say). From the
/// display adapters' driver keys: the 64-bit size where the driver writes one (the 32-bit one stops at 4 GB).
pub fn graphics_cards() -> Vec<(String, u64)> {
    let class = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";
    let mut cards = Vec::new();
    for i in 0..16 {
        let key = format!(r"{class}\{i:04}");
        let Some(name) = registry_string(HKEY_LOCAL_MACHINE, &key, "DriverDesc") else { continue };
        let size = |b: Vec<u8>| -> u64 {
            let mut v = [0u8; 8];
            let n = b.len().min(8);
            v[..n].copy_from_slice(&b[..n]);
            u64::from_le_bytes(v)
        };
        let vram = registry_bytes(HKEY_LOCAL_MACHINE, &key, "HardwareInformation.qwMemorySize").map(size)
            .or_else(|| registry_bytes(HKEY_LOCAL_MACHINE, &key, "HardwareInformation.MemorySize").map(size))
            .unwrap_or(0);
        cards.push((name, vram));
    }
    cards
}
