//! 2ksbox's agent in a Windows 11 guest (track M23, doc 24 §4): the
//! clipboard, shared with the host through QEMU's `qemu-vdagent`.
//!
//! QEMU speaks the SPICE agent protocol on a virtio-serial port named
//! `com.redhat.spice.0` (Windows: `\\.\Global\com.redhat.spice.0`, from
//! virtio-win's `vioser` driver). Messages ride in chunks, each a port
//! number and a size, then the message: protocol 1, type, an opaque
//! word, a size and the data. This agent announces only
//! CLIPBOARD_BY_DEMAND, so the clipboard messages carry no selection
//! byte and no serial:
//!
//! - the host's text: QEMU sends GRAB(types); the agent answers
//!   REQUEST(UTF8_TEXT); QEMU sends CLIPBOARD(type, text), which goes on
//!   the Windows clipboard;
//! - the guest's text: Windows says the clipboard changed; the agent sends
//!   GRAB(UTF8_TEXT); QEMU answers REQUEST, and the agent sends the text.
//!
//! A change the agent made itself (its clipboard sequence number) is not
//! sent back. Windows' text has CRLF line ends, the host's LF.
//! It logs to C:\2KSBOX\agent.log. One instance per user session.
//!
//! vioser lets only SYSTEM and Administrators open the port, so this
//! runs with the user's elevated token (a logon task with highest
//! privileges, `install.ps1`). A drive mapped from that token's session
//! is invisible to Explorer's, so `--map`, run by a second, unelevated
//! logon task, maps the host's shared folder (`\\10.0.2.4\host`, served
//! by the player when the machine has one) and exits.
//!
//! Once the desktop is up it also makes the virtio-gpu's screen the only
//! one (track M24, `virtio_screen_only`).

#![windows_subsystem = "windows"]

use std::ffi::c_void;
use std::io::Write;
use std::sync::atomic::{AtomicIsize, AtomicU32, Ordering};
use std::sync::Mutex;
use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Storage::FileSystem::*;
use windows_sys::Win32::System::DataExchange::*;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::System::Memory::*;
use windows_sys::Win32::System::Threading::*;
use windows_sys::Win32::System::IO::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

const PORT: &str = r"\\.\Global\com.redhat.spice.0";
/// The shared folder, and the account the player's server takes (doc 24 §2.1).
const SHARE: &str = r"\\10.0.2.4\host";
const SHARE_USER: &str = "2ksbox";
const SHARE_PASSWORD: &str = "2ksbox";
const CF_UNICODETEXT: u32 = 13;

const VD_AGENT_PROTOCOL: u32 = 1;
const VD_AGENT_CLIPBOARD: u32 = 4;
const VD_AGENT_ANNOUNCE_CAPABILITIES: u32 = 6;
const VD_AGENT_CLIPBOARD_GRAB: u32 = 7;
const VD_AGENT_CLIPBOARD_REQUEST: u32 = 8;
const VD_AGENT_CLIPBOARD_RELEASE: u32 = 9;
const VD_AGENT_CAP_CLIPBOARD_BY_DEMAND: u32 = 5;
const VD_AGENT_CLIPBOARD_UTF8_TEXT: u32 = 1;
const VDP_CLIENT_PORT: u32 = 1;

const WM_AGENT_MSG: u32 = WM_APP + 1; // lparam: Box<(u32, Vec<u8>)>
const WM_AGENT_UP: u32 = WM_APP + 2; // the port opened

/// The open port, or 0.
static PORT_HANDLE: AtomicIsize = AtomicIsize::new(0);
/// The clipboard sequence number right after the agent set it.
static OWN_SEQ: AtomicU32 = AtomicU32::new(0);
/// The guest's text the agent last offered (UTF-8, LF).
static OFFERED: Mutex<Option<String>> = Mutex::new(None);
static LOG: Mutex<Option<std::fs::File>> = Mutex::new(None);

