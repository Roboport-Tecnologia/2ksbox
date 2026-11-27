//! A game's installer unpacked on the host, for a guest it does not run
//! on: GOG's installers are Inno Setup programs that need a recent
//! Windows, so on Windows 98, XP or DOS they never start. The shelf's
//! "Game installer…" reads the installer here, writes the game's files
//! into a folder beside the shelf, and puts that folder on the shelf,
//! where it goes in the drive as a disc (`isodir:`, M5g). Nothing runs:
//! the installer is only read.
//!
//! The rules:
//!
//! * **Only the game's own files.** An entry bound for `{app}` lands
//!   under the folder at its path below `{app}`. Every other root
//!   (`{tmp}`, `{sys}`, `{commonappdata}`, …) is the installer's own
//!   scaffolding or a change to the system, and is left out.
//! * **GOG's Galaxy layout is put back together.** A newer GOG installer
//!   stores each game file as zlib parts named after their MD5 under
//!   `{tmp}`, and names the file in the entries' `BeforeInstall`
//!   (`before_install('<md5>', '<path>', <parts>)`) and each part's sizes
//!   in `AfterInstall` (`after_install('<md5>', <packed>, <size>)`); its
//!   script joins them at install time. Here the parts are inflated in
//!   order into the named file, and the whole is checked against the
//!   MD5. `before_install_dependency` files are redistributables
//!   (DirectX, the Visual C++ runtime) for today's Windows, and are left
//!   out. innoextract's `--gog-galaxy` reads them the same way.
//! * **One file per path.** Where two entries name one path (a language
//!   or a 32/64-bit variant), the one for English and then the 32-bit one
//!   is kept, since the guest is a 32-bit Windows; otherwise the first.
//!   Paths are compared as Windows compares them, without case.
//! * **A path is used only if it stays inside the folder.** An entry
//!   with an empty, `.` or `..` element, a drive, a character Windows
//!   refuses in a name, or a Windows device name (`CON`, `COM1`) is
//!   skipped and counted. The installer is a
//!   download, and its paths are its author's.
//! * **The folder is new**, named after the game (`library::reserve_dir`),
//!   and removed if the unpacking fails, so a half-written game never
//!   reaches the shelf.
//! * **It runs on a thread**, since a game is gigabytes, and the front end
//!   polls it as it polls a clone (`Shelf::poll_unpack`).

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc};

use md5::{Digest, Md5};

/// The Add menu item, and its dialog's title and filter.
pub const MENU_ITEM: &str = "Game installer\u{2026}";
pub const PICK_TITLE: &str = "Unpack a game installer";
/// The player's Disc menu item, and what it shows while one is unpacked.
pub const INSERT_ITEM: &str = "Insert Game Installer\u{2026}";
pub const UNPACKING_ITEM: &str = "Unpacking Game Installer\u{2026}";
pub const FILTER: (&str, &[&str]) = ("Installers", &["exe"]);

/// What an installer holds, worked out before anything is written.
pub struct Plan {
    /// The game's name as the installer gives it, for the folder and the
    /// shelf's label.
    pub name: String,
    files: Vec<Planned>,
    /// Bytes the files come to once unpacked.
    pub total: u64,
    /// Entries left out because their path would leave the folder.
    pub unsafe_paths: usize,
}

struct Planned {
    /// Below the folder, one element per entry.
    path: Vec<String>,
    /// Entry indices whose data, in order, make up the file.
    parts: Vec<usize>,
    /// A Galaxy part's inflated size as `after_install` declares it, one
    /// per part: what its zlib stream may come to, and must.
    part_sizes: Vec<u64>,
    /// A Galaxy file, whose parts are zlib streams.
    galaxy: bool,
    /// GOG's MD5 of the whole file, when it names one.
    md5: Option<[u8; 16]>,
    size: u64,
    rank: u8,
}

impl Plan {
    pub fn files(&self) -> usize {
        self.files.len()
    }

    /// Each file's size and path below the folder, for `--unpack-installer
    /// --plan`.
    pub fn listing(&self) -> Vec<(u64, String)> {
        self.files.iter().map(|f| (f.size, f.path.join("\\"))).collect()
    }
}

