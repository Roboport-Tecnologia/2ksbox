//! What a sandboxed macOS launcher may still open after a restart.
//!
//! The App Store build is sandboxed (`docs/build-macos.md`, "The App
//! Store package"): it reaches its own container, and outside it only
//! what the user picked in an open panel, and only until it quits. A
//! machine keeps its disks, install ISO and discs by path, so without
//! more, every file outside the container is gone at the next launch.
//!
//! The more is a security-scoped bookmark per pick: `keep` writes one for
//! every path a dialog hands back (`browse::remember` calls it), into
//! `<data dir>/grants/`, and `restore`, first thing in every launcher
//! run, resolves each and starts accessing it for the life of the
//! process. The players and `qemu-img` the launcher spawns inherit its
//! sandbox, the access included.
//!
//! The panel grants exactly what was picked: a folder grants everything
//! under it, a file only that file. A `.cue` picked alone leaves its
//! `.bin` tracks closed, and a qcow2 overlay its backing disk. So after
//! each pick the front end asks `ask_after_pick` whether the file reads
//! others the sandbox refuses (`companions`), and if so shows a folder
//! dialog at their folder with `Ask::message` as its prompt: one click
//! on Open grants the folder, which is kept like any pick.
//!
//! Outside the sandbox nothing here does anything: `keep` returns at
//! once, and an unsandboxed process can open any file anyway.
//! `launcher --grants` lists and resolves the kept ones (`cli`).

use std::path::{Path, PathBuf};

/// `<data dir>/grants`, or `None` when there is no data directory.
pub fn dir() -> Option<PathBuf> {
    crate::paths::data_dir().map(|d| d.join("grants"))
}

/// Whether this process runs in the App Sandbox. macOS sets this for
/// every sandboxed process, the inheriting children included.
pub fn sandboxed() -> bool {
    cfg!(target_os = "macos") && std::env::var_os("APP_SANDBOX_CONTAINER_ID").is_some()
}

/// A bookmark's file name: the path's FNV-1a hash, so picking the same
/// path again replaces its bookmark instead of adding one.
fn file_name(path: &Path) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for b in path.as_os_str().as_encoded_bytes() {
        h = (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3);
    }
    format!("{h:016x}.bookmark")
}

/// Keep access to `path`, which a dialog just granted, for later runs. A
/// failure is a warning: the file still opens in this run.
pub fn keep(path: &Path) {
    if !sandboxed() {
        return;
    }
    if let Err(e) = bookmark_now(path) {
        eprintln!("launcher: cannot keep access to {} after a restart: {e}", path.display());
    }
}

/// `keep` without the sandbox test: `cli`'s `--keep-grant`.
pub fn bookmark_now(path: &Path) -> Result<(), String> {
    let dir = dir().ok_or("no data directory")?;
    let data = mac::bookmark(path)?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    std::fs::write(dir.join(file_name(path)), data).map_err(|e| e.to_string())
}

/// The files `path` reads besides itself: a cue sheet's `FILE`s, a
/// CloneCD image's `.img` and `.sub`, an Alcohol image's `.mdf`, and a
/// qcow2's backing chain. Named by the descriptor, not looked for, since
/// a sandboxed process may not list their folder; libdisc's lookup
/// (`resolve_payload`) then finds a differently cased name once the
/// folder is open. Empty for anything else.
pub fn companions(path: &Path) -> Vec<PathBuf> {
    let dir = path.parent().unwrap_or(Path::new("."));
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let ext = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    match ext.as_str() {
        "cue" => std::fs::read(path)
            .map(|bytes| String::from_utf8_lossy(&bytes).lines().filter_map(cue_file).map(|n| tidy(&dir.join(n))).collect())
            .unwrap_or_default(),
        "ccd" => vec![dir.join(format!("{stem}.img")), dir.join(format!("{stem}.sub"))],
        "mds" => vec![dir.join(format!("{stem}.mdf"))],
        _ => {
            let mut chain = Vec::new();
            let mut at = path.to_path_buf();
            // A chain longer than this is a loop.
            while let Some(backing) = qcow2_backing(&at).filter(|_| chain.len() < 16) {
                chain.push(backing.clone());
                at = backing;
            }
            chain
        }
    }
}

/// A cue sheet line's `FILE` name (quoted or not), with DOS separators.
fn cue_file(line: &str) -> Option<String> {
    let rest = line.trim_start();
    if !rest.get(..5)?.eq_ignore_ascii_case("FILE ") {
        return None;
    }
    let rest = rest[5..].trim_start();
    let name = match rest.strip_prefix('"') {
        Some(quoted) => &quoted[..quoted.find('"')?],
        None => rest.split_whitespace().next()?,
    };
    Some(name.replace('\\', "/"))
}