fn log(msg: &str) {
    // one write per line: the elevated agent and `--map` share the file
    if let Some(f) = LOG.lock().unwrap().as_mut() {
        let _ = f.write_all(format!("{}\r\n", msg).as_bytes());
    }
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

// --- the port ---------------------------------------------------------------

/// Overlapped I/O on the port: the reader thread waits in ReadFile while
/// the window thread writes, which a synchronous handle would serialize.
fn io(h: HANDLE, buf: *mut u8, len: u32, write: bool) -> Option<u32> {
    unsafe {
        let ev = CreateEventW(std::ptr::null(), 1, 0, std::ptr::null());
        let mut ov: OVERLAPPED = std::mem::zeroed();
        ov.hEvent = ev;
        let mut n = 0u32;
        let ok = if write {
            WriteFile(h, buf, len, &mut n, &mut ov)
        } else {
            ReadFile(h, buf, len, &mut n, &mut ov)
        };
        let r = if ok != 0 || GetLastError() == ERROR_IO_PENDING {
            if GetOverlappedResult(h, &ov, &mut n, 1) != 0 {
                Some(n)
            } else {
                None
            }
        } else {
            None
        };
        CloseHandle(ev);
        r
    }
}

fn send(ty: u32, data: &[u8]) {
    let h = PORT_HANDLE.load(Ordering::Acquire);
    if h == 0 {
        return;
    }
    let mut m = Vec::with_capacity(28 + data.len());
    let size = 20 + data.len() as u32;
    m.extend_from_slice(&VDP_CLIENT_PORT.to_le_bytes());
    m.extend_from_slice(&size.to_le_bytes());
    m.extend_from_slice(&VD_AGENT_PROTOCOL.to_le_bytes());
    m.extend_from_slice(&ty.to_le_bytes());
    m.extend_from_slice(&0u64.to_le_bytes());
    m.extend_from_slice(&(data.len() as u32).to_le_bytes());
    m.extend_from_slice(data);
    let mut off = 0;
    while off < m.len() {
        match io(h as HANDLE, m[off..].as_mut_ptr(), (m.len() - off) as u32, true) {
            Some(n) if n > 0 => off += n as usize,
            _ => {
                log("write to the port failed");
                return;
            }
        }
    }
}

fn caps(request: bool) {
    let mut d = Vec::new();
    d.extend_from_slice(&(request as u32).to_le_bytes());
    d.extend_from_slice(&(1u32 << VD_AGENT_CAP_CLIPBOARD_BY_DEMAND).to_le_bytes());
    send(VD_AGENT_ANNOUNCE_CAPABILITIES, &d);
}

/// The reader thread: open the port (it appears with vioser, and may not
/// be there yet), then reassemble chunks into messages for the window.
fn reader(hwnd: usize) {
    let mut last_err = 0;
    loop {
        let name = wide(PORT);
        let h = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                std::ptr::null_mut(),
            )
        };
        if h == INVALID_HANDLE_VALUE {
            let err = unsafe { GetLastError() };
            if err != last_err {
                log(&format!("no port yet ({} error {})", PORT, err));
                last_err = err;
            }
            std::thread::sleep(std::time::Duration::from_secs(2));
            continue;
        }
        log("port open");
        PORT_HANDLE.store(h as isize, Ordering::Release);
        unsafe { PostMessageW(hwnd as HWND, WM_AGENT_UP, 0, 0) };
        let mut pending: Vec<u8> = Vec::new(); // bytes not yet a whole chunk
        let mut msg: Vec<u8> = Vec::new(); // a message spread over chunks
        let mut buf = vec![0u8; 65536];
        loop {
            let Some(n) = io(h, buf.as_mut_ptr(), buf.len() as u32, false) else { break };
            pending.extend_from_slice(&buf[..n as usize]);
            while pending.len() >= 8 {
                let size = u32::from_le_bytes(pending[4..8].try_into().unwrap()) as usize;
                if pending.len() < 8 + size {
                    break;
                }
                msg.extend_from_slice(&pending[8..8 + size]);
                pending.drain(..8 + size);
                if msg.len() >= 20 {
                    let len = u32::from_le_bytes(msg[16..20].try_into().unwrap()) as usize;
                    if msg.len() >= 20 + len {
                        let ty = u32::from_le_bytes(msg[4..8].try_into().unwrap());
                        let data = msg[20..20 + len].to_vec();
                        msg.clear();
                        let b = Box::into_raw(Box::new((ty, data)));
                        unsafe { PostMessageW(hwnd as HWND, WM_AGENT_MSG, 0, b as isize) };
                    }
                }
            }
        }
        log("port closed; reopening");
        PORT_HANDLE.store(0, Ordering::Release);
        unsafe { CloseHandle(h) };
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}

// --- the Windows clipboard -----------------------------------------------------