/// Read the installer's records and decide what to unpack.
pub fn plan(installer: &Path) -> Result<Plan, String> {
    let inno = open(installer)?;
    let name = inno
        .header()
        .app_name()
        // Control characters out: the name becomes a shelf label, and the
        // flat file the guest's CDSHELF reads ends at a NUL.
        .map(|n| n.chars().filter(|c| !c.is_control()).collect::<String>().trim().to_string())
        .filter(|n| !n.is_empty())
        .unwrap_or_else(|| crate::disc_library::default_label(installer));
    let entries = inno.file_entries();
    let locations = inno.file_locations();
    let size_of = |index: usize| {
        let location = entries[index].location();
        locations.get(location as usize).map_or(0, |l| l.uncompressed_size())
    };
    let mut files: Vec<Planned> = Vec::new();
    let mut unsafe_paths = 0;
    // The Galaxy file whose parts are still arriving, and how many more.
    let mut open_galaxy: Option<(usize, usize)> = None;
    for (index, entry) in entries.iter().enumerate() {
        let condition = entry.condition();
        let before = condition.before_install().unwrap_or_default();
        let after = condition.after_install().unwrap_or_default();
        let rank = rank(condition.check().unwrap_or_default());
        if let Some(args) = call(before, "before_install") {
            // A new Galaxy file: its MD5, its path, how many parts.
            open_galaxy = None;
            let parts = args.get(2).and_then(|n| n.parse::<usize>().ok()).unwrap_or(1).max(1);
            match args.get(1).and_then(|p| safe_path(p)) {
                Some(path) => {
                    files.push(Planned {
                        path,
                        parts: Vec::new(),
                        part_sizes: Vec::new(),
                        galaxy: true,
                        md5: parse_md5(&args[0]),
                        size: 0,
                        rank,
                    });
                    open_galaxy = Some((files.len() - 1, parts));
                }
                None => unsafe_paths += 1,
            }
        } else if call(before, "before_install_dependency").is_some() {
            open_galaxy = None;
        }
        if let Some(args) = call(after, "after_install") {
            if let Some((file, left)) = open_galaxy.as_mut() {
                let planned = &mut files[*file];
                let size = args.get(2).and_then(|n| n.parse::<u64>().ok()).unwrap_or(0);
                planned.parts.push(index);
                planned.part_sizes.push(size);
                planned.size += size;
                *left -= 1;
                if *left == 0 {
                    open_galaxy = None;
                }
            }
            continue;
        }
        if !before.is_empty() || !after.is_empty() {
            // A dependency's part, or a Galaxy entry this reading does not
            // know.
            continue;
        }
        let Some(rest) = entry.destination().and_then(below_app) else { continue };
        if locations.get(entry.location() as usize).is_none() {
            continue;
        }
        match safe_path(rest) {
            Some(path) => files.push(Planned {
                path,
                parts: vec![index],
                part_sizes: Vec::new(),
                galaxy: false,
                md5: None,
                size: size_of(index),
                rank,
            }),
            None => unsafe_paths += 1,
        }
    }
    // A Galaxy file missing parts is the installer's fault or this
    // reading's; either way it cannot be put together.
    files.retain(|f| !f.galaxy || !f.parts.is_empty());
    let files = one_per_path(files);
    let total = files.iter().map(|f| f.size).sum();
    Ok(Plan { name, files, total, unsafe_paths })
}

fn open(installer: &Path) -> Result<inno::Inno<io::BufReader<fs::File>>, String> {
    inno::Inno::open(installer).map_err(|e| match e {
        inno::error::InnoError::NotInnoFile => "This is not an installer this app can unpack.".to_string(),
        other => format!("Could not read the installer: {other}"),
    })
}

/// Where two entries name one path, keep the better ranked, the first on
/// a tie.
fn one_per_path(files: Vec<Planned>) -> Vec<Planned> {
    let mut best: HashMap<String, usize> = HashMap::new();
    for (i, file) in files.iter().enumerate() {
        let key = file.path.join("\\").to_lowercase();
        match best.get(&key) {
            Some(&j) if files[j].rank >= file.rank => {}
            _ => {
                best.insert(key, i);
            }
        }
    }
    let keep: HashSet<usize> = best.into_values().collect();
    files.into_iter().enumerate().filter(|(i, _)| keep.contains(i)).map(|(_, f)| f).collect()
}