/// A qcow2's backing file, from its header (offset 8: the name's offset,
/// 16: its length), relative to the image's folder. `None` for anything
/// that is not a qcow2 or has no backing file.
fn qcow2_backing(path: &Path) -> Option<PathBuf> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).ok()?;
    let mut head = [0u8; 20];
    f.read_exact(&mut head).ok()?;
    if &head[..4] != b"QFI\xfb" {
        return None;
    }
    let offset = u64::from_be_bytes(head[8..16].try_into().ok()?);
    let len = u32::from_be_bytes(head[16..20].try_into().ok()?) as usize;
    if offset == 0 || len == 0 || len > 1023 {
        return None;
    }
    f.seek(SeekFrom::Start(offset)).ok()?;
    let mut name = vec![0u8; len];
    f.read_exact(&mut name).ok()?;
    let name = PathBuf::from(String::from_utf8(name).ok()?);
    Some(tidy(&path.parent().unwrap_or(Path::new(".")).join(name)))
}

/// `a/b/../c` as `a/c`, without touching the disk (a sandboxed process
/// may not resolve a path it cannot read), so the dialog opens on a
/// folder whose name the user recognises.
fn tidy(path: &Path) -> PathBuf {
    use std::path::Component;
    let mut out = PathBuf::new();
    for c in path.components() {
        match c {
            Component::CurDir => {}
            Component::ParentDir if matches!(out.components().next_back(), Some(Component::Normal(_))) => {
                out.pop();
            }
            c => out.push(c),
        }
    }
    out
}

/// The folder dialog to show after a pick, and what it says.
pub struct Ask {
    /// Where it opens; picking it as it is grants the companions.
    pub folder: PathBuf,
    /// The dialog's prompt (on macOS its message line).
    pub message: String,
}

/// What to ask for after `picked` was picked, in the sandbox only: the
/// folder of the first of its `companions` the sandbox refuses. Ask again
/// after a grant, since a cue sheet may name tracks in two folders; the
/// front end stops when the user cancels or the same folder comes back.
pub fn ask_after_pick(picked: &Path) -> Option<Ask> {
    if sandboxed() { ask_for(picked) } else { None }
}

/// `ask_after_pick` without the sandbox test (`cli`'s `--grant-ask`).
/// Only a refusal asks: a file that is not there is libdisc's or QEMU's
/// error to give, and no folder grant would fix it.
pub fn ask_for(picked: &Path) -> Option<Ask> {
    let refused = companions(picked).into_iter().find(|c| {
        std::fs::File::open(c).is_err_and(|e| e.kind() == std::io::ErrorKind::PermissionDenied)
    })?;
    let folder = refused.parent()?.to_path_buf();
    let name = picked.file_name()?.to_string_lossy().into_owned();
    let what = match picked.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).as_deref() {
        Some("cue") => "its tracks",
        Some("ccd" | "mds") => "its data files",
        _ => "the disk it is based on",
    };
    Some(Ask { folder, message: format!("{name} needs {what}. Click Open to let 2ksbox read this folder.") })
}

/// One kept bookmark, resolved (`restore`).
pub struct Grant {
    pub bookmark: PathBuf,
    /// The path it resolved to, or why it did not.
    pub path: Result<PathBuf, String>,
    /// Whether access started; false for a path that did not resolve.
    pub open: bool,
}

/// Resolve every kept bookmark and start accessing it until the process
/// ends. A stale one (the file moved or was renamed) is rewritten from
/// where it resolved to. One that does not resolve (a deleted file, an
/// unplugged drive) is left on disk for when it comes back.
pub fn restore() -> Vec<Grant> {
    let Some(dir) = dir() else { return Vec::new() };
    let Ok(entries) = std::fs::read_dir(&dir) else { return Vec::new() };
    let mut out = Vec::new();
    for entry in entries.flatten() {
        let bookmark = entry.path();
        if bookmark.extension().is_none_or(|e| e != "bookmark") {
            continue;
        }
        let grant = match std::fs::read(&bookmark).map_err(|e| e.to_string()).and_then(|d| mac::resolve(&d)) {
            Ok((path, open, stale)) => {
                if stale {
                    if let Ok(data) = mac::bookmark(&path) {
                        let _ = std::fs::remove_file(&bookmark);
                        let _ = std::fs::write(dir.join(file_name(&path)), data);
                    }
                }
                Grant { bookmark, path: Ok(path), open }
            }
            Err(e) => Grant { bookmark, path: Err(e), open: false },
        };
        out.push(grant);
    }
    out
}

#[cfg(target_os = "macos")]
mod mac {
    //! CoreFoundation's half of `NSURL`'s bookmarks, called directly.
    use std::ffi::c_void;
    use std::os::unix::ffi::OsStrExt;
    use std::path::{Path, PathBuf};
    use std::ptr::null;