fn open_clipboard(hwnd: HWND) -> bool {
    // another program may hold it a moment
    for _ in 0..20 {
        if unsafe { OpenClipboard(hwnd) } != 0 {
            return true;
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
    }
    false
}

fn get_text(hwnd: HWND) -> Option<String> {
    if !open_clipboard(hwnd) {
        return None;
    }
    let mut out = None;
    unsafe {
        let h = GetClipboardData(CF_UNICODETEXT);
        if !h.is_null() {
            let p = GlobalLock(h) as *const u16;
            if !p.is_null() {
                let mut n = 0;
                while *p.add(n) != 0 {
                    n += 1;
                }
                out = Some(String::from_utf16_lossy(std::slice::from_raw_parts(p, n)));
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    out.map(|s| s.replace("\r\n", "\n"))
}

fn set_text(hwnd: HWND, text: &str) {
    let crlf = text.replace("\r\n", "\n").replace('\n', "\r\n");
    let w = wide(&crlf);
    if !open_clipboard(hwnd) {
        log("clipboard busy; text dropped");
        return;
    }
    unsafe {
        EmptyClipboard();
        let g = GlobalAlloc(GMEM_MOVEABLE, w.len() * 2);
        if !g.is_null() {
            let p = GlobalLock(g) as *mut u16;
            std::ptr::copy_nonoverlapping(w.as_ptr(), p, w.len());
            GlobalUnlock(g);
            if SetClipboardData(CF_UNICODETEXT, g).is_null() {
                GlobalFree(g);
            }
        }
        CloseClipboard();
        OWN_SEQ.store(GetClipboardSequenceNumber(), Ordering::Release);
    }
}

// --- the window ------------------------------------------------------------------

fn on_message(hwnd: HWND, ty: u32, data: &[u8]) {
    let word = |i: usize| data.get(4 * i..4 * i + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    match ty {
        VD_AGENT_ANNOUNCE_CAPABILITIES => {
            if word(0) == Some(1) {
                caps(false);
            }
            log("host capabilities");
        }
        VD_AGENT_CLIPBOARD_GRAB => {
            let n = data.len() / 4;
            if (0..n).any(|i| word(i) == Some(VD_AGENT_CLIPBOARD_UTF8_TEXT)) {
                send(VD_AGENT_CLIPBOARD_REQUEST, &VD_AGENT_CLIPBOARD_UTF8_TEXT.to_le_bytes());
            }
        }
        VD_AGENT_CLIPBOARD => {
            if word(0) == Some(VD_AGENT_CLIPBOARD_UTF8_TEXT) {
                let text = String::from_utf8_lossy(&data[4..]).into_owned();
                log(&format!("host text, {} bytes", text.len()));
                *OFFERED.lock().unwrap() = Some(text.clone());
                set_text(hwnd, &text);
            }
        }
        VD_AGENT_CLIPBOARD_REQUEST => {
            if word(0) == Some(VD_AGENT_CLIPBOARD_UTF8_TEXT) {
                let text = OFFERED.lock().unwrap().clone().unwrap_or_default();
                let mut d = VD_AGENT_CLIPBOARD_UTF8_TEXT.to_le_bytes().to_vec();
                d.extend_from_slice(text.as_bytes());
                send(VD_AGENT_CLIPBOARD, &d);
                log(&format!("guest text sent, {} bytes", text.len()));
            }
        }
        VD_AGENT_CLIPBOARD_RELEASE => {}
        _ => {}
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        // a program that sets several formats changes the clipboard several
        // times in a row, and may still be finishing (OLE's flush) when the
        // first notice comes: read once things are quiet, not at once
        WM_CLIPBOARDUPDATE => {
            unsafe { SetTimer(hwnd, 1, 150, None) };
            0
        }
        WM_TIMER => {
            unsafe { KillTimer(hwnd, 1) };
            let seq = unsafe { GetClipboardSequenceNumber() };
            if seq != OWN_SEQ.load(Ordering::Acquire) {
                if let Some(text) = get_text(hwnd).filter(|t| !t.is_empty()) {
                    *OFFERED.lock().unwrap() = Some(text);
                    send(VD_AGENT_CLIPBOARD_GRAB, &VD_AGENT_CLIPBOARD_UTF8_TEXT.to_le_bytes());
                    log("guest clipboard changed; offered");
                }
            }
            0
        }
        WM_AGENT_UP => {
            caps(true);
            0
        }
        WM_AGENT_MSG => {
            let b = unsafe { Box::from_raw(lp as *mut (u32, Vec<u8>)) };
            on_message(hwnd, b.0, &b.1);
            0
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

/// `--map`: the shared folder on a drive letter, if the machine has one.
/// The network may come up after logon, so the server is given a minute.
fn map_share() {
    use windows_sys::Win32::NetworkManagement::WNet::*;
    let addr: std::net::SocketAddr = "10.0.2.4:445".parse().unwrap();
    let mut up = false;
    for _ in 0..30 {
        if std::net::TcpStream::connect_timeout(&addr, std::time::Duration::from_secs(2)).is_ok() {
            up = true;
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(2));
    }
    if !up {
        log("no shared folder (nothing answers at 10.0.2.4:445)");
        return;
    }
    // the first free letter from Z: down
    let used = unsafe { GetLogicalDrives() };
    let Some(letter) = (b'D'..=b'Z').rev().find(|l| used & (1 << (l - b'A')) == 0) else {
        log("no free drive letter for the shared folder");
        return;
    };
    let local = wide(&format!("{}:", letter as char));
    let remote = wide(SHARE);
    let user = wide(SHARE_USER);
    let pass = wide(SHARE_PASSWORD);
    let mut nr: NETRESOURCEW = unsafe { std::mem::zeroed() };
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = local.as_ptr() as *mut u16;
    nr.lpRemoteName = remote.as_ptr() as *mut u16;
    let r = unsafe { WNetAddConnection2W(&nr, pass.as_ptr(), user.as_ptr(), 0) };
    match r {
        0 => log(&format!("shared folder on {}:", letter as char)),
        // already connected (another session of this user's, a second run)
        85 | 1219 => log(&format!("shared folder already connected ({})", r)),
        e => log(&format!("mapping the shared folder failed: error {}", e)),
    }
}

/// The virtio-gpu's screen as the only one, once the desktop is up (track
/// M24; the user, 2026-10-09). A Windows 11 machine keeps a second card,
/// the standard VGA on x64 and ramfb on Arm, for setup (which has no
/// virtio driver) and recovery, and Windows extends the desktop onto both,
/// the other card's screen the primary: DWM paces on the primary, and the
/// player shows the virtio-gpu's. So when the virtio-gpu has a screen
/// Windows can drive (viogpudo working), every other screen goes, saved in
/// Windows' display database. That database keeps a layout per set of
/// connected screens: if the virtio-gpu's ever goes (a broken driver, a
/// recovery boot), Windows shows the other card's again by itself.
fn virtio_screen_only() {
    use windows_sys::Win32::Devices::Display::*;
    use windows_sys::Win32::Graphics::Gdi::{DISPLAYCONFIG_PATH_ACTIVE, DISPLAYCONFIG_PATH_MODE_IDX_INVALID};

    // the desktop is up once Explorer's taskbar is; then give it a moment
    let tray = wide("Shell_TrayWnd");
    let up = (0..120).any(|_| {
        let found = !unsafe { FindWindowW(tray.as_ptr(), std::ptr::null()) }.is_null();
        if !found {
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
        found
    });
    if !up {
        log("display: no taskbar after two minutes; the screens are left as they are");
        return;
    }
    std::thread::sleep(std::time::Duration::from_secs(3));

    let query = |flags| -> Option<(Vec<DISPLAYCONFIG_PATH_INFO>, Vec<DISPLAYCONFIG_MODE_INFO>)> {
        let (mut np, mut nm) = (0u32, 0u32);
        if unsafe { GetDisplayConfigBufferSizes(flags, &mut np, &mut nm) } != 0 {
            return None;
        }
        let mut paths = vec![unsafe { std::mem::zeroed::<DISPLAYCONFIG_PATH_INFO>() }; np as usize];
        let mut modes = vec![unsafe { std::mem::zeroed::<DISPLAYCONFIG_MODE_INFO>() }; nm as usize];
        let r = unsafe {
            QueryDisplayConfig(flags, &mut np, paths.as_mut_ptr(), &mut nm, modes.as_mut_ptr(), std::ptr::null_mut())
        };
        if r != 0 {
            return None;
        }
        paths.truncate(np as usize);
        modes.truncate(nm as usize);
        Some((paths, modes))
    };
    // virtio-gpu's PCI ID in the adapter's device path
    let virtio = |p: &DISPLAYCONFIG_PATH_INFO| {
        let mut name: DISPLAYCONFIG_ADAPTER_NAME = unsafe { std::mem::zeroed() };
        name.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        name.header.size = std::mem::size_of::<DISPLAYCONFIG_ADAPTER_NAME>() as u32;
        name.header.adapterId = p.targetInfo.adapterId;
        if unsafe { DisplayConfigGetDeviceInfo(&mut name.header) } != 0 {
            return false;
        }
        let len = name.adapterDevicePath.iter().position(|&c| c == 0).unwrap_or(128);
        String::from_utf16_lossy(&name.adapterDevicePath[..len]).to_ascii_uppercase().contains("VEN_1AF4&DEV_1050")
    };
    let flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE;

    let Some((mut paths, mut modes)) = query(QDC_ONLY_ACTIVE_PATHS) else {
        log("display: QueryDisplayConfig failed");
        return;
    };
    if paths.iter().any(|p| virtio(p)) {
        if paths.iter().all(|p| virtio(p)) {
            log("display: the virtio-gpu's screen is already the only one");
            return;
        }
        // only the virtio-gpu's paths and their modes, re-indexed (a path
        // left out goes off); its screen moves to the origin, the primary's
        // place
        paths.retain(|p| virtio(p));
        let mut keep = Vec::new();
        for p in paths.iter_mut() {
            for idx in [unsafe { &mut p.sourceInfo.Anonymous.modeInfoIdx }, unsafe { &mut p.targetInfo.Anonymous.modeInfoIdx }] {
                if let Some(m) = modes.get(*idx as usize) {
                    let mut m = *m;
                    if m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE {
                        m.Anonymous.sourceMode.position = POINTL { x: 0, y: 0 };
                    }
                    *idx = keep.len() as u32;
                    keep.push(m);
                }
            }
        }
        modes = keep;
        let r = unsafe { SetDisplayConfig(paths.len() as u32, paths.as_ptr(), modes.len() as u32, modes.as_ptr(), flags) };
        log(&format!("display: the virtio-gpu's screen made the only one ({})", r));
        return;
    }
    // not on yet: its first path to a screen Windows can drive, alone, the
    // mode left to Windows (the driver's preferred one, the window's size)
    let Some((all, _)) = query(QDC_ALL_PATHS) else {
        log("display: QueryDisplayConfig failed");
        return;
    };
    let Some(mut p) = all.into_iter().find(|p| p.targetInfo.targetAvailable != 0 && virtio(p)) else {
        log("display: no virtio-gpu screen (viogpudo not working?); the screens are left as they are");
        return;
    };
    p.flags = DISPLAYCONFIG_PATH_ACTIVE;
    p.sourceInfo.Anonymous.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    p.targetInfo.Anonymous.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    let r = unsafe { SetDisplayConfig(1, &p, 0, std::ptr::null(), flags) };
    log(&format!("display: the virtio-gpu's screen turned on as the only one ({})", r));
}

fn main() {
    let _ = std::fs::create_dir_all(r"C:\2KSBOX");
    *LOG.lock().unwrap() = std::fs::OpenOptions::new().create(true).append(true).open(r"C:\2KSBOX\agent.log").ok();
    log(&format!("2ksbox-agent {} starting", env!("CARGO_PKG_VERSION")));
    if std::env::args().any(|a| a == "--map") {
        map_share();
        return;
    }

    // one per session
    let mutex_name = wide(r"Local\2ksbox-agent");
    unsafe {
        CreateMutexW(std::ptr::null(), 1, mutex_name.as_ptr());
        if GetLastError() == ERROR_ALREADY_EXISTS {
            log("already running in this session");
            return;
        }
    }

    let class = wide("2ksbox-agent");
    let hwnd = unsafe {
        let hinst = GetModuleHandleW(std::ptr::null());
        let mut wc: WNDCLASSW = std::mem::zeroed();
        wc.lpfnWndProc = Some(wndproc);
        wc.hInstance = hinst;
        wc.lpszClassName = class.as_ptr();
        RegisterClassW(&wc);
        CreateWindowExW(0, class.as_ptr(), class.as_ptr(), 0, 0, 0, 0, 0, HWND_MESSAGE, std::ptr::null_mut(), hinst, std::ptr::null::<c_void>())
    };
    if hwnd.is_null() || unsafe { AddClipboardFormatListener(hwnd) } == 0 {
        log("no clipboard listener");
        return;
    }
    let hw = hwnd as usize;
    std::thread::spawn(move || reader(hw));
    std::thread::spawn(virtio_screen_only);
    unsafe {
        let mut m: MSG = std::mem::zeroed();
        while GetMessageW(&mut m, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