/// How well an entry suits the guest, from GOG's
/// `check_if_install('<languages>', '<bits>', '<os>')`: English first,
/// then 32-bit.
fn rank(check: &str) -> u8 {
    let Some(args) = call(check, "check_if_install") else { return 3 };
    let listed = |field: Option<&String>, want: &str| {
        field.is_none_or(|f| f.is_empty() || f.split('#').any(|t| t.trim().eq_ignore_ascii_case(want)))
    };
    let english =
        args.first().is_none_or(|langs| langs.is_empty() || langs.split('#').any(|l| l.trim().starts_with("en")));
    u8::from(english) * 2 + u8::from(listed(args.get(1), "32"))
}

/// The path below `{app}\`, for an entry bound there.
fn below_app(destination: &str) -> Option<&str> {
    let rest = destination.strip_prefix("{app}")?;
    let rest = rest.trim_start_matches(['\\', '/']);
    (!rest.is_empty()).then_some(rest)
}

/// A path's elements, if every one is a plain name Windows accepts.
fn safe_path(path: &str) -> Option<Vec<String>> {
    let parts: Vec<String> = path.split(['\\', '/']).map(str::to_string).collect();
    let plain = |p: &String| {
        !p.is_empty()
            && p != "."
            && p != ".."
            && !p.ends_with([' ', '.'])
            && !p.chars().any(|c| c < ' ' || "<>:\"|?*".contains(c))
            && !device_name(p)
    };
    parts.iter().all(plain).then_some(parts)
}

/// A name Windows takes as a device whatever its extension (`CON`,
/// `nul.txt`, `COM1`): on a Windows host, creating it would open the
/// device, not a file in the folder.
fn device_name(name: &str) -> bool {
    let stem = name.split('.').next().unwrap_or(name).trim_end().to_ascii_uppercase();
    if matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$") {
        return true;
    }
    // COM and LPT with one digit, the superscripts Windows also takes
    // (`COM¹`) and the 0 Microsoft lists as reserved.
    let mut chars = stem.chars();
    let head: String = chars.by_ref().take(3).collect();
    matches!(head.as_str(), "COM" | "LPT")
        && matches!((chars.next(), chars.next()), (Some('0'..='9' | '¹' | '²' | '³'), None))
}