    type CFRef = *const c_void;
    const WITH_SECURITY_SCOPE_CREATE: usize = 1 << 11; // kCFURLBookmarkCreationWithSecurityScope
    const WITH_SECURITY_SCOPE_RESOLVE: usize = 1 << 10; // kCFURLBookmarkResolutionWithSecurityScope

    #[link(name = "CoreFoundation", kind = "framework")]
    unsafe extern "C" {
        fn CFURLCreateFromFileSystemRepresentation(a: CFRef, buf: *const u8, len: isize, dir: u8) -> CFRef;
        fn CFURLCreateBookmarkData(a: CFRef, url: CFRef, opts: usize, keys: CFRef, rel: CFRef, err: *mut CFRef) -> CFRef;
        fn CFURLCreateByResolvingBookmarkData(
            a: CFRef, data: CFRef, opts: usize, rel: CFRef, keys: CFRef, stale: *mut u8, err: *mut CFRef,
        ) -> CFRef;
        fn CFURLStartAccessingSecurityScopedResource(url: CFRef) -> u8;
        fn CFURLGetFileSystemRepresentation(url: CFRef, base: u8, buf: *mut u8, max: isize) -> u8;
        fn CFDataCreate(a: CFRef, bytes: *const u8, len: isize) -> CFRef;
        fn CFDataGetLength(d: CFRef) -> isize;
        fn CFDataGetBytePtr(d: CFRef) -> *const u8;
        fn CFErrorGetCode(e: CFRef) -> isize;
        fn CFRelease(r: CFRef);
    }

    /// The `CFError` an out-parameter came back with, as text, released.
    fn error(what: &str, err: CFRef) -> String {
        if err.is_null() {
            return format!("{what} failed");
        }
        // SAFETY: a non-null error from a Create call is ours to release.
        let code = unsafe { CFErrorGetCode(err) };
        unsafe { CFRelease(err) };
        format!("{what} failed (Cocoa error {code})")
    }

    pub fn bookmark(path: &Path) -> Result<Vec<u8>, String> {
        let bytes = path.as_os_str().as_bytes();
        // SAFETY: the buffer is `bytes.len()` long; every Create result
        // is checked for null and released once.
        unsafe {
            let url = CFURLCreateFromFileSystemRepresentation(null(), bytes.as_ptr(), bytes.len() as isize, path.is_dir() as u8);
            if url.is_null() {
                return Err("not a file URL".into());
            }
            let mut err: CFRef = null();
            let data = CFURLCreateBookmarkData(null(), url, WITH_SECURITY_SCOPE_CREATE, null(), null(), &mut err);
            CFRelease(url);
            if data.is_null() {
                return Err(error("the bookmark", err));
            }
            let out = std::slice::from_raw_parts(CFDataGetBytePtr(data), CFDataGetLength(data) as usize).to_vec();
            CFRelease(data);
            Ok(out)
        }
    }

    /// The path, whether access started, and whether the bookmark is stale.
    pub fn resolve(bookmark: &[u8]) -> Result<(PathBuf, bool, bool), String> {
        // SAFETY: as in `bookmark`; the path buffer is as long as it says.
        unsafe {
            let data = CFDataCreate(null(), bookmark.as_ptr(), bookmark.len() as isize);
            if data.is_null() {
                return Err("out of memory".into());
            }
            let (mut stale, mut err): (u8, CFRef) = (0, null());
            let url = CFURLCreateByResolvingBookmarkData(
                null(), data, WITH_SECURITY_SCOPE_RESOLVE, null(), null(), &mut stale, &mut err,
            );
            CFRelease(data);
            if url.is_null() {
                return Err(error("resolving", err));
            }
            let mut buf = vec![0u8; 4096];
            let ok = CFURLGetFileSystemRepresentation(url, 1, buf.as_mut_ptr(), buf.len() as isize) != 0;
            // Never stopped: the access lasts as long as the process.
            let open = CFURLStartAccessingSecurityScopedResource(url) != 0;
            CFRelease(url);
            if !ok {
                return Err("no file system path".into());
            }
            let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
            Ok((PathBuf::from(std::ffi::OsStr::from_bytes(&buf[..len])), open, stale != 0))
        }
    }
}

#[cfg(not(target_os = "macos"))]
mod mac {
    use std::path::{Path, PathBuf};

    pub fn bookmark(_path: &Path) -> Result<Vec<u8>, String> {
        Err("bookmarks are macOS's".into())
    }

    pub fn resolve(_bookmark: &[u8]) -> Result<(PathBuf, bool, bool), String> {
        Err("bookmarks are macOS's".into())
    }
}