fn parse_md5(text: &str) -> Option<[u8; 16]> {
    if text.len() != 32 {
        return None;
    }
    let mut out = [0u8; 16];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(text.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(out)
}

/// The arguments of `code` if it is a call to `name`, as innoextract's
/// `parse_function_call` reads them: quoted strings (`''` an escaped
/// quote) or bare tokens, comma separated.
fn call(code: &str, name: &str) -> Option<Vec<String>> {
    let code = code.trim().trim_end_matches(';').trim_end();
    let rest = code.strip_prefix(name)?.trim_start();
    let inner = rest.strip_prefix('(')?.strip_suffix(')')?;
    let mut args = Vec::new();
    let mut chars = inner.chars().peekable();
    loop {
        while chars.peek().is_some_and(|c| c.is_whitespace()) {
            chars.next();
        }
        let mut arg = String::new();
        if chars.peek() == Some(&'\'') {
            chars.next();
            loop {
                match chars.next()? {
                    '\'' if chars.peek() == Some(&'\'') => {
                        chars.next();
                        arg.push('\'');
                    }
                    '\'' => break,
                    c => arg.push(c),
                }
            }
        } else {
            while let Some(&c) = chars.peek() {
                if c == ',' {
                    break;
                }
                arg.push(c);
                chars.next();
            }
            arg = arg.trim().to_string();
        }
        args.push(arg);
        while chars.peek().is_some_and(|c| c.is_whitespace()) {
            chars.next();
        }
        match chars.next() {
            None => return Some(args),
            Some(',') => {}
            Some(_) => return None,
        }
    }
}

/// Write the plan's files under `dest`, which must exist and be empty.
/// `done` counts the bytes written, for a progress bar.
pub fn unpack(installer: &Path, plan: &Plan, dest: &Path, done: &AtomicU64) -> Result<(), String> {
    let mut inno = open(installer)?;
    // Which file, and which of its parts, each entry is.
    let mut wanted: HashMap<usize, (usize, usize)> = HashMap::new();
    for (f, file) in plan.files.iter().enumerate() {
        for (p, &entry) in file.parts.iter().enumerate() {
            wanted.insert(entry, (f, p));
        }
    }
    let mut current: Option<(usize, fs::File, Md5)> = None;
    let mut stream = inno.streaming_files(|e| wanted.contains_key(&e.index()));
    while let Some(next) = stream.next() {
        let (entry, reader) = next.map_err(|e| format!("Could not read the installer: {e}"))?;
        let Some(&(f, part)) = wanted.get(&entry.index()) else { continue };
        let file = &plan.files[f];
        let target = file.path.iter().fold(dest.to_path_buf(), |p, e| p.join(e));
        if part == 0 {
            finish(current.take(), plan)?;
            if let Some(dir) = target.parent() {
                fs::create_dir_all(dir).map_err(|e| write_error(&target, e))?;
            }
            let out = fs::File::create(&target).map_err(|e| write_error(&target, e))?;
            current = Some((f, out, Md5::new()));
        }
        let Some((cf, out, hash)) = current.as_mut() else { continue };
        if *cf != f {
            // A part out of order: the file it belongs to was closed.
            return Err(format!("The installer's parts of {} are out of order.", file.path.join("\\")));
        }
        let mut sink = Sink { out, hash, done, path: &target };
        let unpack_error = |e: io::Error| format!("Could not unpack {}: {e}", file.path.join("\\"));
        if file.galaxy {
            // A part inflates to the size its entry declares, and no
            // further: a stream that would run on is refused at one byte
            // past it, never written out to fill the disk.
            let declared = file.part_sizes[part];
            let mut inflated = io::Read::take(flate2::read::ZlibDecoder::new(reader), declared + 1);
            let copied = io::copy(&mut inflated, &mut sink).map_err(unpack_error)?;
            if copied != declared {
                return Err(format!("{} came out damaged. The download may be incomplete.", file.path.join("\\")));
            }
        } else {
            // The same for a plain entry, at the size the installer records.
            let recorded = entry.file_location().uncompressed_size();
            let copied = io::copy(&mut io::Read::take(reader, recorded + 1), &mut sink).map_err(unpack_error)?;
            if copied != recorded {
                return Err(format!("{} came out damaged. The download may be incomplete.", file.path.join("\\")));
            }
        }
        if part + 1 == file.parts.len() {
            // The installer's own date, which a DOS directory listing
            // shows; one before 1970 is no date at all.
            let time = std::time::SystemTime::from(entry.file_location().file_time());
            if time > std::time::UNIX_EPOCH {
                let _ = out.set_modified(time);
            }
            finish(current.take(), plan)?;
        }
    }
    finish(current.take(), plan)
}

/// Close a file, checking a Galaxy file against its MD5.
fn finish(current: Option<(usize, fs::File, Md5)>, plan: &Plan) -> Result<(), String> {
    let Some((f, out, hash)) = current else { return Ok(()) };
    drop(out);
    let file = &plan.files[f];
    if let Some(want) = file.md5 {
        if hash.finalize().as_slice() != want {
            return Err(format!("{} came out damaged. The download may be incomplete.", file.path.join("\\")));
        }
    }
    Ok(())
}

fn write_error(path: &Path, e: io::Error) -> String {
    format!("Could not write {}: {e}", path.display())
}

struct Sink<'a> {
    out: &'a mut fs::File,
    hash: &'a mut Md5,
    done: &'a AtomicU64,
    path: &'a Path,
}

impl Write for Sink<'_> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let n = self.out.write(buf).map_err(|e| io::Error::other(write_error(self.path, e)))?;
        self.hash.update(&buf[..n]);
        self.done.fetch_add(n as u64, Ordering::Relaxed);
        Ok(n)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.out.flush()
    }
}

/// An unpacking under way.
pub struct Job {
    pub installer: PathBuf,
    pub name: String,
    pub dest: PathBuf,
    pub total: u64,
    done: Arc<AtomicU64>,
    result: mpsc::Receiver<Result<(), String>>,
}

impl Job {
    /// Plan the installer and start writing it into a new folder named
    /// after the game under `parent`.
    pub fn start(installer: &Path, parent: &Path) -> Result<Job, String> {
        let plan = plan(installer)?;
        if plan.files.is_empty() {
            return Err("The installer has no game files this app can unpack.".into());
        }
        let dest = crate::library::reserve_dir(parent, &plan.name).map_err(|e| write_error(parent, e))?;
        let done = Arc::new(AtomicU64::new(0));
        let (tx, rx) = mpsc::channel();
        let (thread_installer, thread_dest, counter) = (installer.to_path_buf(), dest.clone(), done.clone());
        let (name, total) = (plan.name.clone(), plan.total);
        std::thread::spawn(move || {
            let result = unpack(&thread_installer, &plan, &thread_dest, &counter);
            if result.is_err() {
                let _ = fs::remove_dir_all(&thread_dest);
            }
            let _ = tx.send(result);
        });
        Ok(Job { installer: installer.to_path_buf(), name, dest, total, done, result: rx })
    }

    /// Written so far and in all.
    pub fn progress(&self) -> (u64, u64) {
        (self.done.load(Ordering::Relaxed).min(self.total), self.total)
    }

    /// `None` while it runs, then its outcome, once.
    pub fn poll(&self) -> Option<Result<(), String>> {
        match self.result.try_recv() {
            Ok(result) => Some(result),
            Err(mpsc::TryRecvError::Empty) => None,
            Err(mpsc::TryRecvError::Disconnected) => {
                let _ = fs::remove_dir_all(&self.dest);
                Some(Err("The unpacking stopped unexpectedly.".into()))
            }
        }
    }

    /// Block until it ends, for a caller with no timer (`cli`).
    pub fn wait(&self) -> Result<(), String> {
        match self.result.recv() {
            Ok(result) => result,
            Err(_) => Err("The unpacking stopped unexpectedly.".into()),
        }
    }
}

/// The CD images inside an unpacked game, each with a shelf label: a GOG
/// release of a CD game keeps its disc as a cue sheet (`game.ins` or a
/// `.cue`) beside a `.gog` / `.bin` and `.ogg` music tracks, for DOSBox,
/// and the game wants that disc in the drive, not the folder. A file
/// counts when its name says cue sheet and its text has `FILE`, `TRACK`
/// and `INDEX`, as libdisc's own sniff decides. Searched four levels down.
pub fn disc_images(dir: &Path, game: &str) -> Vec<(PathBuf, String)> {
    let mut found = Vec::new();
    let mut stack = vec![(dir.to_path_buf(), 0)];
    while let Some((at, depth)) = stack.pop() {
        let Ok(entries) = fs::read_dir(&at) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(kind) = entry.file_type() else { continue };
            if kind.is_dir() && depth < 4 {
                stack.push((path, depth + 1));
            } else if kind.is_file() && is_cue_sheet(&path) {
                found.push(path);
            }
        }
    }
    found.sort();
    let one = found.len() == 1;
    found
        .into_iter()
        .map(|p| {
            let stem = p.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            let label = if one { format!("{game} (CD)") } else { format!("{game} ({stem})") };
            (p, label)
        })
        .collect()
}

fn is_cue_sheet(path: &Path) -> bool {
    let named = path
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| ["cue", "ins", "inst"].iter().any(|x| x.eq_ignore_ascii_case(e)));
    if !named {
        return false;
    }
    let mut head = vec![0u8; 4096];
    let n = fs::File::open(path).and_then(|mut f| io::Read::read(&mut f, &mut head)).unwrap_or(0);
    let text = String::from_utf8_lossy(&head[..n]).to_ascii_uppercase();
    text.contains("FILE ") && text.contains("TRACK ") && text.contains("INDEX ")
}

/// Where unpacked games go: `games/` beside the shelf's file.
pub fn games_dir(shelf_file: &Path) -> PathBuf {
    shelf_file.parent().unwrap_or(Path::new(".")).join("games")
}

/// What a file name's extension says about whether it is worth offering
/// to unpack, for a dropped file.
pub fn looks_like_installer(path: &Path) -> bool {
    path.extension().is_some_and(|e| e.eq_ignore_ascii_case("exe"))
}
